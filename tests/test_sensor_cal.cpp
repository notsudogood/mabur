// Host test for drone/venc/sensor_cal.c: the boot-time sensor frame-rate
// trim. The simulated sensor reproduces the IMX415 driver's response to a
// milli-fps request (VMAX = round(vts_30fps * max_fps * 1000 / mfps), rate
// = pixel clock / (VMAX * HMAX)) plus the auto-exposure stretch that pins
// VMAX at expo_lines + 8 in dim light.
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "mtest.h"
#include "sensor_cal.h"

namespace {

constexpr double kPixClk = 74.247e6;
constexpr double kHmax = 365.0;
constexpr double kVtsBase = 2250.0 * 90.0 * 1000.0;  // vts_30fps * max_fps * 1000

// The 1472x816@120 mode (mode 3) runs the same line rate with VMAX 1696
// at its 120 fps maximum, so one line at 100 fps is ~49 mHz.
constexpr double kVtsBaseMode3 = 1696.0 * 120.0 * 1000.0;

struct SimSensor {
  double vts_base = kVtsBase;   // the mode's vts_30fps * max_fps * 1000
  uint32_t vmax_floor = 0;      // AE stretch: VMAX never below this
  bool ignore_requests = false; // a driver that cannot change timing
  uint32_t jitter_us = 40;      // spread of the per-frame ISR latency
  uint32_t vmax = 3375;
  double ideal_us = 1e6;        // true capture instant
  uint32_t rng = 0x9e3779b9u;

  void request(uint32_t mfps) {
    if (ignore_requests) return;
    uint32_t v = static_cast<uint32_t>((vts_base + mfps / 2) / mfps);
    vmax = v > vmax_floor ? v : vmax_floor;
  }
  double hz() const { return kPixClk / (vmax * kHmax); }
  // next frame pts: the true capture instant plus a per-frame ISR latency
  // (so consecutive deltas carry zero-median jitter, like the real VIF)
  uint64_t next() {
    ideal_us += 1e6 / hz();
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;  // xorshift32
    const double latency = 5.0 + (rng % jitter_us);         // 5..44 us by default
    return static_cast<uint64_t>(ideal_us + latency);
  }
};

// Drive the calibrator until it locks, gives up, or n frames pass.
// Applies every request to the sim like the runtime would.
SensorCalAction run(SensorCal* c, SimSensor* s, int max_frames) {
  SensorCalAction last = SENSOR_CAL_NONE;
  for (int i = 0; i < max_frames; ++i) {
    SensorCalAction a = sensor_cal_feed(c, static_cast<uint32_t>(s->next()));
    if (a == SENSOR_CAL_REQUEST) s->request(sensor_cal_request_mfps(c));
    if (a != SENSOR_CAL_NONE) last = a;
    if (a == SENSOR_CAL_LOCKED || a == SENSOR_CAL_GAVE_UP) return a;
  }
  return last;
}

}  // namespace

TEST(cal_trims_a_fast_sensor_to_just_under_target) {
  SimSensor s;  // bright scene: VMAX 3375 -> 60.27 Hz
  SensorCal c;
  sensor_cal_init(&c, 60, 60000, 0);
  s.request(60000);
  sensor_cal_arm(&c);
  CHECK(run(&c, &s, 60 * 60) == SENSOR_CAL_LOCKED);
  CHECK(s.vmax == 3391);
  CHECK(s.hz() < 60.0);
  CHECK(s.hz() > 60.0 - 0.03);
  CHECK(std::fabs(sensor_cal_measured_hz(&c) - s.hz()) < 0.01);
}

TEST(cal_converges_when_ae_stretch_hides_the_first_corrections) {
  SimSensor s;
  s.vmax_floor = 3386;  // dim scene: shutter at the frame period
  SensorCal c;
  sensor_cal_init(&c, 60, 60000, 0);
  s.request(60000);
  CHECK(s.vmax == 3386);
  sensor_cal_arm(&c);
  CHECK(run(&c, &s, 60 * 90) == SENSOR_CAL_LOCKED);
  CHECK(s.vmax == 3391);
}

TEST(cal_gives_up_after_the_round_cap_and_restores_the_boot_request) {
  SimSensor s;
  s.ignore_requests = true;  // nothing we ask changes the timing
  SensorCal c;
  sensor_cal_init(&c, 60, 60000, 0);
  sensor_cal_arm(&c);
  CHECK(run(&c, &s, 60 * 120) == SENSOR_CAL_GAVE_UP);
  CHECK(sensor_cal_rounds(&c) == SENSOR_CAL_MAX_ROUNDS);
  // trimming failed, so the sensor is left exactly as the integer boot request set it
  CHECK(sensor_cal_request_mfps(&c) == 60000);
}

