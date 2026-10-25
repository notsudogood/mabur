#ifndef MABUR_PLAYER_BURN_RECORDER_H_
#define MABUR_PLAYER_BURN_RECORDER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "osd_palette.h"    // OsdPalette
#include "osd_raster.h"     // Surface, DirtyRect
#include "video_backend.h"  // DmaFrame, VideoBackend

namespace maburplay {

class ColorTrans;

// Encoder-side recording settings.
//
// width/height are the recording's picture size. The stage thread scales
// every frame of any other size to it, letterboxed if the aspect differs
// (fit_rect.h), and restrides a frame whose strides differ from what the
// encoder latched, so the encoder -- which latches its geometry, strides
// included, from the first frame it is handed -- always sees width x height
// at one geometry, and the mux's track header follows. A decoded size that
// changes mid-recording (a rung change, a 720p drone) is scaled like any
// other mismatch. The scale is part of the stage thread, so it needs a
// MABUR_PLAYER_GPU build; without one the recording is at the decoded size
// and a different width x height is unsupported (logged at start()).
//
// osd_width/osd_height size the encoder's OSD region, and must equal the
// size of the SURFACE set_osd() is called with: a region that disagrees
// with the maps rejects every one of them -- a recording with no OSD at
// all, which looks exactly like a deliberate plain transcode. The caller
// sets them to width/height when an overlay is live, since the burned
// overlay is composed at the recording's size; 0 means "no OSD" and is a
// legal configuration.
//
// The recording samples the display's vblank grid: one flip in every
// `every` vblank periods (RecorderTap/GlassCadence do the thinning before
// submit()), so the encoder's rate-control fps is
// round(1e6 / (period_us x every)).
struct BurnCfg {
  int width = 0, height = 0;          // the recording's picture size (the stage scales to it)
  int osd_width = 0, osd_height = 0;  // == width/height when an overlay is live, else 0
  double period_us = 16666.667;       // the panel's vblank period
  int every = 1;                      // grid = every x period; fps = round(1e6 / (period x every))
  int bitrate_kbps = 12000;
  int fragment_ms = 1000;             // DvrMux fragment cut, same units as the raw path

