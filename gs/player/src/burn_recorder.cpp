#include "burn_recorder.h"

#include "fit_rect.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <rockchip/rk_mpi.h>

#include "mabur/dvr_mux.h"
#include "mabur/hevc_params.h"
#include "mabur/threaded_sink.h"
#include "mpp_backend.h"
#include "mpp_encoder.h"
#include "pi_mutex.h"

#ifdef MABUR_PLAYER_GPU
#include <rga/im2d.h>
#include <rga/rga.h>

#include "frame_colortrans.h"
#endif

// The burned-DVR driver: an encoder thread fed from the display's flip (the
// release thread, through RecorderTap) by MPP buffer reference, on the
// vblank grid, writing fMP4 through the same DvrMux the raw path uses. See
// burn_recorder.h for the ownership and threading contracts; the notes below
// are the implementation-side consequences of them.
//
// Three things this file exists to get right:
//
//  1. EVERY inc_ref has exactly one matching put. The paths are enumerated in
//     the header and each one is marked "REF-" in the code below. The one
//     that is easy to miss -- and that costs a decoder buffer per occurrence,
//     stalling decode after ~24 -- is the mailbox DISPLACEMENT: overwriting a
//     pending entry is the normal steady-state behaviour under overload, and
//     the displaced entry still holds a reference. submit() runs on the
//     SCHED_FIFO release thread, which must never run mpp_buffer_put, so a
//     displaced entry is parked on a bounded list and released by whichever
//     thread next takes the mailbox.
//
//  2. MppEncoder is single-threaded by contract (set_osd() and encode() from
//     ONE thread, see mpp_encoder.h). init()/set_palette() run on the caller's
//     thread inside start(), before the recorder thread is launched; every
//     later encoder call -- request_idr(), set_osd(), encode() -- happens on
//     the recorder thread. request_idr() from the main loop is therefore a
//     flag, not a passthrough.
//
//  3. The mux CANNOT be opened at start(): MppEncoder::header() is empty until
//     the first successful encode() latches the geometry, and the first IDR
//     carries NO in-band VPS/SPS/PPS (MPP's GET_HDR_SYNC marks the header as
//     already added, and mpp_enc_add_sw_header then skips it for the whole
//     first task). So the hvcC sample entry is built from header() -- the
//     ENCODER's parameter sets, not the drone's -- and the file is created on
//     the first encoded keyframe. A recording that never encodes a frame
//     leaves no file at all, which is the honest outcome.
namespace maburplay {

namespace {

// Consecutive refusals, with nothing ever encoded, after which the recorder
// gives up for good. MppEncoder's documented permanent-refusal signature is
// errors() climbing while frames() stays 0 (a picture size disagreeing with
// EncCfg, or strides smaller than the picture); retrying that at the
// recording's rate forever would only flood the log. 1 s on a 60 fps grid.
constexpr uint64_t kMaxInitialFailures = 60;

// The same backstop for a recording that HAD been working and then stopped --
// the realistic trigger being the VPU session wedging mid-flight, the failure
// class the decode watchdog exists for. Without it, a persistent failure in
// MppEncoder's MPP-error branches (mpp_frame_init / packet init /
// encode_put_frame / encode_get_packet, none of them rate-limited) is fed at
// the recording's rate forever: ~2 KB/s into /tmp/maburplay.log, which is an
// unrotated `>` redirect on tmpfs, plus a decoder-buffer round-trip per
// frame for nothing. BurnRecorder is the right place to bound that because
// it owns the feed rate. Looser than kMaxInitialFailures on purpose (10 s on
// a 60 fps grid): an established recording is worth some patience, and
// whatever was recorded before the stall is still closed cleanly by stop().
constexpr uint64_t kMaxRunningFailures = 600;

}  // namespace

struct BurnRecorder::Impl {
  // Dirty rects the recorder will copy one at a time; beyond this, taking
  // the whole map is cheaper than the per-rect row loop.
  static constexpr size_t kMaxDirtyRects = 128;

  BurnCfg cfg;
  std::string path;

  std::unique_ptr<MppEncoder> enc;
  mabur::DvrMux mux;
  bool mux_open = false;
  bool mux_failed = false;  // open refused once; do not retry per frame

  OsdPalette palette;
  bool have_palette = false;   // set_palette() was called
  bool palette_live = false;   // ...and the encoder accepted it

  // --- mailbox (release thread -> stage or recorder thread) ---------------
  struct Mail {
    void* buf = nullptr;  // MppBuffer; non-null means "a reference is held"
    uint64_t pts_us = 0;
    int w = 0, h = 0, stride = 0, vstride = 0;
  };
  // Two mutexes, never nested except in stop():
  //
  //   box_mu  the mailbox `box`, the displaced list, and nothing else.
  //           submit(), the take of `box` (ct thread, or the recorder thread
  //           without one) and drain_box() hold it for a few field copies --
  //           plus, in submit(), one mpp_buffer_inc_ref -- and never release
  //           a buffer under it. A PiMutex because submit() runs on the
  //           SCHED_FIFO release thread: a normal-priority holder (the
  //           taker, a drain) that CFS preempted mid-section would otherwise
  //           make the release thread wait out a scheduler slice; priority
  //           inheritance runs the holder to its unlock instead. Separate
  //           from `mu` so a submit never waits behind set_osd(), whose
  //           quantize holds `mu` for up to milliseconds.
  //   mu      everything else shared: the OSD handoff, ct_out, ct_dst_busy.
  //
  // `stopping` is written under BOTH (stop() takes them together) and read
  // under either, so every wait predicate reads only state guarded by the
  // mutex that wait holds.
  //
  // Each condition variable pairs with exactly one mutex: box_cv with
  // box_mu (a frame in `box`, or stopping), ct_cv with mu (a frame in
  // ct_out, or stopping). Every writer changes the predicate's state under
  // that mutex and notifies after releasing it, and every waiter re-checks
  // the predicate under the same mutex before sleeping, so a notify cannot
  // fall between a waiter's check and its sleep: no lost wake-up. The OSD
  // map is not a wake condition at all (it rides along with the next frame),
  // so nothing waits on `mu`-guarded state through box_cv, which is a
  // condition_variable_any only because box_mu is a PiMutex.
  //
  // displaced[]: entries a newer submit() pushed out of `box`. The release
  // thread may not run mpp_buffer_put, so they wait here, still holding
  // their references, until the next take of `box` or drain_box() copies
  // them out under box_mu and releases them outside it. Every take of
  // `box` empties the list too, so a non-empty list implies a full `box`.
  // When the list is full submit() takes no reference at all (a refusal,
  // counted in frames_dropped), which is what bounds the buffers this class
  // can hold.
  static constexpr int kDisplaced = 2;
  // Decoder-buffer references this recorder holds right now, wherever they
  // are (mailbox, displaced list, stage, ct_out, encode()). +1 at the
  // inc_ref in submit(), -1 at every put of a decoder buffer (put_dec(), on
  // whichever thread) -- never for the stage's own ct_dst buffers. submit()
  // refuses at kMaxDecRefs, which is the bound in burn_recorder.h in every
  // mode, whether the stage copies or passes frames through flat. Only
  // submit() increments, from the one release thread, so its
  // check-then-increment cannot overshoot. stop() warns if it is not back
  // to 0: a missed put would leave it climbing until every flip is refused.
  static constexpr int kMaxDecRefs = 4;
  std::atomic<int> dec_refs_held{0};
  PiMutex box_mu;
  std::condition_variable_any box_cv;
  Mail box;                      // box_mu
  Mail displaced[kDisplaced];    // box_mu
  int n_displaced = 0;           // box_mu
  std::mutex mu;
  bool stopping = false;     // written under mu AND box_mu, read under either

