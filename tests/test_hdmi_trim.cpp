#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "hdmi_trim.h"
#include "mtest.h"
#include "phase_lock.h"

using namespace maburplay;
namespace hp = maburplay::hpll;

namespace {

// The PMUCRU's HPLL registers as the hardware treats writes: CON0/CON1 take
// a hiword write mask, CON2 is plain; the lock bit reads 1 unless told not
// to. Every write is recorded in order.
struct FakePll {
  uint32_t con0 = 0, con1 = 0, con2 = 0;
  bool lock = true;
  bool refuse = false;   // the PLL ignores divider writes (a point that does not take)
  std::vector<std::pair<uint32_t, uint32_t>> writes;
};

class FakeIo : public RegIo {
 public:
  explicit FakeIo(FakePll* p) : p_(p) {}
  uint32_t read(uint32_t off) override {
    if (off == hp::kCon0) return p_->con0;
    if (off == hp::kCon1) return (p_->con1 & ~hp::kLockBit) | (p_->lock ? hp::kLockBit : 0u);
    if (off == hp::kCon2) return p_->con2;
    return 0;
  }
  void write(uint32_t off, uint32_t v) override {
    p_->writes.push_back({off, v});
    const uint32_t mask = v >> 16, val = v & 0xffffu;
    if (off == hp::kCon0 && !p_->refuse) p_->con0 = (p_->con0 & ~mask) | (val & mask);
    if (off == hp::kCon1 && !p_->refuse) p_->con1 = (p_->con1 & ~mask) | (val & mask);
    if (off == hp::kCon2) p_->con2 = v;
  }

 private:
  FakePll* p_;
};

// The kernel's 148.5 MHz setting: fbdiv 99, postdivs 4x4, refdiv 1,
// integer. An unrelated high CON2 bit and an unrelated CON1 bit (13) must
// survive every write.
FakePll kernel_pll() {
  FakePll p;
  p.con0 = (4u << 12) | 99u;
  p.con1 = (1u << 13) | (1u << 12) | (4u << 6) | 1u;
  p.con2 = 0x05000000u;
  return p;
}

std::unique_ptr<HdmiTrim> trim_on(FakePll* p, int max_ppm = 1000) {
  return std::make_unique<HdmiTrim>(std::make_unique<FakeIo>(p), max_ppm);
}

}  // namespace

TEST(decode_and_rate_of_the_kernel_setting_and_the_point) {
  const FakePll k = kernel_pll();
  const hp::Setting s = hp::decode(k.con0, k.con1, k.con2);
  CHECK(hp::same_point(s, hp::kernel_148()));
  CHECK(std::fabs(hp::rate_hz(s) - 148.5e6) < 1.0);
  CHECK(std::fabs(hp::rate_hz(hp::point_148()) - 148.5e6) < 1.0);
}

TEST(ppm_maps_to_the_fraction_and_back) {
  uint32_t f = 0;
  REQUIRE(hp::frac_for_ppm(0.0, &f));
  CHECK(f == hp::kFracOne / 4);
  for (double ppm : {-3000.0, -1000.0, -250.0, 1.0, 500.0, 3000.0}) {
    REQUIRE(hp::frac_for_ppm(ppm, &f));
    CHECK(std::fabs(hp::ppm_for_frac(f) - ppm) < 0.05);   // ~0.01 ppm per step
  }
  // Past 1/64 of the integer edge (below -3157 ppm): refused, never an
  // fbdiv crossing.
  CHECK(!hp::frac_for_ppm(-3200.0, &f));
  CHECK(!hp::frac_for_ppm(10000.0, &f));
}

TEST(engage_from_the_kernel_setting_moves_to_the_point_frac_first) {
  FakePll p = kernel_pll();
  auto t = trim_on(&p);
  std::string why;
  bool moved = false;
  REQUIRE(t->engage(148500, &why, &moved));
  CHECK(moved && t->engaged());
  REQUIRE(p.writes.size() >= 3);
  CHECK(p.writes[0].first == hp::kCon2);   // the fraction first,
  CHECK(p.writes[1].first == hp::kCon0);   // then the feedback divider,
  CHECK(p.writes[2].first == hp::kCon1);   // then postdiv2 + fractional mode
  const hp::Setting s = hp::decode(p.con0, p.con1, p.con2);
  CHECK(hp::same_point(s, hp::point_148()) && s.frac == hp::kFracOne / 4);
  CHECK((p.con1 & (1u << 13)) != 0);        // unrelated CON1 bit kept
  CHECK((p.con2 & 0xff000000u) == 0x05000000u);   // unrelated CON2 bits kept
  CHECK(std::fabs(hp::rate_hz(s) - 148.5e6) < 1.0);
  CHECK(std::fabs(t->applied_ppm()) < 0.05);
}