  // colortrans (docs/colortrans.md): non-null = run the GPU stage on every
  // recorded frame before encode(). Non-owning; main.cpp owns the evaluator
  // and it outlives the recorder. Only consulted by a MABUR_PLAYER_GPU
  // build; otherwise every frame counts as a colortrans fallback.
  const ColorTrans* colortrans = nullptr;
};

// dvr.mode "burned": re-encodes the frames the display flips to, with the
// MSP OSD composited in by the hardware encoder (vepu540), and muxes the
// result to fMP4. Cross build only (MABUR_PLAYER_HW) -- it reaches into
// DmaFrame::opaque as an MppFrame, which only MppBackend produces.
//
// ---------------------------------------------------------------------------
// FRAME OWNERSHIP: no copy anywhere. The frame the recorder sees is the one
// the display just flipped to: the presenter's flip sink hands it to
// RecorderTap on the release thread, and the tap calls submit() on a
// recorded slot of the vblank grid. The presenter and this recorder are
// decoupled by MPP's own buffer refcount, NOT by copying and NOT by changing
// who owns the DmaFrame:
//
//   flip (release thread): tap -> burn->submit(f, pts)  // mpp_buffer_inc_ref()
//                                                       // on the frame's
//                                                       // buffer, O(1), no copy
//   presenter later: release_frame(f) on the next flip -> drops ITS ref
//   stage/recorder thread: encode from that buffer, then mpp_buffer_put()
//
// The presenter holds the on-screen frame until the next flip replaces it,
// so the buffer is live for the whole submit() call. The decoder cannot
// recycle it while either holder has a reference, so the presenter's
// contract (it owns the handle, releases on flip, obeys the flush ordering)
// is untouched. Cost: at most four decoder buffers, of the decoder's
// 24-buffer external group, in every mode -- typically one in the mailbox,
// up to two displaced and awaiting release, one being staged or encoded
// (the stage releases its source as soon as its own scaled or corrected
// copy exists). A frame the stage passes through FLAT (already in the
// encoder's geometry with colortrans off, or a failed GPU pass) stays a
// decoder reference through the stage's handoff and encode(), so the
// places alone do not bound it: the recorder counts every decoder
// reference it holds (+1 in submit(), -1 at each release) and submit()
// takes no reference at four, as it takes none when the displaced list is
// full. Either refusal counts the flip as dropped.
//
// The GPU stage READS the decoder's frame, and panfrost fences every buffer
// in a job as if it wrote it. DrmPresenter carries an explicit, already
// signalled IN_FENCE_FD on the video plane precisely so a display commit of
// a frame the stage is reading does not wait for the GPU (drm_presenter.cpp,
// make_signalled_sync_file). Without it, recording cost one vsync at p99.
//
// EVERY reference taken in submit() is released on exactly one path:
//   encoded-then-put  the stage or the encoder finished with it;
//   displaced-then-put  a newer flip displaced it from the mailbox onto the
//                     displaced list, and the thread that next takes the
//                     mailbox (stage or recorder) releases it -- never the
//                     release thread, which must not run mpp_buffer_put;
//   drained           drop_pending() or stop()/~BurnRecorder() empties the
//                     mailbox AND the displaced list.
// A flip that finds the displaced list full, or the recorder already
// holding four decoder references, is REFUSED: no reference is taken at
// all, and it counts as dropped. Losing a reference leaks a pool
// buffer; ~24 leaks stall decode outright. frames_dropped() is the
// observable that proves DISPLACEMENT specifically is handled: it climbs
// under overload while the decoder keeps running at full rate. The flush
// paths are counted separately (frames_flushed()) so they cannot stand in
// for that proof.
//
// ---------------------------------------------------------------------------
// THREADING
//   submit() runs on the release thread (SCHED_FIFO), from RecorderTap's
//     hook, once per recorded flip. It does one mpp_buffer_inc_ref() (MPP's
//     buffer-group lock, for microseconds) and a post to the mailbox under
//     box_mu, a PiMutex. It never releases a buffer and never waits on the
//     other mutex. drop_pending() and start(), stop(), set_osd(),
//     set_palette() and request_idr() run on housekeeping (the main loop).
//     None of them blocks on the encoder.
//   Two mutexes: the mailbox has its own (held for a few field copies by
//     submit(), drop_pending() and the thread taking the frame -- every
//     buffer release happens after it is dropped), and everything else
//     shared -- the OSD map, the stage handoff -- sits under the other.
//     set_osd() holds the second across its quantize (up to milliseconds),
//     so submit() never waits behind it.
//   Ordering rule for the release thread: submit() is legal only between
//     start() returning and stop() being called, and the caller orders it
//     against both. Housekeeping installs the tap's hook
//     (RecorderTap::set, recorder_tap.h) after start() returns and removes
//     it -- RecorderTap::clear() -- BEFORE stop() and before destroying the
//     recorder. The tap holds its PiMutex for the duration of every hook
//     call, so once clear() returns no submit() is in flight and none can
//     start; that lock is also what orders start()'s writes before the
//     first submit(). Nothing stronger is relied on: stop() does not itself
//     fence out a concurrent submit().
//   The recorder thread owns the MppEncoder and the DvrMux outright: every
//     MppEncoder call except init()/set_palette() (which run inside start(),
//     before the thread exists) happens there, which is what MppEncoder's
//     "set_osd() and encode() from ONE thread" contract requires.
//   The file itself is written by a third thread, mabur::ThreadedSink's
//     writer: the mux serialises each closing fragment on the recorder
//     thread and hands the bytes over, so the card's write latency never
//     counts against the encoder's frame budget.
//   The handoff is a single-slot mailbox, latest-wins: if the encoder falls
//     behind, the older frame is displaced and its reference released by
//     the next taker. Never a growing queue -- a queue under sustained
//     overload turns into unbounded buffer retention, i.e. decoder
//     starvation, which is the one thing burned mode must never do to the
//     video path. The displaced list is bounded (two) for the same reason.
//   In a MABUR_PLAYER_GPU build a second thread, the stage (ct) thread,
//     sits between the mailbox and the recorder thread: it owns the EGL
//     context (thread-bound) and the GPU draw + RGA into one of three
//     destination buffers, and hands the result over through a second
//     single-slot latest-wins mailbox. It scales every frame of another
//     size to the record size (BurnCfg), so it runs whether colortrans is
//     on or not. Stage times overlap, so the DVR runs at max(GPU stage,
//     encode) rather than the sum -- ~35 ms serial at 1080p meant 26-32 fps
//     and a third of the frames dropped; overlapped it holds the recorded
//     rate with <1 % drops (bench). Drops at either mailbox count in
//     frames_dropped().
class BurnRecorder {
 public:
  BurnRecorder();
  ~BurnRecorder();
  BurnRecorder(const BurnRecorder&) = delete;
  BurnRecorder& operator=(const BurnRecorder&) = delete;