  // OSD handoff, under mu.
  //
  //   osd_map    the AUTHORITATIVE index map. Persistent across calls and
  //              updated in place by set_osd()'s thread, which is what lets a
  //              steady-state OSD update cost the cells that changed
  //              instead of 2.07 M pixels (see set_osd()).
  //   osd_dirty  the parts of osd_map the recorder thread has not copied
  //              out yet -- accumulated, because set_osd() publishes at
  //              the MSP rate and the recorder consumes at the frame rate,
  //              and neither waits for the other.
  //   osd_work   the recorder's own copy, brought up to date under the lock
  //              and then handed to the encoder outside it.
  //
  // No swapping: a swapped-in buffer is a stale GENERATION, and an
  // incremental update on top of one is silently wrong.
  OsdIndexMap osd_map, osd_work;
  std::vector<DirtyRect> osd_dirty;
  bool osd_full_pending = false;   // osd_dirty is meaningless; copy it whole
  bool osd_pending_valid = false;  // osd_map moved since the last take
  QuantizeCache qcache;            // set_osd()'s thread only; palette-lifetime memo

  std::thread th;
  bool started = false;
  std::atomic<bool> dead{false};        // fatal: stop feeding the encoder
  std::atomic<bool> idr_pending{false};

  std::atomic<uint64_t> frames_in{0};
  std::atomic<uint64_t> frames_encoded{0};
  std::atomic<uint64_t> frames_dropped{0};   // displaced, refused or dead: OVERLOAD
  std::atomic<uint64_t> frames_flushed{0};   // drop_pending()/stop(): HYGIENE
  std::atomic<uint64_t> encode_errors{0};
  std::atomic<uint64_t> osd_rejects{0};
  std::atomic<uint64_t> colortrans_fallbacks{0};
  std::atomic<uint64_t> ct_n{0}, ct_sum_us{0}, ct_max_us{0};    // stage_us()
  std::atomic<uint64_t> enc_n{0}, enc_sum_us{0}, enc_max_us{0};
  static void account(std::atomic<uint64_t>& n, std::atomic<uint64_t>& sum,
                      std::atomic<uint64_t>& mx, std::chrono::steady_clock::time_point t0) {
    const uint64_t us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                  std::chrono::steady_clock::now() - t0)
                                                  .count());
    n.fetch_add(1);
    sum.fetch_add(us);
    uint64_t cur = mx.load();
    while (us > cur && !mx.compare_exchange_weak(cur, us)) {
    }
  }
  // --- stage thread (colortrans and/or scale) and its handoff -------------
  //
  // In a MABUR_PLAYER_GPU build the recorder is a two-stage pipeline:
  //
  //   release --box--> ct thread --ct_out--> recorder thread
  //                    GPU draw + RGA          encode() + mux
  //                    (or RGA resize)
  //                    into ct_dst[k]
  //
  // SCALE: the encoder latches its picture size from the first frame and
  // rejects every frame of another size after that, and it lays the OSD
  // index map 1:1 from the picture's top-left. The stage therefore scales
  // every frame to the recording's own size (out_w x out_h = BurnCfg
  // width x height), letterboxed if the aspect differs (fit_rect.h), so a
  // decoded size that changes mid-recording keeps recording and the overlay,
  // composed at the record size, stays in place. With colortrans the shader
  // draw scales for free; without it, one RGA resize. A frame already in
  // the encoder's input geometry passes through flat; one at the record
  // size but with other strides takes a restride copy through the same path,
  // and one the stage cannot conform is dropped (run_ct states the rule).
  //
  // Both stages are hardware waits (GPU/RGA ~13-22 ms, rkvenc ~13 ms at
  // 1080p) and serialising them on one thread capped the burned DVR at
  // 26-32 fps with a third of the admitted frames dropped (bench).
  // Overlapped, the DVR runs at max(stage) instead of the sum,
  // and the GPU is busy enough for devfreq to actually clock it up.
  //
  // ct_out is a one-deep, latest-wins mailbox like `box`: a slow encoder
  // displaces the previous output (frames_dropped, same meaning as a box
  // displacement) rather than stalling the GPU stage. kCtDst = 3 covers the
  // worst case: one destination being rendered, one waiting in ct_out, one
  // inside encode(). ct_dst_busy[] (under mu) says which are taken.
  //
  // A build without the stage has no ct thread: the recorder thread
  // consumes `box` directly, at the decoded size.
  struct CtMail {
    void* buf = nullptr;  // MppBuffer: ct_dst[dst_idx], or the SOURCE when dst_idx < 0
    int dst_idx = -1;     // < 0 = flat: buf is a held decoder reference
    uint64_t pts_us = 0;
    int w = 0, h = 0, stride = 0, vstride = 0;
  };
  bool ct_on = false;  // start(): the stage thread runs (every MABUR_PLAYER_GPU build)
  // Recording picture size and its strides, fixed at start() from BurnCfg
  // width x height; 0 = no stage, record at the decoded size.
  int out_w = 0, out_h = 0, out_hs = 0, out_vs = 0;

  // A frame's NV12 geometry as the encoder sees it. MppEncoder latches all
  // four on its first frame and rejects every later frame that differs in
  // any of them, stride included.
  struct Geom {
    int w = 0, h = 0, stride = 0, vstride = 0;
    bool known() const { return w > 0; }
    bool operator==(const Geom& o) const {
      return w == o.w && h == o.h && stride == o.stride && vstride == o.vstride;
    }
  };
  // enc_latched_geom: the geometry the encoder latched, published by the
  // recorder thread after its first successful encode() (16 bits per field,
  // 0 = not yet). stage_fwd_geom: the geometry of the first frame the stage
  // forwarded (ct thread only) -- the stage is the encoder's only feeder,
  // so that is what the encoder will latch; it stands in until the
  // encoder's own value is published.
  std::atomic<uint64_t> enc_latched_geom{0};
  Geom stage_fwd_geom;
  static uint64_t pack_geom(const Geom& g) {
    if (g.w <= 0 || g.h <= 0 || g.stride <= 0 || g.vstride <= 0 || g.w > 0xffff ||
        g.h > 0xffff || g.stride > 0xffff || g.vstride > 0xffff)
      return 0;
    return (uint64_t)g.w << 48 | (uint64_t)g.h << 32 | (uint64_t)g.stride << 16 |
           (uint64_t)g.vstride;
  }
  static Geom unpack_geom(uint64_t v) {
    Geom g;
    g.w = (int)(v >> 48 & 0xffff);
    g.h = (int)(v >> 32 & 0xffff);
    g.stride = (int)(v >> 16 & 0xffff);
    g.vstride = (int)(v & 0xffff);
    return g;
  }
  std::condition_variable ct_cv;  // ct_out handoff, with mu
  CtMail ct_out;                  // mu
  std::thread ct_th;

  // box_mu HELD: moves the displaced list into `out` (kDisplaced entries)
  // and empties it; returns how many `out` now holds. The caller releases
  // them with put_mails() after dropping box_mu -- never under it, since
  // mpp_buffer_put takes MPP's own lock and submit() waits on box_mu from
  // the release thread.
  int take_displaced_locked(Mail* out) {
    const int n = n_displaced;
    for (int i = 0; i < n; ++i) {
      out[i] = displaced[i];
      displaced[i] = Mail{};
    }
    n_displaced = 0;
    return n;
  }
  // Releases one decoder-buffer reference taken in submit(). Every put of
  // a decoder buffer goes through here so dec_refs_held stays exact.
  void put_dec(void* buf) {
    mpp_buffer_put(static_cast<MppBuffer>(buf));
    dec_refs_held.fetch_sub(1);
  }
  // box_mu NOT held: releases the references `n` mailbox entries hold.
  void put_mails(const Mail* m, int n) {
    for (int i = 0; i < n; ++i) put_dec(m[i].buf);
  }

  // Releases whatever a CtMail holds: a destination goes back to the free
  // set, a flat source reference goes back to the decoder pool.
  void release_ct(const CtMail& o) {
    if (!o.buf) return;
    if (o.dst_idx >= 0) {
      std::lock_guard<std::mutex> lk(mu);
      ct_dst_busy[o.dst_idx] = false;
    } else {
      put_dec(o.buf);
    }
  }

  static constexpr int kCtDst = 3;
  bool ct_dst_busy[kCtDst] = {};  // mu