TEST(cal_locked_stays_silent_until_rearmed) {
  SimSensor s;
  SensorCal c;
  sensor_cal_init(&c, 60, 60000, 0);
  s.request(60000);
  sensor_cal_arm(&c);
  REQUIRE(run(&c, &s, 60 * 60) == SENSOR_CAL_LOCKED);
  for (int i = 0; i < 60 * 10; ++i)
    CHECK(sensor_cal_feed(&c, static_cast<uint32_t>(s.next())) == SENSOR_CAL_NONE);
  // a new producer rate (fps verb) re-arms; the sensor drifted back
  s.vmax = 3375;
  sensor_cal_arm(&c);
  CHECK(run(&c, &s, 60 * 60) == SENSOR_CAL_LOCKED);
  CHECK(s.vmax == 3391);
}

TEST(cal_measurement_ignores_a_dropped_frame_in_the_window) {
  SimSensor s;
  s.request(59717);  // already at 3391: must lock first window
  SensorCal c;
  sensor_cal_init(&c, 60, 59717, 0);
  sensor_cal_arm(&c);
  SensorCalAction a = SENSOR_CAL_NONE;
  for (int i = 0; i < 60 * 60 && a == SENSOR_CAL_NONE; ++i) {
    uint64_t p = s.next();
    if (i % 50 == 25) p = s.next();  // a hole: one frame never arrives
    a = sensor_cal_feed(&c, static_cast<uint32_t>(p));
  }
  CHECK(a == SENSOR_CAL_LOCKED);
  CHECK(std::fabs(sensor_cal_measured_hz(&c) - s.hz()) < 0.01);
}

TEST(cal_rejected_request_ends_in_gave_up_with_the_last_good_request) {
  SimSensor s;
  SensorCal c;
  sensor_cal_init(&c, 60, 60000, 0);
  s.request(60000);
  sensor_cal_arm(&c);
  SensorCalAction a = SENSOR_CAL_NONE;
  for (int i = 0; i < 60 * 20 && a != SENSOR_CAL_REQUEST; ++i)
    a = sensor_cal_feed(&c, static_cast<uint32_t>(s.next()));
  REQUIRE(a == SENSOR_CAL_REQUEST);
  sensor_cal_request_rejected(&c);  // driver refused milli-fps
  CHECK(sensor_cal_state(&c) == SENSOR_CAL_STATE_GAVE_UP);
  CHECK(sensor_cal_request_mfps(&c) == 60000);
  CHECK(sensor_cal_feed(&c, static_cast<uint32_t>(s.next())) == SENSOR_CAL_NONE);
}

// The bind rule that keeps the SDK's frame-rate limiter out of the path at
// full rate: dst = src whenever the requested rate is the sensor's set
// rate, else the request (low-power decimation).
TEST(bind_dst_is_the_source_rate_at_full_rate) {
  CHECK(sensor_cal_bind_dst_fps(60, 60, 90) == 90);
  CHECK(sensor_cal_bind_dst_fps(90, 60, 90) == 90);   // above full: still src
}

TEST(bind_dst_follows_a_request_below_full_rate) {
  CHECK(sensor_cal_bind_dst_fps(30, 60, 90) == 30);
  CHECK(sensor_cal_bind_dst_fps(15, 60, 90) == 15);
}

TEST(bind_dst_with_no_known_source_rate_is_the_request) {
  CHECK(sensor_cal_bind_dst_fps(60, 60, 0) == 60);
}

// From one line fast (VMAX 3381, the bench's round-2 position) the
// controller must land on the line nearest the target from below (3391,
// 59.988 Hz), not a line lower (3392, 59.970 Hz): every extra line costs
// a shorter beat against the panel.
TEST(cal_lands_on_the_line_nearest_the_target_from_below) {
  SimSensor s;
  s.request(59894);
  REQUIRE(s.vmax == 3381);
  SensorCal c;
  sensor_cal_init(&c, 60, 59894, 0);
  sensor_cal_arm(&c);
  CHECK(run(&c, &s, 60 * 60) == SENSOR_CAL_LOCKED);
  CHECK(s.vmax == 3391);
}

// At 100 fps on mode 3 one line is ~49 mHz, wider than the 60 fps band:
// the rate flips between 99.960 Hz (VMAX 2035) and 100.009 Hz (2034) and
// never lands in a band sized for 18 mHz lines. The band scales with the
// line step, so the line just under the target locks.
TEST(cal_locks_at_100fps_where_one_line_is_wider_than_the_60fps_band) {
  SimSensor s;
  s.vts_base = kVtsBaseMode3;
  s.jitter_us = 4;  // the bench's medians sat on the true period every round
  s.request(100000);
  REQUIRE(s.vmax == 2035);
  SensorCal c;
  sensor_cal_init(&c, 100, 100000, 0);
  sensor_cal_arm(&c);
  CHECK(run(&c, &s, 100 * 60) == SENSOR_CAL_LOCKED);
  CHECK(s.vmax == 2035);
  CHECK(s.hz() < 100.0);
}

