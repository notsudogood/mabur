#include <array>

#include "arq_shadow.h"
#include "mtest.h"

using maburgs::ArqEpisode;
using maburgs::ArqShadow;
using maburgs::ArqShadowCfg;
using maburgs::ArqSnap;

namespace {
// A scripted decoder: tests set what each layer's next sample reads.
struct FakeDecoder {
  std::array<ArqSnap, 2> snap{};
  std::array<int, 2> reads{};
  ArqShadow::SnapFn fn() {
    return [this](int sid) {
      ++reads[static_cast<size_t>(sid)];
      return snap[static_cast<size_t>(sid)];
    };
  }
};

// One burst of `n` bodies of layer sid, 1 ms apart from t0.
double burst(ArqShadow& a, int sid, double t0, int n) {
  for (int i = 0; i < n; ++i) a.on_video_body(sid, t0 + i);
  return t0 + n - 1;  // last body's stamp
}
}  // namespace

TEST(layer_switch_ends_the_burst_and_samples_after_settle) {
  FakeDecoder dec;
  ArqShadow a(ArqShadowCfg{}, dec.fn());
  const double base_end = burst(a, 0, 0, 11);  // 0..10
  a.on_video_body(1, 16);                      // enh starts: base ended at 10
  a.tick(13.9);                                // 10 + 4 settle not reached
  CHECK(a.bursts(0) == 0);
  a.tick(base_end + 4);
  CHECK(a.bursts(0) == 1);
  CHECK(dec.reads[0] == 1);
  CHECK(a.bursts(1) == 0);  // enh burst still on air
}

TEST(tail_copy_from_the_other_card_is_not_a_new_burst) {
  FakeDecoder dec;
  ArqShadow a(ArqShadowCfg{}, dec.fn());
  burst(a, 0, 0, 11);        // base 0..10
  a.on_video_body(1, 16);    // enh starts
  a.on_video_body(0, 12.5);  // card B's late copy of base's tail
  CHECK(dec.reads[0] == 0);  // not sampled early, not a new base burst
  a.on_video_body(1, 17);    // enh continues: no spurious enh burst end
  a.tick(20);
  CHECK(a.bursts(0) == 1);
  CHECK(a.bursts(1) == 0);
}

TEST(probe_ends_the_burst_and_a_stale_probe_copy_does_not) {
  FakeDecoder dec;
  ArqShadow a(ArqShadowCfg{}, dec.fn());
  burst(a, 1, 0, 10);  // enh 0..9
  a.on_probe(10.5);    // probe trails the enh burst
  a.on_probe(10.9);    // card B's copy of the same probe: no burst on air
  a.tick(14.5);
  CHECK(a.bursts(1) == 1);
  burst(a, 0, 16, 5);  // base 16..20
  a.on_probe(11.0);    // a straggling copy stamped before base began
  a.tick(30);          // only silence ends base now (quiet 15 from 20)
  CHECK(a.bursts(0) == 0);
  a.tick(35);
  CHECK(a.bursts(0) == 1);
  CHECK(a.bursts(1) == 1);
}

TEST(same_layer_after_a_gap_samples_before_the_new_burst_is_decoded) {
  FakeDecoder dec;
  ArqShadow a(ArqShadowCfg{}, dec.fn());
  burst(a, 0, 0, 5);  // base-only (enh shed): 0..4
  dec.snap[0].deficit = 7;
  a.on_video_body(0, 40);  // next base burst, no tick in between
  CHECK(a.bursts(0) == 1);  // sampled synchronously, before body 40 decodes
  CHECK(a.short_bursts(0) == 1);
}

TEST(silence_ends_the_burst_on_tick) {
  FakeDecoder dec;
  ArqShadow a(ArqShadowCfg{}, dec.fn());
  burst(a, 0, 0, 3);
  a.tick(10);
  CHECK(a.bursts(0) == 0);
  a.tick(17);  // 2 + 15 quiet
  CHECK(a.bursts(0) == 1);
}