#ifdef MABUR_PLAYER_GPU
  std::unique_ptr<FrameColorTrans> ct;  // ct thread only (EGL is thread-bound)
  bool ct_failed = false;               // init failed once: no GPU pass, logged once
  bool dst_failed = false;              // destinations failed once: stay flat
  uint64_t scale_fail_logs = 0;
  MppBufferGroup ct_grp = nullptr;      // private DRM group for the destinations
  MppBuffer ct_dst[kCtDst] = {};
  // The letterbox geometry whose black bars ct_dst[k] already holds (0 =
  // none): rga_scale fills them once, not every frame.
  uint64_t ct_dst_box[kCtDst] = {};
  size_t ct_dst_size = 0;
  int ct_w = 0, ct_h = 0;

  // ct thread. Brings the colortrans stage up at the OUTPUT size; false =
  // no GPU pass for this frame (logged once, then stays off).
  bool ct_ready(int ow, int oh) {
    if (ct_failed) return false;
    if (!ct || ct_w != ow || ct_h != oh) {
      ct.reset(new FrameColorTrans());
      if (!ct->init((uint32_t)ow, (uint32_t)oh)) {
        std::fprintf(stderr, "BurnRecorder: colortrans GPU stage unavailable; recording FLAT\n");
        ct.reset();
        ct_failed = true;
        return false;
      }
      ct_w = ow;
      ct_h = oh;
    }
    return true;
  }

  // ct thread. The destinations, sized once on the first staged frame at
  // the output geometry, which is the encoder's latched input geometry
  // from then on (see run_ct), so later frames need the same size. false =
  // no destination: a frame already in the encoder's geometry passes
  // through flat, any other is dropped.
  bool dst_prepare(size_t need) {
    if (dst_failed) return false;
    if (ct_dst_size) return need <= ct_dst_size;
    if (!ct_grp && mpp_buffer_group_get_internal(&ct_grp, MPP_BUFFER_TYPE_DRM) != MPP_OK) {
      std::fprintf(stderr, "BurnRecorder: stage buffer group failed; frames that need a scale "
                           "or restride are dropped\n");
      dst_failed = true;
      return false;
    }
    for (int i = 0; i < kCtDst; ++i) {
      if (mpp_buffer_get(ct_grp, &ct_dst[i], need) != MPP_OK || !ct_dst[i]) {
        std::fprintf(stderr, "BurnRecorder: stage destination alloc failed; frames that need a "
                             "scale or restride are dropped\n");
        dst_failed = true;
        return false;
      }
    }
    ct_dst_size = need;
    return true;
  }

  // ct thread. NV12 -> NV12 resize on RGA, synchronous; at equal size a
  // restride copy into the destination's strides. box: scale into that
  // sub-rectangle with black bars around it (a letterbox); the bars are
  // filled once per destination buffer and geometry, since nothing else
  // writes them.
  bool rga_scale(int src_fd, const Mail& m, int dst_fd, int ow, int oh, int ohs, int ovs,
                 int k, const maburplay::Rect* box) {
    rga_buffer_t s = wrapbuffer_fd_t(src_fd, m.w, m.h, m.stride, m.vstride,
                                     RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t d = wrapbuffer_fd_t(dst_fd, ow, oh, ohs, ovs, RK_FORMAT_YCbCr_420_SP);
    if (box) {
      const uint64_t key = (uint64_t)box->x << 48 | (uint64_t)box->y << 32 |
                           (uint64_t)box->w << 16 | (uint64_t)box->h;
      if (ct_dst_box[k] != key) {
        // 0: black whatever byte order librga reads the colour in (its
        // RGBA order puts red in the top byte, so 0xff000000 is red).
        if (imfill(d, im_rect{0, 0, ow, oh}, 0x00000000) != IM_STATUS_SUCCESS) {
          if (scale_fail_logs++ % 300 == 0)
            std::fprintf(stderr, "BurnRecorder: RGA letterbox fill %dx%d failed\n", ow, oh);
          return false;
        }
        ct_dst_box[k] = key;
      }
      rga_buffer_t pat{};
      if (improcess(s, d, pat, im_rect{0, 0, m.w, m.h}, im_rect{box->x, box->y, box->w, box->h},
                    im_rect{}, -1, nullptr, nullptr, IM_SYNC) == IM_STATUS_SUCCESS)
        return true;
    } else if (m.w == ow && m.h == oh) {
      if (imcopy(s, d) == IM_STATUS_SUCCESS) return true;
    } else if (imresize(s, d, 0, 0, INTER_LINEAR) == IM_STATUS_SUCCESS) {
      return true;
    }
    if (scale_fail_logs++ % 300 == 0)
      std::fprintf(stderr, "BurnRecorder: RGA resize %dx%d -> %dx%d failed (%llu so far)\n", m.w,
                   m.h, ow, oh, static_cast<unsigned long long>(scale_fail_logs));
    return false;
  }

  // stop(), after BOTH joins: nothing references the destinations any more.
  void ct_free_dst() {
    for (int i = 0; i < kCtDst; ++i) {
      if (ct_dst[i]) mpp_buffer_put(ct_dst[i]);
      ct_dst[i] = nullptr;
      ct_dst_busy[i] = false;
      ct_dst_box[i] = 0;
    }
    ct_dst_size = 0;
    if (ct_grp) mpp_buffer_group_put(ct_grp);
    ct_grp = nullptr;
  }

  // The ct thread: box -> GPU stage -> ct_out. Never blocks on the encoder.
  void run_ct() {
    for (;;) {
      Mail m;
      Mail old[kDisplaced];
      int n_old = 0;
      {
        std::unique_lock<PiMutex> lk(box_mu);
        box_cv.wait(lk, [this] { return stopping || box.buf != nullptr; });
        if (stopping) break;
        m = box;
        box.buf = nullptr;
        n_old = take_displaced_locked(old);
      }
      if (n_old) {
        // REF-DISPLACE, released: flips a newer one pushed out of `box`
        // while this thread was busy. Released here, by the taker, because
        // the release thread that displaced them may not run
        // mpp_buffer_put.
        put_mails(old, n_old);
        frames_dropped.fetch_add(static_cast<uint64_t>(n_old));
      }
      if (dead.load()) {
        // REF-DEAD, ct side: the encoder gave up; nothing downstream wants this.
        put_dec(m.buf);
        frames_dropped.fetch_add(1);
        continue;
      }

      CtMail o;
      o.buf = m.buf;  // flat unless the stage succeeds below
      o.pts_us = m.pts_us;
      o.w = m.w;
      o.h = m.h;
      o.stride = m.stride;
      o.vstride = m.vstride;

      // Output geometry = the encoder's INPUT geometry, and the rule is: a
      // frame goes through the stage whenever its geometry (w, h, stride,
      // vstride) differs from it, not only when its size differs from the
      // record size -- the encoder rejects a stride mismatch as surely as a
      // size mismatch. The encoder's input geometry is what it latched
      // (enc_latched_geom), else what the stage forwarded first
      // (stage_fwd_geom). Before either exists, a frame at the record size
      // passes with its own strides and any other is scaled to the record
      // size at the stage's own aligned strides (out_hs x out_vs). So an
      // untouched first frame (a decoder vstride of 1080) makes every later
      // scaled frame land at 1080 rows too, and a scaled first frame makes
      // a later record-size frame take a restride copy (same size, RGA or
      // GPU) to the stage's strides.
      const Geom in{m.w, m.h, m.stride, m.vstride};
      Geom tgt = unpack_geom(enc_latched_geom.load());
      if (!tgt.known()) tgt = stage_fwd_geom;
      if (!tgt.known())
        tgt = (in.w == out_w && in.h == out_h) ? in : Geom{out_w, out_h, out_hs, out_vs};
      const bool scale = !(in == tgt);  // resize or restride through the stage
      const int ow = tgt.w, oh = tgt.h, ohs = tgt.stride, ovs = tgt.vstride;
      // A picture of another aspect than the output is letterboxed, as the
      // display shows it, never stretched. A restride at equal size is the
      // full frame (no box).
      const maburplay::Rect fit = maburplay::fit_rect(in.w, in.h, ow, oh);
      const maburplay::Rect* box = (scale && (fit.w != ow || fit.h != oh)) ? &fit : nullptr;
      const bool want_ct = cfg.colortrans != nullptr;

      bool ct_done = false, done = false;
      if ((want_ct || scale) && dst_prepare((size_t)ohs * (size_t)ovs * 3 / 2)) {
        int k = -1;
        {
          std::lock_guard<std::mutex> lk(mu);
          for (int i = 0; i < kCtDst && k < 0; ++i)
            if (!ct_dst_busy[i]) k = i;
          if (k >= 0) ct_dst_busy[k] = true;
        }
        // k < 0 cannot happen with kCtDst = 3 (see the pipeline comment);
        // if it ever does, the frame is handled as a failed stage below.
        if (k >= 0) {
          const auto t_ct = std::chrono::steady_clock::now();
          const int src_fd = mpp_buffer_get_fd(static_cast<MppBuffer>(m.buf));
          const int dst_fd = mpp_buffer_get_fd(ct_dst[k]);
          if (want_ct && ct_ready(ow, oh))
            ct_done = ct->process(src_fd, (uint32_t)m.w, (uint32_t)m.h, (uint32_t)m.stride,
                                  (uint32_t)m.vstride, dst_fd, (uint32_t)ohs, (uint32_t)ovs, box);
          // Without the GPU pass (off, or failed) a mismatched frame is still
          // scaled: a flat recording with the OSD in place beats a corrected
          // one with it misplaced.
          done = ct_done || (scale && rga_scale(src_fd, m, dst_fd, ow, oh, ohs, ovs, k, box));
          if (done) {
            account(ct_n, ct_sum_us, ct_max_us, t_ct);
            // REF-ENCODE, early: the GPU and RGA are done with the source
            // (glFinish + synchronous RGA); the encoder reads ct_dst.
            put_dec(m.buf);
            o.buf = ct_dst[k];
            o.dst_idx = k;
            o.w = ow;
            o.h = oh;
            o.stride = ohs;
            o.vstride = ovs;
          } else {
            std::lock_guard<std::mutex> lk(mu);
            ct_dst_busy[k] = false;
          }
        }
      }
      if (scale && !done) {
        // The frame needed the stage (a resize or a restride: its geometry
        // differs from tgt) and did not get it. It is dropped, never
        // forwarded flat: forwarded, a wrong-size frame either fixes the
        // encoder's latch at the decoded size, so the OSD region (laid at
        // the record size) is refused for the whole file, or is rejected
        // by an encoder that latched already. A same-size restride failure
        // goes the same way: a frame at the target size enters the stage
        // only when the latched (or first forwarded) geometry has other
        // strides, so its own are ones the encoder refuses. A permanent
        // stage failure shows as burn_drop climbing while frames_encoded
        // stands still.
        put_dec(m.buf);
        frames_dropped.fetch_add(1);
        continue;
      }
      if (want_ct && !ct_done) colortrans_fallbacks.fetch_add(1);

      // Whatever is forwarded is what the encoder will see; the first one
      // fixes the geometry later frames are conformed to (see above).
      if (!stage_fwd_geom.known()) stage_fwd_geom = Geom{o.w, o.h, o.stride, o.vstride};
      CtMail prev_out;
      {
        std::lock_guard<std::mutex> lk(mu);
        prev_out = ct_out;  // latest wins, same rule as the box
        ct_out = o;
      }
      ct_cv.notify_one();
      if (prev_out.buf) {
        // REF-DISPLACE, ct side: the encoder is the slow stage right now.
        release_ct(prev_out);
        frames_dropped.fetch_add(1);
      }
    }
    if (ct) {
      const FrameColorTrans::Breakdown& b = ct->breakdown();
      if (b.n)
        std::fprintf(stderr,
                     "BurnRecorder: colortrans stage mean per frame over %llu: import %.1f ms, "
                     "draw+finish %.1f ms, rga %.1f ms\n",
                     static_cast<unsigned long long>(b.n), b.import_us / 1000.0 / b.n,
                     b.draw_us / 1000.0 / b.n, b.rga_us / 1000.0 / b.n);
    }
    ct.reset();  // EGL teardown on the thread that owns the context
  }
#endif
  uint64_t consecutive_fail = 0;  // recorder thread only

  // One coded picture out of the encoder, on the recorder thread, from inside
  // encode(). `p` is only valid for the duration of the call, so DvrMux (which
  // copies into its pending fragment) is written here and now.
  void on_nal(const uint8_t* p, size_t n, uint64_t pts_us, bool keyframe) {
    if (!p || n == 0) return;
    if (!mux_open) {
      if (mux_failed) return;
      // Wait for a keyframe: an fMP4 whose first sample is a P slice cannot
      // be decoded from the start.
      if (!keyframe) return;
      // hvcC from the ENCODER's own VPS/SPS/PPS. header() is populated by the
      // geometry latch inside the first encode(), so by the time any packet
      // reaches this sink it is non-empty. The AU itself is fed too, for the
      // (currently impossible, but free) case of an encoder that does put its
      // parameter sets in band on the first IDR.
      mabur::HevcParams params;
      const std::vector<uint8_t>& hdr = enc->header();
      if (!hdr.empty()) params.feed(hdr.data(), hdr.size());
      if (!params.complete()) params.feed(p, n);
      if (!params.complete()) {
        mux_failed = true;
        std::fprintf(stderr,
                     "BurnRecorder: no VPS/SPS/PPS from the encoder (%zu header bytes); "
                     "recording disabled\n",
                     hdr.size());
        dead.store(true);
        return;
      }
      // Track header from the LATCHED picture size, which is the only thing
      // the samples actually are: the record size the stage scaled to, or
      // the decoded size in a build without the stage. cfg.width/height is
      // a fallback that cannot be reached today (this sink only runs from
      // inside encode(), after the latch).
      int tw = 0, th = 0;
      enc->picture_size(&tw, &th);
      if (tw <= 0 || th <= 0) {
        tw = cfg.width;
        th = cfg.height;
      }
      // The file's I/O runs on ThreadedSink's writer thread: a closing
      // fragment is ~2.5 MB, and writing it here (~22 ms on the GS card)
      // would push this thread past a frame period and lose the next frame
      // to the ct_out handoff.
      std::unique_ptr<mabur::DvrSink> file = mabur::open_file_sink(path);
      if (!file || !mux.open(std::make_unique<mabur::ThreadedSink>(std::move(file)),
                             params.hvcc(), tw, th, cfg.fragment_ms)) {
        mux_failed = true;
        std::fprintf(stderr, "BurnRecorder: cannot open %s; recording disabled\n", path.c_str());
        dead.store(true);
        return;
      }
      mux_open = true;
      std::fprintf(stderr, "BurnRecorder: recording %s (%dx%d, hvcC from %zu header bytes)\n",
                   path.c_str(), tw, th, hdr.size());
    }
    mux.write_sample(p, n, static_cast<uint32_t>(pts_us), keyframe);
  }

  void run() {
    for (;;) {
      CtMail o;
      bool have_osd = false;
      // stop() drains whatever is left in either mailbox (and the displaced
      // list) after the join, so this thread never has to decide who
      // releases a half-handled entry.
      if (ct_on) {
        std::unique_lock<std::mutex> lk(mu);
        ct_cv.wait(lk, [this] { return stopping || ct_out.buf != nullptr; });
        if (stopping) break;
        o = ct_out;
        ct_out.buf = nullptr;
      } else {
        Mail old[kDisplaced];
        int n_old = 0;
        {
          std::unique_lock<PiMutex> lk(box_mu);
          box_cv.wait(lk, [this] { return stopping || box.buf != nullptr; });
          if (stopping) break;
          o.buf = box.buf;
          o.pts_us = box.pts_us;
          o.w = box.w;
          o.h = box.h;
          o.stride = box.stride;
          o.vstride = box.vstride;
          box.buf = nullptr;
          n_old = take_displaced_locked(old);
        }
        if (n_old) {
          // REF-DISPLACE, released: same as the ct thread's take.
          put_mails(old, n_old);
          frames_dropped.fetch_add(static_cast<uint64_t>(n_old));
        }
      }
      // The OSD is taken after the frame, under mu alone: box_mu is never
      // held while waiting for mu, so a submit never queues behind a quantize
      // through this thread either. A map published between the two takes
      // rides with this frame instead of the next, which is the same
      // ride-along rule one frame earlier.
      {
        std::lock_guard<std::mutex> lk(mu);
        if (osd_pending_valid) {
          take_osd_locked();
          have_osd = true;
        }
      }

      if (dead.load()) {
        // REF-DEAD: a frame admitted just before the recorder gave up.
        release_ct(o);
        frames_dropped.fetch_add(1);
        continue;
      }

      if (idr_pending.exchange(false)) enc->request_idr();
      // A refused map means the encoder's region and the OSD surface
      // disagree -- a recording that is a silent plain transcode. Counted so
      // --fps-log can say so; the encoder's own log for it is once-only.
      if (have_osd && !enc->set_osd(osd_work)) osd_rejects.fetch_add(1);

      // Without the ct thread every frame is flat; with it, the ct thread
      // already counted its own fallbacks.
      if (!ct_on && cfg.colortrans) colortrans_fallbacks.fetch_add(1);

      const auto t_enc = std::chrono::steady_clock::now();
      const bool ok =
          enc->encode(o.buf, o.w, o.h, o.stride, o.vstride, static_cast<uint64_t>(o.pts_us));
      account(enc_n, enc_sum_us, enc_max_us, t_enc);
      // REF-ENCODE: released the moment the encoder is done with it, whether
      // or not it produced a packet. encode() borrows, it never owns.
      release_ct(o);

      if (ok) {
        // The first success is the encoder's latch: publish its input
        // geometry for the stage to conform every later frame to.
        if (!enc_latched_geom.load())
          enc_latched_geom.store(pack_geom(Geom{o.w, o.h, o.stride, o.vstride}));
        frames_encoded.fetch_add(1);
        consecutive_fail = 0;
        continue;
      }
      encode_errors.fetch_add(1);
      ++consecutive_fail;
      // Two thresholds, both "stop feeding the encoder" rather than "retry
      // forever". The tight one is MppEncoder's documented permanent-refusal
      // signature (errors() climbing with frames() stuck at 0), which will
      // never fix itself. The loose one bounds everything else, including a
      // recording that worked and then stopped working.
      const bool never_encoded = frames_encoded.load() == 0;
      if ((never_encoded && consecutive_fail >= kMaxInitialFailures) ||
          consecutive_fail >= kMaxRunningFailures) {
        // MppEncoder logged the reason once itself; say why the recording is
        // over, and stop burning a decoder-buffer round-trip per frame.
        std::fprintf(stderr,
                     "BurnRecorder: encoder refused %llu consecutive frames (%llu encoded so "
                     "far); recording disabled%s\n",
                     static_cast<unsigned long long>(consecutive_fail),
                     static_cast<unsigned long long>(frames_encoded.load()),
                     never_encoded ? " (no file written)" : " (file closed at stop())");
        dead.store(true);
      }
    }
  }

  // Recorder thread, mu HELD: brings osd_work up to date with osd_map,
  // copying only the rows set_osd() marked dirty. The full copy is the
  // fallback for a resize, a blank, or a rect list that outgrew its cap --
  // never the steady state.
  void take_osd_locked() {
    if (osd_full_pending || osd_work.mb_w != osd_map.mb_w || osd_work.mb_h != osd_map.mb_h ||
        osd_work.px.size() != osd_map.px.size()) {
      osd_work = osd_map;
    } else {
      const size_t stride = (size_t)osd_map.stride();
      for (const DirtyRect& r : osd_dirty) {
        for (int y = r.y; y < r.y + r.h; ++y)
          std::memcpy(osd_work.px.data() + (size_t)y * stride + r.x,
                      osd_map.px.data() + (size_t)y * stride + r.x, (size_t)r.w);
      }
    }
    osd_dirty.clear();
    osd_full_pending = false;
    osd_pending_valid = false;
  }

  // Empties the mailbox and the displaced list, releasing every reference
  // they hold (outside box_mu). Called from housekeeping (drop_pending(),
  // stop()); box_mu serialises it against submit() and against the
  // stage/recorder thread's own take.
  void drain_box() {
    void* buf = nullptr;
    Mail old[kDisplaced];
    int n_old = 0;
    {
      std::lock_guard<PiMutex> lk(box_mu);
      buf = box.buf;
      box.buf = nullptr;
      n_old = take_displaced_locked(old);
    }
    if (n_old) {
      // REF-DRAIN, displaced list: flush hygiene too, counted as such.
      put_mails(old, n_old);
      frames_flushed.fetch_add(static_cast<uint64_t>(n_old));
    }
    if (buf) {
      // REF-DRAIN: displaced by a drop_pending()/stop() rather than by a
      // newer frame. The reference accounting is identical, but the MEANING
      // is not, so it gets its own counter: this is flush hygiene (a link
      // discontinuity, a watchdog reset, shutdown), not encoder overload.
      // Sharing one counter let a link-loss storm read as overload -- and
      // let the hardware leak test, which reads frames_dropped(), be
      // satisfied entirely by flushes with the displacement path never
      // exercised at all.
      put_dec(buf);
      frames_flushed.fetch_add(1);
    }
  }
  // Same hygiene for the ct -> encoder handoff.
  void drain_ct_out() {
    CtMail o;
    {
      std::lock_guard<std::mutex> lk(mu);
      o = ct_out;
      ct_out.buf = nullptr;
    }
    if (o.buf) {
      release_ct(o);
      frames_flushed.fetch_add(1);
    }
  }
};