  // Stages the OSD palette. MUST be called before start(), which uploads it
  // to the encoder before the recorder thread exists (MPP wants the palette
  // installed before the first frame, and this keeps every encoder call on
  // one thread). Never calling it is a legal configuration -- osd.enable
  // false with dvr.mode burned records a plain transcode, and the encoder
  // then runs with no OSD region at all; set_osd() becomes a cheap no-op.
  void set_palette(const OsdPalette& pal);

  // Creates the encoder, requests an opening IDR and starts the thread.
  // `backend` must be the live MppBackend: burned mode is refused (logged,
  // false) for any other backend, because DmaFrame::opaque is only an
  // MppFrame under that one. The pointer is checked and NOT retained -- the
  // decode watchdog may destroy and recreate the backend mid-run.
  //
  // `path` is the fMP4 file. It is NOT created here: the mux opens on the
  // first encoded keyframe, because MppEncoder::header() (the source of the
  // hvcC sample entry) is empty until the first successful encode() latches
  // the geometry. An encoder that never encodes anything therefore leaves no
  // zero-frame file behind.
  bool start(const BurnCfg& cfg, const std::string& path, const VideoBackend* backend);

  // Release thread, from RecorderTap's hook on a recorded flip (see
  // THREADING for the ordering rule against start()/stop()). pts_us is the
  // flip's slot on the vblank grid, which stamps the recording. Takes a
  // buffer reference and posts to the mailbox. Never releases a buffer: a
  // displaced entry goes to the displaced list (two deep), released by the
  // thread that next takes the mailbox; a full list, or four decoder
  // references already held, means no reference is taken and the flip
  // counts as dropped. O(1), never touches pixels.
  void submit(const DmaFrame& f, uint64_t pts_us);

  // Housekeeping, whenever a fresh OSD screen has been rasterized. Quantizes
  // `s` against the staged palette on the CALLER's thread and publishes the
  // index map to the recorder thread, which installs it into the encoder's
  // double-buffered OSD slot before its next frame. No-op without a palette.
  //
  // THIS RUNS ON THE 2 ms MAIN LOOP, so it keeps a PERSISTENT index map and
  // re-quantizes only what changed -- the same bargain OsdRaster strikes
  // with its ShadowGrid, for the same reason. Pass the rects
  // OsdRaster::diff() produced for THIS map's own ShadowGrid; an empty list
  // is a no-op. `rects == nullptr` (the default) re-quantizes the whole
  // surface, which is what a blank or a first screen wants.
  //
  // Cost, measured at 1080p with the shipped font (tools/bench/
  // quantize_bench.cpp, x86 host; the RK3566 A55 runs ~7.4x slower):
  //   incremental, 5 cells   0.010 ms   <- the steady-state path, ~4 Hz
  //   full, typical screen   0.66 ms    <- first screen / layout change
  //   full, blank surface    0.42 ms    <- the stale-OSD blank
  // The pre-fix implementation was 5.5 ms per call, unconditionally.
  void set_osd(const Surface& s, const DirtyRect* rects = nullptr, size_t n_rects = 0);

  // Makes the recording's next encoded frame an IDR. Sets a flag consumed on
  // the recorder thread (MppEncoder is single-threaded), so it is safe from
  // housekeeping. Use it wherever the decode path is reset -- flush_before,
  // the decode watchdog -- so the recording reseals after a discontinuity.
  void request_idr();