TEST(episode_records_first_peak_growth_duration_and_requests) {
  FakeDecoder dec;
  ArqShadow a(ArqShadowCfg{}, dec.fn());
  dec.snap[0] = ArqSnap{5, 100, 0, 4, 20, 0.5, 4};
  const double t1 = burst(a, 0, 0, 5);    // base ends 4
  burst(a, 1, 10, 5);
  a.tick(15);                             // base sample: short 5, opens
  dec.snap[0].deficit = 8;                // grew: the next burst lost more
  const double t2 = burst(a, 0, 33, 5);   // base ends 37
  burst(a, 1, 43, 5);
  a.tick(48);                             // short 8: peak, growth
  dec.snap[0].deficit = 3;                // repairs arrived in-band
  burst(a, 0, 66, 5);
  burst(a, 1, 76, 5);
  a.tick(81);                             // short 3
  dec.snap[0].deficit = 0;
  dec.snap[0].abandoned = 100;            // nothing lost for good
  const double t4 = burst(a, 0, 99, 5);   // base ends 103
  a.on_video_body(1, 109);
  a.tick(200);                            // back at 0: closes
  auto eps = a.take_episodes();
  REQUIRE(eps.size() == 1);
  const ArqEpisode& e = eps[0];
  CHECK(e.t_open_ms == t1);
  CHECK(e.sid == 0 && e.mcs == 4 && e.bw == 20 && e.bpb == 4);
  CHECK(e.ov == 0.5);
  CHECK(e.d0 == 5);
  CHECK(e.dpk == 8);
  CHECK(e.nack == 3);              // 5, 8, 3: three would-be requests
  CHECK(e.grow_ms == t2 - t1);     // the 8 was sampled at burst 2's end
  CHECK(e.dur_ms == t4 - t1);      // closed by burst 4's sample
  CHECK(e.aband == 0 && e.stale == 0);
  CHECK(a.bursts(0) == 4);
  CHECK(a.short_bursts(0) == 3);
  CHECK(a.bursts(1) == 4);         // three ended by the next base, one by silence
  CHECK(a.short_bursts(1) == 0);
  CHECK(a.take_episodes().empty());
}

TEST(episode_counts_symbols_abandoned_inside_it) {
  FakeDecoder dec;
  ArqShadow a(ArqShadowCfg{}, dec.fn());
  dec.snap[1] = ArqSnap{30, 10, 2, 5, 40, 0.5, 4};
  burst(a, 1, 0, 5);
  a.tick(20);
  dec.snap[1].deficit = 0;
  dec.snap[1].abandoned = 40;       // evicted unrecovered: 30 lost
  dec.snap[1].abandoned_stale = 2;  // none of it transition debris
  burst(a, 1, 50, 5);
  a.tick(80);
  auto eps = a.take_episodes();
  REQUIRE(eps.size() == 1);
  CHECK(eps[0].aband == 30);
  CHECK(eps[0].stale == 0);
  CHECK(eps[0].bw == 40);
}

TEST(counters_restarting_under_the_episode_saturate_at_zero) {
  FakeDecoder dec;
  ArqShadow a(ArqShadowCfg{}, dec.fn());
  dec.snap[0] = ArqSnap{3, 500, 50, 4, 20, 0.5, 4};
  burst(a, 0, 0, 3);
  a.tick(20);
  dec.snap[0] = ArqSnap{0, 1, 0, 4, 20, 0.5, 4};  // decoder rebuilt
  burst(a, 0, 50, 3);
  a.tick(80);
  auto eps = a.take_episodes();
  REQUIRE(eps.size() == 1);
  CHECK(eps[0].aband == 0 && eps[0].stale == 0);
}

TEST(reset_drops_the_open_episode_and_pending_samples) {
  FakeDecoder dec;
  ArqShadow a(ArqShadowCfg{}, dec.fn());
  dec.snap[0].deficit = 4;
  burst(a, 0, 0, 3);
  a.tick(20);  // episode open
  burst(a, 0, 50, 3);
  a.reset();   // new session: burst on air and its pending end forgotten
  dec.snap[0].deficit = 0;
  a.tick(100);
  burst(a, 0, 120, 3);
  a.tick(150);
  CHECK(a.take_episodes().empty());
  CHECK(a.bursts(0) == 2);  // the pre-reset sample + the post-reset burst
}

TEST(summaries_carry_per_layer_denominators_and_skip_idle_layers) {
  FakeDecoder dec;
  ArqShadowCfg cfg;
  cfg.summary_ms = 1000;
  ArqShadow a(cfg, dec.fn());
  a.tick(0);  // arms the first window
  burst(a, 0, 10, 3);
  a.tick(40);
  dec.snap[0].deficit = 2;
  burst(a, 0, 100, 3);
  a.tick(130);
  a.tick(1000);
  auto s = a.take_summaries();
  REQUIRE(s.size() == 1);  // enh never had a burst: no line for it
  CHECK(s[0].sid == 0 && s[0].bursts == 2 && s[0].short_bursts == 1);
  CHECK(s[0].t_ms == 1000);
  a.tick(2000);
  CHECK(a.take_summaries().empty());  // window reset, nothing new
}

TEST(invalid_layers_are_ignored) {
  FakeDecoder dec;
  ArqShadow a(ArqShadowCfg{}, dec.fn());
  a.on_video_body(5, 0);
  a.on_video_body(-1, 1);
  a.tick(100);
  CHECK(dec.reads[0] == 0 && dec.reads[1] == 0);
  CHECK(a.bursts(-1) == 0 && a.bursts(2) == 0);
}

MTEST_MAIN