BurnRecorder::BurnRecorder() : impl_(new Impl) {}
BurnRecorder::~BurnRecorder() { stop(); }

void BurnRecorder::set_palette(const OsdPalette& pal) {
  if (impl_->started) {
    std::fprintf(stderr, "BurnRecorder: set_palette() after start(), ignored\n");
    return;
  }
  impl_->palette = pal;
  impl_->have_palette = true;
}

bool BurnRecorder::start(const BurnCfg& cfg, const std::string& path,
                         const VideoBackend* backend) {
  Impl& im = *impl_;
  if (im.started) {
    std::fprintf(stderr, "BurnRecorder: already started\n");
    return false;
  }
  // Backend check ONCE, here, rather than a blind per-frame cast: only
  // MppBackend puts an MppFrame in DmaFrame::opaque, and a burned recording
  // fed by anything else would dereference garbage. The pointer is not kept
  // -- the decode watchdog can destroy and recreate the backend, and the
  // replacement is built from the same config, so it is an MppBackend too.
  if (dynamic_cast<const MppBackend*>(backend) == nullptr) {
    std::fprintf(stderr,
                 "BurnRecorder: dvr.mode \"burned\" needs the mpp backend "
                 "(decoded frames carry no MppFrame otherwise); recording disabled\n");
    return false;
  }
  // !(period_us > 0) rather than <= 0 so a NaN period is refused too.
  if (cfg.width <= 0 || cfg.height <= 0 || cfg.every < 1 || !(cfg.period_us > 0) ||
      cfg.bitrate_kbps <= 0) {
    std::fprintf(stderr, "BurnRecorder: bad BurnCfg %dx%d every %d period %.3f ms %d kbps\n",
                 cfg.width, cfg.height, cfg.every, cfg.period_us / 1000.0, cfg.bitrate_kbps);
    return false;
  }

  im.cfg = cfg;
  im.path = path;
  im.mux_open = false;
  im.mux_failed = false;
  im.dead.store(false);
  im.colortrans_fallbacks.store(0);
  im.ct_n.store(0);
  im.ct_sum_us.store(0);
  im.ct_max_us.store(0);
  im.enc_n.store(0);
  im.enc_sum_us.store(0);
  im.enc_max_us.store(0);
  im.stopping = false;

  EncCfg ec;
  // The encoder's rc rate IS the recorded grid's: one frame per `every`
  // vblank periods. At least 1 so an absurd period cannot hand MPP a zero.
  ec.fps = std::max(1, static_cast<int>(std::llround(1e6 / (cfg.period_us * cfg.every))));
  ec.bitrate_kbps = cfg.bitrate_kbps;
  // The OSD region follows the overlay's SURFACE, which the caller composes
  // at the record size. The picture size is not passed at all: the encoder
  // latches it from the first frame it is handed, which the stage thread
  // has already scaled to the record size (see Impl).
  ec.osd_width = cfg.osd_width;
  ec.osd_height = cfg.osd_height;

  im.enc.reset(new MppEncoder());
  Impl* pim = impl_.get();
  if (!im.enc->init(ec, [pim](const uint8_t* p, size_t n, uint64_t pts, bool key) {
        pim->on_nal(p, n, pts, key);
      })) {
    std::fprintf(stderr, "BurnRecorder: encoder init failed; recording disabled\n");
    im.enc.reset();
    return false;
  }

  // Palette upload before the thread exists, so every later encoder call is
  // on the recorder thread. A failure here is NOT fatal: an OSD-less burned
  // recording is a legal (and useful) outcome, so log and carry on.
  if (im.have_palette) {
    im.palette_live = im.enc->set_palette(im.palette);
    if (!im.palette_live)
      std::fprintf(stderr, "BurnRecorder: palette upload failed; recording without the OSD\n");
  }

#ifndef MABUR_PLAYER_GPU
  if (cfg.colortrans)
    std::fprintf(stderr, "BurnRecorder: colortrans requested but this build has no GPU stage; "
                         "recording FLAT (colortrans_fallbacks will climb)\n");
#endif

  im.idr_pending.store(true);  // consumed before the first encode
  im.ct_out = Impl::CtMail{};
  for (int i = 0; i < Impl::kCtDst; ++i) im.ct_dst_busy[i] = false;
  im.box = Impl::Mail{};
  for (int i = 0; i < Impl::kDisplaced; ++i) im.displaced[i] = Impl::Mail{};
  im.n_displaced = 0;
  im.dec_refs_held.store(0);
  im.enc_latched_geom.store(0);
  im.stage_fwd_geom = Impl::Geom{};
#ifdef MABUR_PLAYER_GPU
  // The record size: every frame of another size is scaled to it (see the
  // stage comment in Impl), whatever the OSD region. Strides at the
  // encoder's 16-px alignment; 1080 rows become a 1088-row buffer.
  im.out_w = cfg.width;
  im.out_h = cfg.height;
  im.out_hs = (cfg.width + 15) & ~15;
  im.out_vs = (cfg.height + 15) & ~15;
  im.ct_on = true;
  im.ct_failed = false;
  im.dst_failed = false;
  im.scale_fail_logs = 0;
#else
  im.out_w = im.out_h = im.out_hs = im.out_vs = 0;
  im.ct_on = false;
  std::fprintf(stderr, "BurnRecorder: this build has no scale stage; it records at the decoded "
                       "size, and a decoded size other than %dx%d is unsupported (its OSD "
                       "records misplaced)\n",
               cfg.width, cfg.height);
#endif
  im.started = true;
  im.th = std::thread([pim] { pim->run(); });
#ifdef MABUR_PLAYER_GPU
  if (im.ct_on) im.ct_th = std::thread([pim] { pim->run_ct(); });
#endif
  std::fprintf(stderr,
               "BurnRecorder: start %dx%d every=%d fps=%d period=%.3fms osd=%dx%d ct=%s "
               "(%d kbps, frag %d ms, palette %s) -> %s\n",
               cfg.width, cfg.height, cfg.every, ec.fps, cfg.period_us / 1000.0, cfg.osd_width,
               cfg.osd_height, cfg.colortrans ? "on" : "off", cfg.bitrate_kbps, cfg.fragment_ms,
               im.palette_live ? "live" : "off", path.c_str());
  return true;
}

