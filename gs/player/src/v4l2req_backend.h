#ifndef MABUR_PLAYER_V4L2REQ_BACKEND_H_
#define MABUR_PLAYER_V4L2REQ_BACKEND_H_

#include <cstdint>
#include <memory>

#include "poll_wait.h"
#include "stream_decoder.h"
#include "video_backend.h"

namespace maburplay {

// HEVC hardware decode on a V4L2 stateless (request API) decoder -- Cedrus on
// the Allwinner H618 -- through libav's v4l2request hwaccel, to DRM-PRIME
// linear NV12. Cross build only (MABUR_PLAYER_V4L2REQ); registered by
// make_backend("v4l2req").
//
// Threads: libav may only be driven from one thread, so every libav call --
// decode, and the frees that return buffers to its pool -- runs on the thread
// that calls submit_au()/start()/append()/abort() (the feed thread, or the
// housekeeping thread when there is none). poll() and release_frame() run
// on the decode-output thread (DecodePump) and only touch a locked handoff:
// decoded frames wait in a ready queue for poll() to hand to the FrameSink;
// released frames wait in a release queue that the next submit_au(),
// start() or append() frees, which is exactly when the decoder needs a
// free buffer again. flush() and the destructor run on the housekeeping
// thread only while the feed and the decode-output thread are parked
// (main.cpp's flush order) and free both queues before resetting or
// freeing the decoder; abort() runs there too, when main.cpp's do_flush
// calls StreamFeeder::on_flush to end a streamed picture that never got
// its own close.
//
// No libav include here, as with mpp_backend.h: null_backend.cpp includes
// this on every v4l2req build.
//
// Streams: the libav it builds against carries mabur's FFmpeg patch
// (gs/player/ffmpeg-patches); without it this file does not compile and
// init() fails -- there is no whole-only fallback.
class V4l2ReqBackend : public VideoBackend, public StreamDecoder, public PollWait {
 public:
  V4l2ReqBackend();
  ~V4l2ReqBackend() override;

  bool init(const BackendCfg&, FrameSink) override;
  void submit_au(const uint8_t* au, size_t n, uint32_t pts_us) override;
  void flush() override;
  void release_frame(const DmaFrame&) override;
  void poll() override;

  // Streamed decode (StreamDecoder): one slice per packet through the
  // patched libav (AV_PKT_FLAG_PARTIAL); each slice reaches Cedrus as its
  // packet arrives and only the last one waits for the picture. Feed thread.
  bool probe() override;
  bool start(const uint8_t* p, size_t n, uint8_t nslices, uint32_t pts_us) override;
  bool append(const uint8_t* p, size_t n, uint32_t pts_us, bool last) override;
  void abort(uint32_t pts_us) override;

  // PollWait: an empty ready queue holds poll() up to ms for the feed
  // thread's next frame, so the decode-output thread sleeps here instead of
  // between polls. After init() (false before); 0 restores a poll() that
  // returns at once.
  bool set_poll_timeout_ms(int ms) override;

  // Diagnostics (read by --decode-only and the feed thread's 5 s line).
  uint64_t frames() const;  // decoded frames delivered by poll() (to the FrameSink, or dropped if none)
  // send/receive failures that are not bitstream damage: device/alloc
  // failure, a rejected packet, a frame in a shape DrmPresenter can't
  // import. Does NOT include AVERROR_INVALIDDATA -- see bitstream().
  uint64_t errors() const;
  uint64_t bitstream() const;      // AVERROR_INVALIDDATA from send/receive: bitstream damage, not a hard failure
  uint64_t dropped_shape() const;  // decoded frames not in DrmPresenter's NV12 shape
  uint64_t gated() const;          // AUs held back waiting for an entry point

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace maburplay

#endif  // MABUR_PLAYER_V4L2REQ_BACKEND_H_
