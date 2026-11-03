#ifndef MABUR_PLAYER_HDMI_TRIM_H_
#define MABUR_PLAYER_HDMI_TRIM_H_

#include <cstdint>
#include <memory>
#include <string>

namespace maburplay {

// Display-side actuator for the phase lock (display.phase_actuator =
// "display", docs/phase-lock.md "Display actuator"): trims the GS's HDMI
// pixel clock so the panel follows the camera, instead of steering the
// camera to the panel.
//
// On the RK3566 the HDMI port's pixel clock (dclk_vop0) and the HDMI PHY's
// reference both come from HPLL, a PMUCRU PLL nothing else uses
// (docs/findings/2026-10-22-hdmi-clock-trim-spike.md). Its rate is
// 24 MHz x (fbdiv + frac / 2^24) / (refdiv x postdiv1 x postdiv2). Writing
// only the 24-bit fraction moves the rate without the sink re-syncing; a
// change of fbdiv (a separate register) makes the PLL overshoot for a
// moment and the sink blanks. The kernel's 148.5 MHz setting is integer
// (fbdiv 99, postdivs 4 x 4), where any slowing crosses fbdiv, so engage()
// first moves to the same rate at postdivs 4 x 3, fbdiv 74 + 0.25 (one
// blank, right after the player's own modeset, which blanks anyway), and
// every later change writes the fraction alone.
//
// A sink driven over a YCbCr 4:2:0 link gets the same 1080p60 mode with
// HPLL at half that, 74.25 MHz (refdiv 2, otherwise the same integer
// setting): the video port runs at half the mode's pixel clock (two pixels
// per clock), and the refresh still scales 1:1 with HPLL. No sink has been
// seen doing it: the HDZero goggles' 74.25 MHz reading under gs22 was the
// 720p60 mode the kernel had on the port before the player's modeset (their
// EDID offers no 4:2:0 and the link reads RGB888); the re-check after the
// first modeset (main.cpp) is what covers that case. The same setting is
// the full-rate one of a 720p mode. Its trim point keeps the 148.5 MHz
// point's VCO (1782 MHz) and N (74 + 0.25) and divides by 6 x 4 instead,
// so the ppm arithmetic is shared.
namespace hpll {

constexpr uint32_t kPmucruBase = 0xfdd00000;  // 4 KiB page
constexpr uint32_t kCon0 = 0x40;  // [14:12] postdiv1, [11:0] fbdiv; hiword write mask
constexpr uint32_t kCon1 = 0x44;  // [12] dsmpd (1 = integer), [10] lock, [8:6] postdiv2,
                                  // [5:0] refdiv; hiword write mask
constexpr uint32_t kCon2 = 0x48;  // [23:0] frac; plain read-modify-write
constexpr uint32_t kLockBit = 1u << 10;
constexpr uint32_t kFracOne = 1u << 24;
constexpr double kRefHz = 24e6;

struct Setting {
  uint32_t fbdiv = 0, postdiv1 = 0, postdiv2 = 0, refdiv = 0;
  bool integer = true;
  uint32_t frac = 0;
};

Setting decode(uint32_t con0, uint32_t con1, uint32_t con2);
double rate_hz(const Setting& s);
// Same divider structure (everything but the fraction).
bool same_point(const Setting& a, const Setting& b);
uint32_t con0_write(uint32_t postdiv1, uint32_t fbdiv);
uint32_t con1_write(bool integer, uint32_t postdiv2, uint32_t refdiv);
uint32_t con2_value(uint32_t old_con2, uint32_t frac);

// Every trim point runs N = kPointFbdiv + 1/4 = 74.25 from the 24 MHz
// reference (VCO 1782 MHz); only the post-dividers differ.
constexpr uint32_t kPointFbdiv = 74;

// A 1920x1080@60 mode (pixel clock 148.5 MHz): HPLL at the full clock, the
// kernel's setting and the trim point ...
inline Setting kernel_148() { return Setting{99, 4, 4, 1, true, 0}; }
inline Setting point_148() { return Setting{kPointFbdiv, 4, 3, 1, false, kFracOne / 4}; }
// ... or at half of it, 74.25 MHz (a 4:2:0 link, see above).
// The same pair is the full-rate setting of a 74.25 MHz mode (720p60/50).
inline Setting kernel_74() { return Setting{99, 4, 4, 2, true, 0}; }
inline Setting point_74() { return Setting{kPointFbdiv, 6, 4, 1, false, kFracOne / 4}; }
constexpr int kClock148Khz = 148500;   // 1920x1080 at 60 or 50 Hz
constexpr int kClock74Khz = 74250;     // 1280x720 at 60 or 50 Hz

// The fraction for `ppm` at a trim point, kept 1/64 away from either
// integer edge (-3157..+9906 ppm). False outside that.
bool frac_for_ppm(double ppm, uint32_t* frac);
double ppm_for_frac(uint32_t frac);

}  // namespace hpll

// Register access to the PMUCRU page. The real one maps /dev/mem; tests
// pass a fake.
class RegIo {
 public:
  virtual ~RegIo() = default;
  virtual uint32_t read(uint32_t off) = 0;
  virtual void write(uint32_t off, uint32_t value) = 0;
};

// /dev/mem mapping of the PMUCRU page; nullptr and *why on failure.
std::unique_ptr<RegIo> open_pmucru(std::string* why);
// /proc/device-tree/compatible names an RK3566 (the only SoC this was
// measured on); false and *why otherwise.
bool soc_is_rk3566(std::string* why);

class HdmiTrim {
 public:
  HdmiTrim(std::unique_ptr<RegIo> io, int max_ppm) : io_(std::move(io)), max_ppm_(max_ppm) {}
  // Back to 0 ppm (the trim point's exact nominal rate) when engaged; the
  // PLL stays at the trim point, so a restart engages without a blank.
  ~HdmiTrim();
  HdmiTrim(const HdmiTrim&) = delete;
  HdmiTrim& operator=(const HdmiTrim&) = delete;