void BurnRecorder::submit(const DmaFrame& f, uint64_t pts_us) {
  Impl& im = *impl_;
  if (!im.started || im.dead.load()) return;

  // Thinning already happened in RecorderTap: every call here is a recorded
  // flip, stamped with its grid slot. No clock read, no rate decision.
  MppFrame frame = static_cast<MppFrame>(f.opaque);
  if (!frame) return;
  MppBuffer buf = mpp_frame_get_buffer(frame);
  if (!buf) return;
  im.frames_in.fetch_add(1);

  // box_mu (a PiMutex), never mu: this runs on the SCHED_FIFO release
  // thread and must not wait behind set_osd()'s quantize. Nothing is
  // released here -- the release thread never runs mpp_buffer_put -- so a
  // displaced entry moves to the displaced list for the next taker.
  bool refused = false;
  {
    std::lock_guard<PiMutex> lk(im.box_mu);
    if (im.dec_refs_held.load() >= Impl::kMaxDecRefs ||
        (im.box.buf && im.n_displaced == Impl::kDisplaced)) {
      // REF-REFUSE: the recorder already holds its four decoder buffers, or
      // the taker has not emptied the mailbox for three recorded flips.
      // Holding another buffer would break the bound, so no reference is
      // taken and nothing is posted.
      refused = true;
    } else {
      // REF-SUBMIT: from here the recorder owns a reference; the decoder
      // cannot recycle this buffer until one of the release paths runs.
      // Taken only once refusal is ruled out, and before the frame is
      // visible to a taker.
      mpp_buffer_inc_ref(buf);
      im.dec_refs_held.fetch_add(1);
      // REF-DISPLACE, deferred: latest wins, and the displaced entry keeps
      // its reference until the next take or drain releases it. Dropping
      // it without that leaks one decoder pool buffer per displacement,
      // and the pool is 24 deep.
      if (im.box.buf) im.displaced[im.n_displaced++] = im.box;
      im.box.buf = buf;
      im.box.pts_us = pts_us;
      im.box.w = f.width;
      im.box.h = f.height;
      im.box.stride = f.stride;
      im.box.vstride = f.vstride;
    }
  }
  if (refused) {
    im.frames_dropped.fetch_add(1);
    return;
  }
  im.box_cv.notify_one();
}

