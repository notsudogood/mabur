#ifndef MABUR_PLAYER_POLL_WAIT_H_
#define MABUR_PLAYER_POLL_WAIT_H_

namespace maburplay {

// A decoder whose poll() can wait for output instead of returning at once.
// VideoBackend's shape is frozen, so this sits next to it and is reached by
// dynamic_cast (like StreamDecoder). DecodePump sets it so its thread sleeps
// inside the decoder until a frame is ready, rather than spinning or adding
// a fixed sleep's worth of latency to every frame. MppBackend implements it
// (MPP_SET_OUTPUT_TIMEOUT) and so does V4l2ReqBackend (a timed wait on its
// ready queue); a backend without it gets the pump's own short sleep
// between empty polls.
class PollWait {
 public:
  virtual ~PollWait() = default;
  // From now on poll() blocks up to ms when nothing is ready. false = not
  // applied (poll() keeps returning at once).
  virtual bool set_poll_timeout_ms(int ms) = 0;
};

}  // namespace maburplay

#endif  // MABUR_PLAYER_POLL_WAIT_H_