  // Takes the PLL to the trim point for a mode of pixel clock `clock_khz`
  // and sets 0 ppm. A 148.5 MHz mode has two points, HPLL at the full
  // clock or at half (half_rate()), picked by what the PLL reads; a
  // 74.25 MHz mode has the 74.25 MHz point at full rate. A PLL
  // already at a point (a previous run, a modeset at the same rate) is
  // kept; one at the matching kernel setting is moved (*moved = true: the
  // sink re-syncs once); anything else is refused untouched. A move the
  // PLL does not take is undone to the kernel's setting and refused, never
  // retried (a retry loop held the goggles blank for 30 s in the spike).
  bool engage(int clock_khz, std::string* why, bool* moved);
  bool engaged() const { return engaged_; }
  // Back to 0 ppm (the PLL stays at its point) and no longer engaged: the
  // sensor steer takes over. For a sink the trim must leave alone.
  void disengage() {
    if (engaged_) set_ppm(0.0);
    engaged_ = false;
    half_rate_ = false;
  }
  // Engaged with HPLL at half the mode's pixel clock (a 148.5 MHz mode at
  // the 74.25 MHz point).
  bool half_rate() const { return half_rate_; }
  // The trim point's HPLL rate, MHz (148.5 or 74.25); 0 when not engaged.
  double pll_mhz() const { return engaged_ ? pll_mhz_ : 0.0; }

  // Writes the fraction for `ppm`, clamped to +-max_ppm; nothing when it is
  // already in force. Returns the ppm in force afterwards.
  double set_ppm(double ppm);
  double applied_ppm() const { return applied_ppm_; }
  bool saturated() const { return saturated_; }   // the last request was clamped
  uint64_t writes() const { return writes_; }

 private:
  bool wait_lock();

  std::unique_ptr<RegIo> io_;
  const int max_ppm_;
  bool engaged_ = false;
  bool half_rate_ = false;
  double pll_mhz_ = 0.0;
  bool saturated_ = false;
  uint32_t frac_ = 0;
  double applied_ppm_ = 0.0;
  uint64_t writes_ = 0;
};

}  // namespace maburplay

#endif  // MABUR_PLAYER_HDMI_TRIM_H_
