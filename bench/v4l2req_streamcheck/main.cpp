// v4l2req_streamcheck -- proves slice-by-slice (streamed) decode through the
// patched libav + Cedrus gives the same pictures as whole-AU decode.
//
//   v4l2req_streamcheck <capture.265> [--abort N]
//
// The capture is AUs back to back (tools/bench/ausniff.py --dump-annexb).
// Run 1 decodes each AU as one packet; run 2 cuts each AU the way maburplay's
// StreamFeeder does (one slice per packet, AV_PKT_FLAG_PARTIAL on all but the
// last). Both start at the first entry point (mabur::au_is_entry_point) and
// hash every decoded NV12 picture (FNV-1a over the visible rows). Pass: the
// same number of pictures and identical hashes. With --abort N, run 2 ends AU
// N after two slices with an AUD packet (maburplay's abort) -- but only if AU
// N actually has >= 3 stream pieces to cut after two; otherwise the abort
// never fires and the run would silently degrade into an ordinary pass that
// never exercised recovery, so that case is a hard error (exit 2) instead.
// When the abort does fire, pass means every picture from the next entry
// point on matches run 1, and decoding did not stall. Exit 0 = pass.
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/opt.h>
}

#include "au_split.h"
#include "mabur/hevc_params.h"

#ifndef AV_PKT_FLAG_PARTIAL
#error "libavcodec lacks the mabur stream patch (gs/player/ffmpeg-patches)"
#endif

namespace {

enum AVPixelFormat pick_drm_prime(AVCodecContext*, const enum AVPixelFormat* f) {
  for (; *f != AV_PIX_FMT_NONE; ++f)
    if (*f == AV_PIX_FMT_DRM_PRIME) return *f;
  return AV_PIX_FMT_NONE;
}

uint64_t fnv(uint64_t h, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

// Hash of the visible NV12 picture, read through a dmabuf mmap. `row` is a
// scratch buffer the caller keeps across frames: each visible row is
// memcpy'd into it (wide, cached loads) before FNV hashes it -- byte-wise
// FNV straight off the dmabuf mmap is a byte-at-a-time read of uncached
// memory, very slow; copying the row first and hashing the cached copy
// keeps the same bytes in the same order (so the hash is unchanged) while
// hashing cached memory instead.
std::optional<uint64_t> hash_frame(const AVFrame* f, std::vector<uint8_t>* row) {
  const auto* d = reinterpret_cast<const AVDRMFrameDescriptor*>(f->data[0]);
  if (d->nb_objects != 1 || d->nb_layers < 1 || d->layers[0].nb_planes != 2) return std::nullopt;
  const int fd = d->objects[0].fd;
  const size_t size = d->objects[0].size;
  void* m = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
  if (m == MAP_FAILED) return std::nullopt;
  dma_buf_sync s{DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
  if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &s) < 0) {
    munmap(m, size);
    return std::nullopt;
  }
  const auto* base = static_cast<const uint8_t*>(m);
  const auto& y = d->layers[0].planes[0];
  const auto& c = d->layers[0].planes[1];
  row->resize(static_cast<size_t>(f->width));
  uint64_t h = 1469598103934665603ull;
  for (int r = 0; r < f->height; ++r) {
    std::memcpy(row->data(), base + y.offset + r * y.pitch, row->size());
    h = fnv(h, row->data(), row->size());
  }
  for (int r = 0; r < f->height / 2; ++r) {
    std::memcpy(row->data(), base + c.offset + r * c.pitch, row->size());
    h = fnv(h, row->data(), row->size());
  }
  s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
  ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
  munmap(m, size);
  return h;
}

struct Decoder {
  AVCodecContext* cc = nullptr;
  AVBufferRef* hw = nullptr;
  AVPacket* pkt = nullptr;
  AVFrame* fr = nullptr;
  std::vector<uint8_t> hash_row;  // reused across hash_frame() calls
  bool ok() const { return cc != nullptr; }
  Decoder() {
    const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    cc = avcodec_alloc_context3(dec);
    cc->flags |= AV_CODEC_FLAG_LOW_DELAY;
    cc->thread_count = 1;
    if (av_opt_set_int(cc->priv_data, "stream_slices", 1, 0) < 0 ||
        av_hwdevice_ctx_create(&hw, AV_HWDEVICE_TYPE_V4L2REQUEST, nullptr, nullptr, 0) < 0) {
      avcodec_free_context(&cc);
      return;
    }
    cc->hw_device_ctx = av_buffer_ref(hw);
    cc->get_format = pick_drm_prime;
    cc->extra_hw_frames = 8;
    if (avcodec_open2(cc, dec, nullptr) < 0) avcodec_free_context(&cc);
    pkt = av_packet_alloc();
    fr = av_frame_alloc();
  }
  ~Decoder() {
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&cc);
    av_buffer_unref(&hw);
  }
  int send(const uint8_t* p, size_t n, bool partial) {
    pkt->data = const_cast<uint8_t*>(p);
    pkt->size = static_cast<int>(n);
    pkt->flags = partial ? AV_PKT_FLAG_PARTIAL : 0;
    const int r = avcodec_send_packet(cc, pkt);
    av_packet_unref(pkt);
    return r;
  }
  // Every frame now available, hashed.
  void drain(std::vector<std::optional<uint64_t>>* out) {
    while (avcodec_receive_frame(cc, fr) == 0) {
      out->push_back(hash_frame(fr, &hash_row));
      av_frame_unref(fr);
    }
  }
};

