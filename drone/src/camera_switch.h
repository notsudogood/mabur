#pragma once
// The GS menu's Camera tab, drone side. The presets and the wire bytes are
// mabur/camera_presets.h; the sensor modes behind them docs/camera-modes.md.
//
// The GS holds a request level-triggered in every RCF (Rcf::cam);
// CameraSwitch turns its CHANGES into one decision each. Applying one is
// main.cpp's job: set_venc_mode_text() on /etc/mabur.toml, load_config() on
// the result (a file that no longer loads is refused, not written), a
// pending marker, a reboot. The next boot either runs video and drops the
// marker (cam::State::Switched) or puts the previous file back and reboots
// (cam::State::RolledBack). Everything here is pure and I/O-free.
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "camera_modes.h"
#include "config.h"
#include "mabur/camera_presets.h"

namespace mabur {

// The FC's arm state as far as a camera switch is concerned. NoFc: MSP is
// off or the FC never answered (a bench drone), so nothing says it flies.
// Stale: it answered once and has gone quiet -- unknown, so refused.
enum class ArmView : uint8_t { NoFc, Disarmed, Armed, Stale };

// Bit i set: preset i's sensor mode (camera_mode_for at its fps) is in
// `modes`, the loaded driver's list.
uint8_t camera_offered_mask(const CameraMode* modes, int n);

// waybeam's star6e IMX415 driver's mode list (docs/camera-modes.md): what a
// driver switch would offer.
inline constexpr CameraMode kStar6eImx415Modes[] = {
    {3840, 2160, 33}, {2816, 1584, 60}, {1920, 1080, 90}, {1728, 972, 100}, {1728, 816, 120}};
inline constexpr int kNumStar6eImx415Modes =
    static_cast<int>(sizeof(kStar6eImx415Modes) / sizeof(kStar6eImx415Modes[0]));

enum class CamAction : uint8_t { None, Apply, Refuse };

struct CamDecision {
  CamAction action = CamAction::None;
  cam::State refused = cam::State::Idle;  // why, with Refuse
  bool driver_switch = false;              // with Apply: boot waybeam's driver for it
};

class CameraSwitch {
 public:
  // One RCF's cam byte against what this boot runs (`cur`, a preset id or
  // cam::kCustom), the presets the loaded driver offers (`now_mask`), those a
  // driver switch would add (`switch_mask`) and the arm state. Decides once
  // per change to a preset id; cam::kNoWish (the GS dropped its request)
  // re-arms, so the same preset asked again later is decided again.
  CamDecision on_wish(uint8_t wish, uint8_t cur, uint8_t now_mask, uint8_t switch_mask,
                      ArmView arm);

  // Counts `preset` as decided already. The boot after a rollback calls it:
  // the GS keeps asking for the preset that failed until a Telem tells it
  // so, and deciding on that request again would switch, fail and reboot
  // once more. The GS dropping its request (kNoWish) re-arms as usual.
  void seed_decided(uint8_t preset) { last_ = preset; }

 private:
  uint8_t last_ = cam::kNoWish;
};

// The legal venc.slices count nearest `want` at `height` lines (the encoder
// cuts whole 64-px CTU rows, so 544 lines take 1, 2, 3, 5 or 9 and not the
// bundle's 4); a tie goes to the larger count. `want` itself when legal.
uint8_t nearest_legal_slices(uint16_t height, uint8_t want);

// `toml` with [venc] size and fps set to w x h @ fps, and venc.slices to
// `slices` (0 = leave the line alone). Existing lines are rewritten in place
// with their comments kept; a missing key goes right under the [venc]
// header; a file with no [venc] gains one at the end.
std::string set_venc_mode_text(const std::string& toml, uint16_t w, uint16_t h, uint16_t fps,
                               uint8_t slices = 0);

// `toml` with `section`'s keys set to the given TOML value literals, the
// same way set_venc_mode_text does it.
std::string set_toml_values(const std::string& toml, const std::string& section,
                            const std::vector<std::pair<std::string, std::string>>& kv);

// The file a switch to `p` writes: `toml` (the current /etc/mabur.toml, which
// loaded as `cur`) with [venc] size/fps set to the preset, venc.slices moved
// to the nearest count the new height allows, and an enabled recording of
// another shape set to the sensor mode's own size. main.cpp still runs
// load_config on the result before writing it.
std::string preset_config_text(const std::string& toml, const Config& cur, const cam::Preset& p);

// The pending-switch marker (/etc/mabur.cam-pending): which preset the boot
// should come up in, and whether the switch also turned waybeam's driver on
// (a rollback turns it back off).
struct CamPending {
  uint8_t preset = cam::kNoWish;
  bool driver_switched = false;
};
std::string format_cam_pending(const CamPending& p);
std::optional<CamPending> parse_cam_pending(const std::string& text);

}  // namespace mabur
