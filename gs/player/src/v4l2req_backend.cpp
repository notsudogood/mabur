#include "v4l2req_backend.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/opt.h>
}

#include "frame_handoff.h"
#include "mabur/hevc_params.h"
#include "prime_frame.h"

// AV_HWDEVICE_TYPE_V4L2REQUEST is not in upstream FFmpeg: it is an enumerator
// the v4l2request fork adds to AVHWDeviceType, so this file builds only
// against the fork's headers (the Orange Pi image's ffmpeg-v4l2request).

#ifndef AV_PKT_FLAG_PARTIAL
#error "libavcodec lacks the mabur stream patch (gs/player/ffmpeg-patches)"
#endif

namespace maburplay {

namespace {

// Extra buffers the hwaccel's DRM-PRIME frame pool (sized at avcodec_open2)
// preallocates beyond the decoder's own references: on screen + pending flip + mailbox
// (DrmPresenter), the regulator's held frame, and slack for a frame in the
// ready queue. Not a ceiling -- the pool is an av_buffer_pool that grows on
// demand past this if the handoff backs up further.
constexpr int kExtraHwFrames = 8;

// One AUD NAL: no slice, so libav ends an open streamed picture with the
// slices it has and the hwaccel releases the held capture buffer.
constexpr uint8_t kAud[] = {0x00, 0x00, 0x01, 0x46, 0x01, 0x50};

enum AVPixelFormat pick_drm_prime(AVCodecContext*, const enum AVPixelFormat* fmts) {
  for (; *fmts != AV_PIX_FMT_NONE; ++fmts)
    if (*fmts == AV_PIX_FMT_DRM_PRIME) return *fmts;
  return AV_PIX_FMT_NONE;
}

PrimeDesc to_prime_desc(const AVDRMFrameDescriptor& d) {
  PrimeDesc p;
  p.nb_objects = d.nb_objects;
  for (int i = 0; i < d.nb_objects && i < 4; ++i)
    p.objects[i] = {d.objects[i].fd, d.objects[i].format_modifier};
  p.nb_layers = d.nb_layers;
  if (d.nb_layers > 0) {
    p.layer_format = d.layers[0].format;
    p.nb_planes = d.layers[0].nb_planes;
    for (int i = 0; i < d.layers[0].nb_planes && i < 4; ++i)
      p.planes[i] = {d.layers[0].planes[i].object_index,
                     static_cast<long>(d.layers[0].planes[i].offset),
                     static_cast<long>(d.layers[0].planes[i].pitch)};
  }
  return p;
}

}  // namespace

struct V4l2ReqBackend::Impl {
  FrameSink sink;
  AVCodecContext* cc = nullptr;
  AVBufferRef* hw = nullptr;
  AVPacket* pkt = nullptr;
  FrameHandoff<DmaFrame> handoff;
  EntryGate gate;
  std::atomic<uint64_t> frames{0}, errors{0}, bitstream{0}, dropped_shape{0}, gated{0};
  std::atomic<int> poll_timeout_ms{0};  // PollWait; read by poll(), written from any thread
  bool logged_shape = false;
  bool stream_open = false;
  bool logged_stream_error = false;
  bool logged_reopen = false;

  ~Impl() {
    free_all();
    av_packet_free(&pkt);
    avcodec_free_context(&cc);
    av_buffer_unref(&hw);
  }

  static void free_frame(const DmaFrame& f) {
    AVFrame* av = static_cast<AVFrame*>(f.opaque);
    av_frame_free(&av);
  }

  void free_releases() {
    for (const DmaFrame& f : handoff.take_releases()) free_frame(f);
  }

  void free_all() {
    for (const DmaFrame& f : handoff.drain_all()) free_frame(f);
  }

  bool open() {
    const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!dec) {
      std::fprintf(stderr, "V4l2ReqBackend: no HEVC decoder in libavcodec\n");
      return false;
    }
    cc = avcodec_alloc_context3(dec);
    pkt = av_packet_alloc();
    if (!cc || !pkt) return false;
    // In-band VPS/SPS/PPS, no B-frames: emit each picture as it completes.
    cc->flags |= AV_CODEC_FLAG_LOW_DELAY;
    cc->thread_count = 1;
    // Streamed decode needs mabur's FFmpeg patch; there is no fallback.
    if (av_opt_set_int(cc->priv_data, "stream_slices", 1, 0) < 0) {
      std::fprintf(stderr, "V4l2ReqBackend: libavcodec lacks the mabur stream patch\n");
      return false;
    }
    const int hr = av_hwdevice_ctx_create(&hw, AV_HWDEVICE_TYPE_V4L2REQUEST, nullptr, nullptr, 0);
    if (hr < 0) {
      std::fprintf(stderr, "V4l2ReqBackend: v4l2request device create failed (%d)\n", hr);
      return false;
    }
    cc->hw_device_ctx = av_buffer_ref(hw);
    cc->get_format = pick_drm_prime;
    cc->extra_hw_frames = kExtraHwFrames;
    const int orc = avcodec_open2(cc, dec, nullptr);
    if (orc < 0) {
      std::fprintf(stderr, "V4l2ReqBackend: avcodec_open2 failed (%d)\n", orc);
      return false;
    }
    return true;
  }