TEST(engage_keeps_a_pll_already_at_the_point) {
  // A previous run (or a modeset at the same rate) left the point, trimmed.
  FakePll p = kernel_pll();
  p.con0 = (4u << 12) | 74u;
  p.con1 = (3u << 6) | 1u;
  uint32_t f = 0;
  REQUIRE(hp::frac_for_ppm(-400.0, &f));
  p.con2 = f;
  auto t = trim_on(&p);
  std::string why;
  bool moved = true;
  REQUIRE(t->engage(148500, &why, &moved));
  CHECK(!moved);
  REQUIRE(p.writes.size() == 1);            // only the fraction, back to 0 ppm
  CHECK(p.writes[0].first == hp::kCon2);
  CHECK(std::fabs(t->applied_ppm()) < 0.05);
}

TEST(engage_refuses_other_modes_and_unknown_settings_untouched) {
  FakePll p = kernel_pll();
  auto t = trim_on(&p);
  std::string why;
  bool moved = false;
  CHECK(!t->engage(244640, &why, &moved));   // 1080p100's reduced-blanking clock
  CHECK(why.find("148500") != std::string::npos);
  CHECK(!t->engage(148352, &why, &moved));   // 59.94
  // The parked 123.75 MHz mode's setting at a 148.5 MHz mode: not ours.
  FakePll q = kernel_pll();
  q.con0 = (1u << 12) | 433u;
  q.con1 = (1u << 12) | (7u << 6) | 12u;
  auto u = trim_on(&q);
  CHECK(!u->engage(148500, &why, &moved));
  CHECK(why.find("not a kernel setting") != std::string::npos);
  CHECK(p.writes.empty() && q.writes.empty());
  CHECK(!t->engaged() && !u->engaged());
  CHECK(t->set_ppm(500.0) == 0.0 && p.writes.empty());   // never writes unengaged
}

TEST(a_point_that_does_not_take_is_undone_and_refused) {
  FakePll p = kernel_pll();
  p.refuse = true;
  auto t = trim_on(&p);
  std::string why;
  bool moved = false;
  CHECK(!t->engage(148500, &why, &moved));
  CHECK(!t->engaged());
  CHECK(why.find("did not take") != std::string::npos);
  // Three writes out, three back (the kernel's setting), and no retry.
  REQUIRE(p.writes.size() == 6);
  CHECK(hp::decode(p.writes[4].second, 0, 0).fbdiv == 99);
  CHECK((p.writes[5].second & (1u << 12)) != 0);   // integer again
}

TEST(set_ppm_writes_only_the_fraction_and_clamps) {
  FakePll p = kernel_pll();
  auto t = trim_on(&p, 1000);
  std::string why;
  bool moved = false;
  REQUIRE(t->engage(148500, &why, &moved));
  p.writes.clear();
  const uint32_t con0 = p.con0, con1 = p.con1;
  CHECK(std::fabs(t->set_ppm(-300.0) + 300.0) < 0.05);
  CHECK(!t->saturated());
  CHECK(std::fabs(t->set_ppm(-2500.0) + 1000.0) < 0.05);   // clamped to the bound
  CHECK(t->saturated());
  CHECK(std::fabs(t->set_ppm(-2600.0) + 1000.0) < 0.05);   // same fraction: no write
  CHECK(std::fabs(t->set_ppm(800.0) - 800.0) < 0.05);
  CHECK(!t->saturated());
  CHECK(p.writes.size() == 3);
  for (const auto& w : p.writes) CHECK(w.first == hp::kCon2);
  CHECK(p.con0 == con0 && p.con1 == con1);
  const hp::Setting s = hp::decode(p.con0, p.con1, p.con2);
  CHECK(std::fabs(hp::rate_hz(s) / 148.5e6 - 1.0 - 800e-6) < 1e-8);
}