void BurnRecorder::set_osd(const Surface& s, const DirtyRect* rects, size_t n_rects) {
  Impl& im = *impl_;
  // No palette => the encoder has no OSD region at all, so quantizing would
  // be pure waste on the housekeeping loop. This is the osd.enable:false path.
  if (!im.started || !im.palette_live || im.dead.load()) return;
  if (!s.pixels || s.width <= 0 || s.height <= 0) return;
  if (rects && n_rects == 0) return;  // nothing changed; do not even publish

  // The lock spans the quantize deliberately: osd_map is the shared object
  // being updated in place, and the alternative (a private map plus a
  // publish copy) puts a 2 MB memcpy back on this loop, which is most of
  // what the incremental path just removed. The recorder thread waits at
  // most one quantize; submit() never does -- the mailbox has its own
  // mutex (box_mu).
  //
  // How long that wait is, measured rather than assumed (the text here used
  // to say "only the rare FULL one is longer than ~10 us", which was wrong
  // by more than two orders of magnitude for the GS overlay):
  //   MSP steady state, a few cells        ~10 us          -- as claimed
  //   GS single field, ~9 k px @1080p      <0.15 ms A55    -- draw included
  //   GS all fields, 151,590 px @1080p     ~1.8 ms A55     -- the quantize
  //   GS all fields, 564,328 px @2160p     ~5.2 ms A55        half alone
  //   full pass, whole surface             ~5.0 ms A55 @1080p
  // (tools/bench/gs_osd_bench.cpp.) So an INCREMENTAL quantize can hold
  // this mutex for milliseconds, and the encoder thread blocks on it for
  // exactly that long. The all-fields cases are why osd_compose.cpp scopes
  // its burn restate to the fields an MSP collision actually hit instead of
  // invalidating the whole burn overlay -- see the comment there.
  std::lock_guard<std::mutex> lk(im.mu);

  // Incremental first; a map that was never sized for this surface (first
  // call, or a resized OSD) refuses and falls back to the full pass.
  bool full = (rects == nullptr);
  if (!full) full = !quantize_rects(s, im.palette, rects, n_rects, &im.osd_map, &im.qcache);
  if (full) {
    quantize(s, im.palette, &im.osd_map, &im.qcache);
    im.osd_dirty.clear();
    im.osd_full_pending = true;
  } else if (!im.osd_full_pending) {
    for (size_t i = 0; i < n_rects; ++i) {
      // Clipped to the surface, because take_osd_locked() copies these
      // straight into osd_work with no bounds check of its own.
      DirtyRect r = rects[i];
      const int x1 = r.x + r.w > s.width ? s.width : r.x + r.w;
      const int y1 = r.y + r.h > s.height ? s.height : r.y + r.h;
      if (r.x < 0) r.x = 0;
      if (r.y < 0) r.y = 0;
      r.w = x1 - r.x;
      r.h = y1 - r.y;
      if (r.w <= 0 || r.h <= 0) continue;
      im.osd_dirty.push_back(r);
      if (im.osd_dirty.size() > Impl::kMaxDirtyRects) {
        // Past this the recorder's row-by-row copy stops being cheaper than
        // taking the whole map.
        im.osd_dirty.clear();
        im.osd_full_pending = true;
        break;
      }
    }
  }
  im.osd_pending_valid = true;
  // Not notified: the map rides along with the next frame the recorder thread
  // picks up. An OSD update with no video to burn it into is a no-op anyway.
}