constexpr uint8_t kAud[] = {0x00, 0x00, 0x01, 0x46, 0x01, 0x50};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <capture.265> [--abort N]\n", argv[0]);
    return 2;
  }
  long abort_au = -1;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--abort")) continue;
    if (i + 1 >= argc) {
      std::fprintf(stderr, "usage: %s <capture.265> [--abort N]\n", argv[0]);
      return 2;
    }
    abort_au = std::atol(argv[++i]);
  }
  std::ifstream in(argv[1], std::ios::binary);
  const std::vector<uint8_t> cap((std::istreambuf_iterator<char>(in)), {});
  const auto aus = streamcheck::split_aus(cap.data(), cap.size());
  size_t first = 0;
  while (first < aus.size() &&
         !mabur::au_is_entry_point(cap.data() + aus[first].begin, aus[first].end - aus[first].begin))
    ++first;
  if (first == aus.size()) {
    std::fprintf(stderr, "no entry point in %zu AUs\n", aus.size());
    return 2;
  }

  // per_au[i] = hashes the decoder returned after AU i (empty = none).
  using Hashes = std::vector<std::vector<std::optional<uint64_t>>>;
  // Set only by the streamed run, when it reaches AU abort_au: whether the
  // cut actually happened (>= 3 pieces to cut after two) and, either way,
  // how many pieces that AU had -- so a never-fired abort can be reported
  // rather than silently passing on a run that never exercised recovery.
  bool abort_fired = false;
  size_t abort_au_pieces = 0;
  auto run = [&](bool streamed, Hashes* per_au, double* ms) -> bool {
    Decoder d;
    if (!d.ok()) {
      std::fprintf(stderr, "decoder init failed (patched libavcodec + Cedrus?)\n");
      return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    per_au->assign(aus.size(), {});
    for (size_t i = first; i < aus.size(); ++i) {
      const uint8_t* au = cap.data() + aus[i].begin;
      const size_t n = aus[i].end - aus[i].begin;
      if (!streamed) {
        d.send(au, n, false);
      } else {
        const auto pieces = streamcheck::stream_pieces(au, n);
        const bool want_cut = static_cast<long>(i) == abort_au;
        if (want_cut) abort_au_pieces = pieces.size();
        const bool cut = want_cut && pieces.size() >= 3;
        if (cut) abort_fired = true;
        const size_t send_n = cut ? 2 : pieces.size();
        for (size_t k = 0; k < send_n; ++k)
          d.send(au + pieces[k].begin, pieces[k].end - pieces[k].begin, cut || k + 1 < pieces.size());
        if (cut) d.send(kAud, sizeof kAud, false);
      }
      d.drain(&(*per_au)[i]);
    }
    *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
  };

  Hashes whole, streamed;
  double ms_whole = 0, ms_stream = 0;
  if (!run(false, &whole, &ms_whole) || !run(true, &streamed, &ms_stream)) return 1;

  // An --abort that never cut anything would otherwise compare cleanly
  // (the exclusion window below is empty) and exit 0 having exercised
  // nothing -- a pass that proves nothing is worse than no test at all.
  if (abort_au >= 0 && !abort_fired) {
    std::fprintf(stderr, "abort AU %ld has %zu piece(s); need >= 3\n", abort_au, abort_au_pieces);
    return 2;
  }

  size_t next_entry = aus.size();
  if (abort_au >= 0)
    for (size_t i = static_cast<size_t>(abort_au) + 1; i < aus.size(); ++i)
      if (mabur::au_is_entry_point(cap.data() + aus[i].begin, aus[i].end - aus[i].begin)) {
        next_entry = i;
        break;
      }

  size_t compared = 0, mismatched = 0, missing = 0;
  for (size_t i = first; i < aus.size(); ++i) {
    if (abort_au >= 0 && static_cast<long>(i) >= abort_au && i < next_entry) continue;
    if (whole[i].size() != streamed[i].size() || whole[i].empty()) {
      ++missing;
      continue;
    }
    for (size_t k = 0; k < whole[i].size(); ++k) {
      ++compared;
      if (!whole[i][k] || !streamed[i][k] || *whole[i][k] != *streamed[i][k]) ++mismatched;
    }
  }
  std::printf("{\"aus\":%zu,\"first_entry\":%zu,\"compared\":%zu,\"mismatched\":%zu,"
              "\"missing\":%zu,\"abort_au\":%ld,\"abort_fired\":%s,\"next_entry\":%zu,"
              "\"ms_whole\":%.1f,\"ms_streamed\":%.1f}\n",
              aus.size(), first, compared, mismatched, missing, abort_au,
              abort_fired ? "true" : "false", abort_au >= 0 ? next_entry : 0, ms_whole, ms_stream);
  return (mismatched == 0 && missing == 0 && compared > 0) ? 0 : 1;
}