TEST(destruction_returns_to_0_ppm_at_the_point) {
  FakePll p = kernel_pll();
  {
    auto t = trim_on(&p);
    std::string why;
    bool moved = false;
    REQUIRE(t->engage(148500, &why, &moved));
    t->set_ppm(-600.0);
  }
  const hp::Setting s = hp::decode(p.con0, p.con1, p.con2);
  CHECK(hp::same_point(s, hp::point_148()) && s.frac == hp::kFracOne / 4);
}

// --- Half rate: HPLL at 74.25 MHz under a 148.5 MHz mode (HDZero goggles).

namespace {
// What the HDZero goggles' log read: fbdiv 99 (integer), postdivs 4x4,
// refdiv 2 = 74.250 MHz. Same unrelated bits as kernel_pll().
FakePll kernel74_pll() {
  FakePll p = kernel_pll();
  p.con1 = (1u << 13) | (1u << 12) | (4u << 6) | 2u;
  return p;
}
}  // namespace

TEST(the_four_settings_are_distinct_and_the_points_share_n) {
  const hp::Setting all[] = {hp::kernel_148(), hp::point_148(), hp::kernel_74(), hp::point_74()};
  for (int i = 0; i < 4; ++i)
    for (int j = i + 1; j < 4; ++j) CHECK(!hp::same_point(all[i], all[j]));
  CHECK(std::fabs(hp::rate_hz(hp::kernel_74()) - 74.25e6) < 1.0);
  CHECK(std::fabs(hp::rate_hz(hp::point_74()) - 74.25e6) < 1.0);
  for (const hp::Setting& p : {hp::point_148(), hp::point_74()}) {
    CHECK(p.fbdiv == hp::kPointFbdiv && p.frac == hp::kFracOne / 4 && p.refdiv == 1);
    // VCO 1782 MHz at both points: only the post-dividers differ.
    CHECK(std::fabs(hp::rate_hz(p) * p.postdiv1 * p.postdiv2 - 1782e6) < 10.0);
  }
}

TEST(half_rate_engages_from_the_kernel_setting_and_trims_alike) {
  FakePll p = kernel74_pll();
  CHECK(hp::same_point(hp::decode(p.con0, p.con1, p.con2), hp::kernel_74()));
  auto t = trim_on(&p);
  std::string why;
  bool moved = false;
  REQUIRE(t->engage(148500, &why, &moved));
  CHECK(moved && t->engaged() && t->half_rate());
  REQUIRE(p.writes.size() >= 3);
  CHECK(p.writes[0].first == hp::kCon2);
  CHECK(p.writes[1].first == hp::kCon0);
  CHECK(p.writes[2].first == hp::kCon1);
  hp::Setting s = hp::decode(p.con0, p.con1, p.con2);
  CHECK(hp::same_point(s, hp::point_74()) && s.frac == hp::kFracOne / 4);
  CHECK((p.con1 & (1u << 13)) != 0);
  CHECK((p.con2 & 0xff000000u) == 0x05000000u);
  CHECK(std::fabs(t->applied_ppm()) < 0.05);
  // A trim moves the half-rate clock by the same ppm, fraction only.
  p.writes.clear();
  CHECK(std::fabs(t->set_ppm(-800.0) + 800.0) < 0.05);
  REQUIRE(p.writes.size() == 1);
  CHECK(p.writes[0].first == hp::kCon2);
  s = hp::decode(p.con0, p.con1, p.con2);
  CHECK(std::fabs(hp::rate_hz(s) / 74.25e6 - 1.0 + 800e-6) < 1e-8);
}

TEST(half_rate_point_is_kept_on_restart) {
  FakePll p = kernel74_pll();
  {
    auto t = trim_on(&p);
    std::string why;
    bool moved = false;
    REQUIRE(t->engage(148500, &why, &moved));
    t->set_ppm(500.0);
  }   // exit: 0 ppm, still at the half-rate point
  p.writes.clear();
  auto t = trim_on(&p);
  std::string why;
  bool moved = true;
  REQUIRE(t->engage(148500, &why, &moved));
  CHECK(!moved && t->half_rate());
  CHECK(p.writes.empty());   // already at 0 ppm: nothing to write
}