  // Drops the mailbox entry and the displaced list (releasing their buffer
  // references) without stopping. Call it wherever the presenter's held
  // frames are dropped -- before backend->flush() and before a watchdog
  // decoder recreation -- so the recorder is not holding decoder buffers
  // across a reset either. The caller guarantees no submit() races it (the
  // release thread is parked): a frame posted just after the drain would be
  // held across the very reset this precedes. The frame the stage or the
  // encoder is working on right now cannot be recalled, so this narrows the
  // window rather than closing it. From housekeeping it can wait out one
  // set_osd() quantize (it empties the stage handoff under that mutex) -- a
  // flush-time cost only.
  //
  // Holding a buffer across a group teardown is BENIGN, so this is hygiene
  // rather than a guard: mpp_buffer_group_put()/clear() on a group with a
  // buffer still referenced does not free it -- service_put_group()
  // (mpp_buffer_impl.c) logs, moves the group to the orphan list, and the
  // last mpp_buffer_put() reaps it. Cost is two mpp_err lines and a slightly
  // late DMA free. That matters because one such teardown is NOT reachable
  // from here at all: MppBackend acks a mid-stream resolution change by
  // calling mpp_buffer_group_clear() inside its own drain loop
  // (mpp_backend.cpp), an event the main loop never sees and cannot precede
  // with a drop_pending().
  void drop_pending();

  // Joins the thread, releases any buffer still held, and closes the mux.
  // Idempotent; also run by the destructor.
  void stop();

  bool running() const;  // started, and not disabled by a fatal encode path

  // Counters. Once stopped, frames_in == frames_encoded + frames_dropped +
  // frames_flushed + encode_errors:
  //   frames_in      flips tapped after thinning (every submit() that
  //                  reached the post decision, refused ones included)
  //   frames_encoded encode() calls that produced a packet
  //   frames_dropped flips DISPLACED in a mailbox by a newer one, REFUSED
  //                  (a full displaced list, or four decoder references
  //                  already held), dropped because the stage could not
  //                  scale or restride them to the encoder's geometry, or
  //                  dropped because the recorder had given up -- i.e. the
  //                  OVERLOAD signal (the stage or the encoder fell behind,
  //                  or the stage failed), and the proof that displacement
  //                  releases its buffer reference. Thinning
  //                  happens before submit() and is deliberately NOT
  //                  counted here.
  //   frames_flushed flips released by drop_pending()/stop(), i.e.
  //                  flush HYGIENE -- a link discontinuity, a watchdog reset,
  //                  shutdown. Separate from frames_dropped on purpose: they
  //                  shared a counter, so a link-loss flush storm read as
  //                  encoder overload, and the hardware leak test (which
  //                  requires frames_dropped() to climb) could be satisfied
  //                  by flushes alone with the displacement path never run.
  //   encode_errors  frames the encoder refused
  //   osd_rejects    index maps the encoder refused because they disagree
  //                  with its OSD region. Nonzero means the recording is a
  //                  plain transcode with NO overlay -- which is a legal
  //                  configuration and therefore invisible without this
  //                  counter.
  uint64_t frames_in() const;
  uint64_t frames_encoded() const;
  uint64_t frames_dropped() const;
  uint64_t frames_flushed() const;
  uint64_t encode_errors() const;
  uint64_t osd_rejects() const;
  //   colortrans_fallbacks  recorded frames encoded FLAT because the GPU
  //                  stage was unavailable or failed (or the binary has no
  //                  stage). Nonzero with colortrans on = a silently
  //                  uncorrected recording, so it is on the fps-log line.
  uint64_t colortrans_fallbacks() const;
  // Recorder-thread stage timing, cumulative since start(): the colortrans
  // stage (GPU draw + RGA, only frames that ran it) and encode() (every
  // recorded frame). Sums and maxima in microseconds; the fps-log differences
  // consecutive reads for a per-window mean. Zero when the stage never ran.
  struct StageUs {
    uint64_t ct_n = 0, ct_sum = 0, ct_max = 0;
    uint64_t enc_n = 0, enc_sum = 0, enc_max = 0;
  };
  StageUs stage_us() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace maburplay

#endif  // MABUR_PLAYER_BURN_RECORDER_H_