// A dim scene pins the AE shutter at its ceiling, and the sensor's frame
// is then the shutter plus ~22 lines of overhead (bench: a 10000 us cap
// at 100 fps held mode 3 at VMAX 2056, 98.94 Hz, whatever was requested).
// Above 90 fps the ceiling leaves that overhead inside the frame, so the
// stretch stays under the line the trim wants and the trim still locks.
TEST(cal_locks_at_100fps_in_a_dim_scene_under_the_shutter_cap) {
  SimSensor s;
  s.vts_base = kVtsBaseMode3;
  s.jitter_us = 4;
  const double line_us = kHmax / kPixClk * 1e6;
  s.vmax_floor = static_cast<uint32_t>(sensor_cal_shutter_cap_us(100) / line_us) + 22;
  s.request(100000);
  SensorCal c;
  sensor_cal_init(&c, 100, 100000, 0);
  sensor_cal_arm(&c);
  CHECK(run(&c, &s, 100 * 60) == SENSOR_CAL_LOCKED);
  CHECK(s.vmax == 2035);
}

// Up to 90 fps the ceiling is the whole frame period, as it always was.
TEST(shutter_cap_is_the_frame_period_up_to_90fps) {
  CHECK(sensor_cal_shutter_cap_us(60) == 16666);
  CHECK(sensor_cal_shutter_cap_us(90) == 11111);
  CHECK(sensor_cal_shutter_cap_us(0) == 0);
}

// A mode whose ceiling sits under its nominal rate (waybeam's 1728x816@120
// is VMAX 1700 at the line rate: 119.66 Hz, and the driver refuses any
// request above 120000 mfps) can never reach the band under 120. The trim
// must settle on the fastest rate the mode has, not give up and leave the
// sensor wherever the last round put it.
TEST(cal_locks_at_the_ceiling_of_a_mode_that_cannot_reach_the_target) {
  SimSensor s;
  s.vts_base = 1700.0 * 120.0 * 1000.0;
  s.jitter_us = 4;
  s.request(120000);
  REQUIRE(s.vmax == 1700);
  SensorCal c;
  sensor_cal_init(&c, 120, 120000, 120000);
  sensor_cal_arm(&c);
  CHECK(run(&c, &s, 120 * 60) == SENSOR_CAL_LOCKED);
  CHECK(sensor_cal_request_mfps(&c) == 120000);
  CHECK(s.vmax == 1700);
}

// While the bind decimates (low-power 30 fps) the encoded stream does not
// carry the sensor's rate, so the runtime holds the controller instead of
// feeding it. A hold must discard the partial window and cost no round:
// after resuming, the first action comes only from a fresh full window of
// full-rate frames and measures the real rate.
TEST(cal_hold_discards_the_partial_window_and_resumes_clean) {
  SimSensor s;
  SensorCal c;
  sensor_cal_init(&c, 60, 60000, 0);
  s.request(60000);
  sensor_cal_arm(&c);
  const int most_of_a_window = static_cast<int>(60 * SENSOR_CAL_WINDOW_US / 1e6 * 0.7);
  for (int i = 0; i < most_of_a_window; ++i)
    REQUIRE(sensor_cal_feed(&c, static_cast<uint32_t>(s.next())) == SENSOR_CAL_NONE);
  sensor_cal_hold(&c);
  for (int i = 0; i < 30 * 20; ++i) s.next(), s.next();  // 20 s decimated, unfed
  // resume: nothing may fire inside most of a fresh window of new frames
  for (int i = 0; i < most_of_a_window; ++i)
    CHECK(sensor_cal_feed(&c, static_cast<uint32_t>(s.next())) == SENSOR_CAL_NONE);
  CHECK(sensor_cal_rounds(&c) == 0);
  SensorCalAction a = SENSOR_CAL_NONE;
  for (int i = 0; i < 60 * 3 && a == SENSOR_CAL_NONE; ++i)
    a = sensor_cal_feed(&c, static_cast<uint32_t>(s.next()));
  CHECK(a == SENSOR_CAL_REQUEST);
  CHECK(std::fabs(sensor_cal_measured_hz(&c) - s.hz()) < 0.01);
}

// Lock time budget: the dim-scene case (AE stretch hides the first two
// corrections) is the slowest path and must still lock within 8 s of
// frames, which only a window of about 1 s allows.
TEST(cal_locks_the_slow_ae_case_within_8_seconds) {
  SimSensor s;
  s.vmax_floor = 3386;
  SensorCal c;
  sensor_cal_init(&c, 60, 60000, 0);
  s.request(60000);
  sensor_cal_arm(&c);
  CHECK(run(&c, &s, 60 * 8) == SENSOR_CAL_LOCKED);
  CHECK(s.vmax == 3391);
}

MTEST_MAIN