TEST(a_half_rate_point_that_does_not_take_goes_back_to_refdiv_2) {
  FakePll p = kernel74_pll();
  p.refuse = true;
  auto t = trim_on(&p);
  std::string why;
  bool moved = false;
  CHECK(!t->engage(148500, &why, &moved));
  CHECK(!t->engaged() && !t->half_rate());
  REQUIRE(p.writes.size() == 6);
  // The undo restores this sink's kernel setting, not the full-rate one.
  CHECK(hp::decode(p.writes[4].second, 0, 0).fbdiv == 99);
  CHECK((p.writes[4].second >> 12 & 0x7u) == 4u);
  const uint32_t con1 = p.writes[5].second & 0xffffu;
  CHECK((con1 & 0x3fu) == 2u);               // refdiv 2
  CHECK(((con1 >> 6) & 0x7u) == 4u);         // postdiv2 4
  CHECK((con1 & (1u << 12)) != 0);           // integer
}

TEST(a_720p_mode_uses_the_74_point_at_full_rate) {
  FakePll p = kernel74_pll();
  auto t = trim_on(&p);
  std::string why;
  bool moved = false;
  REQUIRE(t->engage(74250, &why, &moved));
  CHECK(moved && !t->half_rate());
  CHECK(std::fabs(t->pll_mhz() - 74.25) < 1e-6);
  CHECK(hp::same_point(hp::decode(p.con0, p.con1, p.con2), hp::point_74()));
  // A 720p mode over a PLL at the 148.5 MHz setting is not a setting for it.
  FakePll q = kernel_pll();
  auto u = trim_on(&q);
  CHECK(!u->engage(74250, &why, &moved));
  CHECK(why.find("not a kernel setting") != std::string::npos && q.writes.empty());
  FakePll r = kernel_pll();
  r.con0 = (4u << 12) | 74u;   // the 148.5 MHz point: still not for a 720p mode
  r.con1 = (3u << 6) | 1u;
  r.con2 = hp::kFracOne / 4;
  auto w = trim_on(&r);
  CHECK(!w->engage(74250, &why, &moved) && r.writes.empty());
}

TEST(pll_mhz_names_the_point_and_disengage_returns_to_0_ppm) {
  FakePll p = kernel_pll();
  auto t = trim_on(&p);
  CHECK(t->pll_mhz() == 0.0);
  std::string why;
  bool moved = false;
  REQUIRE(t->engage(148500, &why, &moved));
  CHECK(std::fabs(t->pll_mhz() - 148.5) < 1e-6 && !t->half_rate());
  t->set_ppm(-700.0);
  t->disengage();
  CHECK(!t->engaged() && t->pll_mhz() == 0.0);
  const hp::Setting s = hp::decode(p.con0, p.con1, p.con2);
  CHECK(hp::same_point(s, hp::point_148()) && s.frac == hp::kFracOne / 4);
  p.writes.clear();
  CHECK(t->set_ppm(300.0) == 0.0 && p.writes.empty());   // no writes once disengaged
}

// --- PhaseLock::set_applied: a bounded actuator must not wind the centre up.

namespace {
// A camera whose rate needs +120 mfps against the panel, followed through
// an actuator that can apply at most +-cap mfps. Returns the lock's centre
// after `secs` of 60 fps frames.
double run_capped(int32_t cap, bool feed_back, double secs) {
  PhaseLock pl(4.0);
  const double need = 120.0, period = 16'667.0;
  double phase = 0.0;   // µs of error against the target margin
  int32_t in_force = 0;
  uint64_t t = 1'000'000;
  for (int i = 0; i < static_cast<int>(secs * 60); ++i, t += 16'667) {
    // An offset short of the need drifts the phase at (in_force - need) / 60 ms per s.
    phase += (static_cast<double>(in_force) - need) * 1000.0 / 60.0 * (period / 1e6);
    double e = phase - period * std::round(phase / period);
    pl.on_margin(4000.0 + e, period, t);
    if (pl.tick(t)) {
      const int32_t want = pl.offset_mfps();
      in_force = std::max(-cap, std::min(cap, want));
      if (feed_back && in_force != want) pl.set_applied(in_force);
    }
  }
  return pl.centre_mfps();
}
}  // namespace

TEST(set_applied_keeps_the_centre_on_the_real_need) {
  const double fed = run_capped(60, true, 40.0);
  const double blind = run_capped(60, false, 40.0);
  CHECK(std::fabs(fed - 120.0) < 25.0);   // the true need, though out of reach
  CHECK(blind > 180.0);                   // wound up toward the +200 range
}

MTEST_MAIN