  // One packet to the decoder. false = refused; a refused packet also ends
  // any streamed picture on libav's side, so stream_open follows it.
  bool send(const uint8_t* p, size_t n, uint32_t pts_us, bool partial) {
    pkt->data = const_cast<uint8_t*>(p);
    pkt->size = static_cast<int>(n);
    pkt->pts = pts_us;
    pkt->flags = partial ? AV_PKT_FLAG_PARTIAL : 0;
    const int sr = avcodec_send_packet(cc, pkt);
    av_packet_unref(pkt);
    if (sr >= 0) return true;
    // AVERROR_INVALIDDATA is bitstream damage from a lossy link, not a
    // decoder/device fault; AVERROR(EAGAIN) here means the packet was NOT
    // accepted (the decoder wants receive_frame() drained first), which is
    // a dropped packet and therefore a hard error, not something to ignore.
    if (sr == AVERROR_INVALIDDATA) ++bitstream; else ++errors;
    stream_open = false;
    return false;
  }

  // Every frame the decoder has ready -> the ready queue (or dropped).
  void drain() {
    for (;;) {
      AVFrame* av = av_frame_alloc();
      const int rr = av ? avcodec_receive_frame(cc, av) : AVERROR(ENOMEM);
      if (rr != 0) {
        av_frame_free(&av);
        if (rr == AVERROR_INVALIDDATA) ++bitstream;
        else if (rr != AVERROR(EAGAIN) && rr != AVERROR_EOF) ++errors;
        break;
      }
      take(av);
    }
  }

  // One decoded frame -> the ready queue, or dropped (counted) if it is not
  // the shape DrmPresenter imports.
  void take(AVFrame* av) {
    if (av->format != AV_PIX_FMT_DRM_PRIME || !av->data[0]) {
      ++errors;
      av_frame_free(&av);
      return;
    }
    const auto* desc = reinterpret_cast<const AVDRMFrameDescriptor*>(av->data[0]);
    DmaFrame f;
    const PrimeReject why = prime_to_dma(to_prime_desc(*desc), av->width, av->height,
                                         static_cast<uint32_t>(av->pts), &f);
    if (why != PrimeReject::kNone) {
      ++dropped_shape;
      if (!logged_shape) {
        logged_shape = true;
        std::fprintf(stderr,
                     "V4l2ReqBackend: dropping frames: DRM-PRIME layout not linear NV12 (%s): "
                     "objects=%d layers=%d fmt=%.4s planes=%d mod=%#llx\n",
                     prime_reject_name(why), desc->nb_objects, desc->nb_layers,
                     reinterpret_cast<const char*>(&desc->layers[0].format),
                     desc->layers[0].nb_planes,
                     static_cast<unsigned long long>(desc->objects[0].format_modifier));
      }
      av_frame_free(&av);
      return;
    }
    f.opaque = av;
    handoff.push_ready(f);
  }
};

V4l2ReqBackend::V4l2ReqBackend() = default;
V4l2ReqBackend::~V4l2ReqBackend() = default;

bool V4l2ReqBackend::init(const BackendCfg&, FrameSink sink) {
  impl_ = std::make_unique<Impl>();
  impl_->sink = std::move(sink);
  if (!impl_->open()) {
    impl_.reset();
    return false;
  }
  return true;
}

void V4l2ReqBackend::submit_au(const uint8_t* au, size_t n, uint32_t pts_us) {
  if (!impl_ || !au || n == 0) return;
  Impl& m = *impl_;
  m.free_releases();
  // Only worth scanning the AU for an entry point while the gate is still
  // closed -- an open gate admits unconditionally (EntryGate::admit), so
  // every AU after the first one would otherwise pay for a split_nals scan
  // (and its vector alloc) whose result is discarded.
  const bool entry = !m.gate.open() && mabur::au_is_entry_point(au, n);
  if (!m.gate.admit(entry)) {
    m.gated.store(m.gate.gated(), std::memory_order_relaxed);
    return;
  }
  // The decoder copies what it keeps (the packet is not refcounted), so the
  // ring slot may be reused as soon as this returns.
  m.send(au, n, pts_us, false);
  m.drain();
}