void BurnRecorder::request_idr() { impl_->idr_pending.store(true); }

void BurnRecorder::drop_pending() {
  // Housekeeping, with the release thread parked (the caller's guarantee,
  // burn_recorder.h). drain_box() takes box_mu only; drain_ct_out() takes
  // mu, so this can wait out one set_osd() quantize -- a flush-time cost,
  // never a per-frame one.
  if (!impl_->started) return;
  impl_->drain_box();
  impl_->drain_ct_out();
}

void BurnRecorder::stop() {
  Impl& im = *impl_;
  if (!im.started) return;
  // The caller has already cleared the tap's hook (THREADING in
  // burn_recorder.h), so no submit() races the teardown below. `stopping` is
  // set under both mutexes because the box waiters read it under box_mu
  // and the ct_out waiter under mu.
  {
    std::scoped_lock lk(im.mu, im.box_mu);
    im.stopping = true;
  }
  im.box_cv.notify_all();
  im.ct_cv.notify_all();
  // ct thread first: it only ever hands frames DOWN to the encoder thread,
  // so once it is gone ct_out is quiescent for the encoder's own exit.
  if (im.ct_th.joinable()) im.ct_th.join();
  if (im.th.joinable()) im.th.join();
  // After the joins nothing else touches the mailboxes, but the drains lock
  // anyway -- they are the code path drop_pending() uses and correctness
  // here must not depend on the joins having happened.
  im.drain_box();     // REF-STOP: the mailbox and the displaced list
  im.drain_ct_out();  // REF-STOP, ct side
#ifdef MABUR_PLAYER_GPU
  im.ct_free_dst();
#endif
  if (const int held = im.dec_refs_held.load())
    std::fprintf(stderr, "BurnRecorder: %d decoder references unaccounted at stop\n", held);

  if (im.mux_open) {
    im.mux.close();
    im.mux_open = false;
  }
  const uint64_t enc_frames = im.enc ? im.enc->frames() : 0;
  const uint64_t enc_errs = im.enc ? im.enc->errors() : 0;
  im.enc.reset();
  im.started = false;
  const StageUs su = stage_us();
  std::fprintf(stderr,
               "BurnRecorder: stopped -- in=%llu encoded=%llu dropped=%llu flushed=%llu "
               "errors=%llu osd_rejects=%llu ct_fallbacks=%llu (encoder frames=%llu errors=%llu) "
               "samples=%llu fragments=%llu ct_ms=%.1f/%.1f enc_ms=%.1f/%.1f (mean/max)\n",
               static_cast<unsigned long long>(im.frames_in.load()),
               static_cast<unsigned long long>(im.frames_encoded.load()),
               static_cast<unsigned long long>(im.frames_dropped.load()),
               static_cast<unsigned long long>(im.frames_flushed.load()),
               static_cast<unsigned long long>(im.encode_errors.load()),
               static_cast<unsigned long long>(im.osd_rejects.load()),
               static_cast<unsigned long long>(im.colortrans_fallbacks.load()),
               static_cast<unsigned long long>(enc_frames),
               static_cast<unsigned long long>(enc_errs),
               static_cast<unsigned long long>(im.mux.samples()),
               static_cast<unsigned long long>(im.mux.fragments()),
               su.ct_n ? su.ct_sum / 1000.0 / su.ct_n : 0.0, su.ct_max / 1000.0,
               su.enc_n ? su.enc_sum / 1000.0 / su.enc_n : 0.0, su.enc_max / 1000.0);
}

bool BurnRecorder::running() const { return impl_->started && !impl_->dead.load(); }

uint64_t BurnRecorder::frames_in() const { return impl_->frames_in.load(); }
uint64_t BurnRecorder::frames_encoded() const { return impl_->frames_encoded.load(); }
uint64_t BurnRecorder::frames_dropped() const { return impl_->frames_dropped.load(); }
uint64_t BurnRecorder::frames_flushed() const { return impl_->frames_flushed.load(); }
uint64_t BurnRecorder::encode_errors() const { return impl_->encode_errors.load(); }
uint64_t BurnRecorder::osd_rejects() const { return impl_->osd_rejects.load(); }
uint64_t BurnRecorder::colortrans_fallbacks() const { return impl_->colortrans_fallbacks.load(); }
BurnRecorder::StageUs BurnRecorder::stage_us() const {
  StageUs s;
  s.ct_n = impl_->ct_n.load();
  s.ct_sum = impl_->ct_sum_us.load();
  s.ct_max = impl_->ct_max_us.load();
  s.enc_n = impl_->enc_n.load();
  s.enc_sum = impl_->enc_sum_us.load();
  s.enc_max = impl_->enc_max_us.load();
  return s;
}

}  // namespace maburplay