bool V4l2ReqBackend::probe() { return impl_ != nullptr; }

bool V4l2ReqBackend::start(const uint8_t* p, size_t n, uint8_t, uint32_t pts_us) {
  if (!impl_ || !p || n == 0) return false;
  Impl& m = *impl_;
  m.free_releases();
  // libav has no guard of its own against a new picture's first slice
  // arriving while a picture is still open (StreamFeeder should always abort
  // or finish the previous one first, but this is cheap insurance): end the
  // open picture exactly as abort() does before starting the new one.
  if (m.stream_open) {
    m.send(kAud, sizeof kAud, pts_us, false);
    m.stream_open = false;
    m.drain();
    ++m.errors;
    if (!m.logged_reopen) {
      m.logged_reopen = true;
      std::fprintf(stderr,
                   "V4l2ReqBackend: a new streamed picture started over an open one; ended it\n");
    }
  }
  // Before the first entry point the picture is not taken here: it goes the
  // whole-AU path at its close, where the gate drops and counts it.
  const bool entry = !m.gate.open() && mabur::au_is_entry_point(p, n);
  if (!m.gate.would_admit(entry)) return false;
  m.gate.admit(entry);
  if (!m.send(p, n, pts_us, true)) {
    if (!m.logged_stream_error) {
      m.logged_stream_error = true;
      std::fprintf(stderr, "V4l2ReqBackend: a streamed picture was refused at its first slice\n");
    }
    return false;
  }
  m.stream_open = true;
  return true;
}

bool V4l2ReqBackend::append(const uint8_t* p, size_t n, uint32_t pts_us, bool last) {
  if (!impl_ || !impl_->stream_open || !p || n == 0) return false;
  Impl& m = *impl_;
  m.free_releases();
  if (!m.send(p, n, pts_us, !last)) {
    if (!m.logged_stream_error) {
      m.logged_stream_error = true;
      std::fprintf(stderr, "V4l2ReqBackend: a streamed slice was refused\n");
    }
    return false;
  }
  if (last) {
    m.stream_open = false;
    m.drain();
  }
  return true;
}

void V4l2ReqBackend::abort(uint32_t pts_us) {
  if (!impl_) return;
  Impl& m = *impl_;
  // A hard mid-picture send() failure already cleared stream_open on libav's
  // side (and ours) without draining: hevc_receive_frame() consumes the
  // failing packet itself, but the picture's frame is left behind in
  // libav's output fifo. That frame must still be drained here, or it
  // parks unconsumed behind the next start()'s packet and desyncs from
  // libav's real state. So drain unconditionally; only a still-open
  // picture needs the AUD first.
  if (m.stream_open) {
    m.send(kAud, sizeof kAud, pts_us, false);
    m.stream_open = false;
  }
  m.drain();
}

bool V4l2ReqBackend::set_poll_timeout_ms(int ms) {
  if (!impl_ || ms < 0) return false;
  impl_->poll_timeout_ms.store(ms);
  return true;
}

void V4l2ReqBackend::poll() {
  if (!impl_) return;
  DmaFrame f;
  const int wait_ms = impl_->poll_timeout_ms.load();
  bool got = wait_ms > 0 ? impl_->handoff.pop_ready_wait(&f, std::chrono::milliseconds(wait_ms))
                         : impl_->handoff.pop_ready(&f);
  for (; got; got = impl_->handoff.pop_ready(&f)) {
    ++impl_->frames;
    if (impl_->sink) {
      impl_->sink(f);
    } else {
      impl_->handoff.push_release(f);
    }
  }
}

void V4l2ReqBackend::release_frame(const DmaFrame& f) {
  if (!impl_ || !f.opaque) return;
  impl_->handoff.push_release(f);
}

void V4l2ReqBackend::flush() {
  if (!impl_) return;
  // The presenter and regulator have already returned their frames
  // (main.cpp's flush order), so both queues hold everything outstanding.
  impl_->free_all();
  impl_->stream_open = false;
  avcodec_flush_buffers(impl_->cc);
  impl_->gate.arm();
}

uint64_t V4l2ReqBackend::frames() const { return impl_ ? impl_->frames.load() : 0; }
uint64_t V4l2ReqBackend::errors() const { return impl_ ? impl_->errors.load() : 0; }
uint64_t V4l2ReqBackend::bitstream() const { return impl_ ? impl_->bitstream.load() : 0; }
uint64_t V4l2ReqBackend::dropped_shape() const {
  return impl_ ? impl_->dropped_shape.load() : 0;
}
uint64_t V4l2ReqBackend::gated() const { return impl_ ? impl_->gated.load() : 0; }

}  // namespace maburplay
