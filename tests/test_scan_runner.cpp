#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "mtest.h"
#include "nhm_busy.h"
#include "scan_runner.h"
using namespace maburgs;
using mabur::scan::Source;

namespace {
struct FakeRadio : ScoutRadio {
  int64_t now = 0;
  std::vector<std::string> calls;
  uint8_t ch = 0;
  int busy_pct_on[256];             // NHM busy % per channel; -1: no NHM window
  uint32_t cca_per_ms_on[256] = {};
  bool retune_ok_on[256];
  int throw_retune_on = -1, throw_read_on = -1;   // a USB control transfer that fails
  uint64_t foreign_per_dwell_on[256] = {};
  uint16_t armed = 0;
  ScoutFrames fr;
  int64_t last_read = 0;
  FakeRadio() {
    std::fill(std::begin(busy_pct_on), std::end(busy_pct_on), -1);
    std::fill(std::begin(retune_ok_on), std::end(retune_ok_on), true);
  }
  bool retune(uint8_t c) override {
    calls.push_back("retune " + std::to_string(c));
    if (c == throw_retune_on) throw std::runtime_error("rtw_write failed");
    if (!retune_ok_on[c]) return false;
    ch = c;
    return true;
  }
  bool arm_nhm_busy(uint16_t p) override {
    calls.push_back("arm " + std::to_string(p));
    armed = p;
    return busy_pct_on[ch] >= 0;
  }
  NhmBusyRead read_nhm_busy() override {
    calls.push_back("nhm");
    NhmBusyRead r;
    r.valid = true;
    r.period = armed;
    r.buckets[0] = static_cast<uint8_t>(100 - busy_pct_on[ch]);
    r.buckets[11] = static_cast<uint8_t>(busy_pct_on[ch]);
    return r;
  }
  ScoutEnergy read_energy(bool with_nhm) override {
    calls.push_back(with_nhm ? "read+nhm" : "read");
    if (with_nhm && ch == throw_read_on) throw std::runtime_error("rtw_read(0f48)");
    ScoutEnergy e;
    e.fa_valid = true;
    e.cca_ofdm = static_cast<uint32_t>((now - last_read) * cca_per_ms_on[ch]);
    last_read = now;
    if (with_nhm) {
      e.floor_valid = true;
      e.floor_dbm = -95;
      fr.foreign += foreign_per_dwell_on[ch];
    }
    return e;
  }
  ScoutEnergy read_energy_scout() override { return ScoutEnergy{}; }
  ScoutFrames frames() const override { return fr; }
};

struct Rig {
  FakeRadio radio;
  ScanRunCfg cfg;
  std::vector<std::string> sleeps;
  std::unique_ptr<ScanRunner> run;
  explicit Rig(std::vector<uint8_t> plan, int rounds = 1) {
    cfg.plan = std::move(plan);
    cfg.rounds = rounds;
    run = std::make_unique<ScanRunner>(
        radio, cfg,
        [this](int ms) {
          radio.now += ms;
          radio.calls.push_back("sleep " + std::to_string(ms));
        },
        [this] { return radio.now; });
  }
  std::vector<ScanDwellOut> all() {
    std::vector<ScanDwellOut> v;
    ScanDwellOut d;
    while (run->step(d)) v.push_back(d);
    return v;
  }
};

CardCaps caps(uint16_t lo, uint16_t hi, bool nhm = true) {
  CardCaps c;
  c.valid = true;
  c.tune5g_lo = lo;
  c.tune5g_hi = hi;
  c.nhm_ok = nhm;
  return c;
}
}  // namespace

TEST(plan_full_band) {
  const auto p = scan_plan(caps(5080, 6165));
  CHECK(p.size() == 28);
  CHECK(p.front() == 36 && p.back() == 177);
  CHECK(std::find(p.begin(), p.end(), 100) != p.end());
  CHECK(std::find(p.begin(), p.end(), 68) == p.end());
  CHECK(std::find(p.begin(), p.end(), 149) != p.end());
}

TEST(plan_empty_without_tune_range) {
  CHECK(scan_plan(caps(0, 0)).empty());
}

TEST(plan_respects_tune_range) {
  const auto p = scan_plan(caps(5170, 5330));
  CHECK((p == std::vector<uint8_t>{36, 40, 44, 48, 52, 56, 60, 64}));
}

TEST(dwell_sequence_matches_scout) {
  Rig r({36});
  r.radio.busy_pct_on[36] = 10;
  r.all();
  const std::string period = std::to_string(nhm_period_4us(r.cfg.dwell_ms));
  const std::vector<std::string> want = {
      "retune 36", "sleep " + std::to_string(r.cfg.settle_ms), "read", "arm " + period,
      "sleep " + std::to_string(r.cfg.dwell_ms), "nhm", "read+nhm"};
  CHECK(r.radio.calls == want);
}

TEST(busy_matches_nhm_busy_pct) {
  Rig r({40});
  r.radio.busy_pct_on[40] = 37;
  const auto v = r.all();
  NhmBusyRead nb;
  nb.valid = true;
  nb.buckets[0] = 63;
  nb.buckets[11] = 37;
  CHECK(v.size() == 1 && v[0].reading && v[0].busy_valid);
  CHECK(v[0].busy_pct == *nhm_busy_pct(nb, r.cfg.busy_dbm));
  CHECK(v[0].floor_valid && v[0].floor_dbm == -95);
}

TEST(retune_failure_marks_no_reading) {
  Rig r({144, 149});
  r.radio.retune_ok_on[144] = false;
  const auto v = r.all();
  CHECK(v.size() == 2);
  CHECK(v[0].ch == 144 && !v[0].reading && !v[0].busy_valid);
  CHECK(v[1].ch == 149 && v[1].reading);
}

TEST(runs_n_rounds_then_done) {
  Rig r({36, 40}, 3);
  const auto v = r.all();
  CHECK(v.size() == 6);
  CHECK(v[0].round == 0 && v[5].round == 2 && v[5].ch == 40);
  ScanDwellOut d;
  CHECK(!r.run->step(d));
}

TEST(rate_is_counters_per_second) {
  Rig r({44});
  r.radio.cca_per_ms_on[44] = 3;
  r.radio.foreign_per_dwell_on[44] = 12;
  const auto v = r.all();
  CHECK(v[0].rate_per_s == 3000.0);
  CHECK(v[0].foreign == 12);
}

TEST(scan_stops_cleanly_mid_round) {
  Rig r({36, 40, 44});
  bool stop = false;
  r.run->set_should_stop([&] { return stop; });
  ScanDwellOut d;
  CHECK(r.run->step(d));
  stop = true;
  const size_t n = r.radio.calls.size();
  CHECK(!r.run->step(d));
  CHECK(r.radio.calls.size() == n);   // no further retune once stopped
}

TEST(card_lost_ends_scan_and_says_so) {
  Rig r({36, 40, 44});
  bool alive = true;
  r.run->set_alive([&] { return alive; });
  ScanDwellOut d;
  CHECK(r.run->step(d) && d.ch == 36);
  alive = false;
  const size_t n = r.radio.calls.size();
  CHECK(!r.run->step(d));
  CHECK(r.run->lost());
  CHECK(r.radio.calls.size() == n);   // no dwell against a gone card
}

// devourer throws when a USB register access fails; on WebUSB that happens
// under heavy RX. One failed dwell is "no reading", never the end of the scan.
TEST(a_throwing_radio_call_is_one_unread_dwell) {
  Rig r({36, 40, 44});
  r.radio.throw_retune_on = 40;
  r.radio.throw_read_on = 44;
  std::vector<ScanDwellOut> v;
  bool threw = false;
  try { v = r.all(); } catch (...) { threw = true; }
  CHECK(!threw);
  CHECK(v.size() == 3);
  CHECK(v[0].reading);
  CHECK(v[1].ch == 40 && !v[1].reading && !v[1].busy_valid);
  CHECK(v[2].ch == 44 && !v[2].reading);
}

TEST(source_from_busy_window) {
  CHECK(scan_source(true, false) == Source::Nhm);
  CHECK(scan_source(false, true) == Source::Estimate);
  CHECK(scan_source(false, false) == Source::RankOnly);
}

// An 8812AU reads an NHM histogram (caps.nhm_ok) but has no timed busy
// window: the scan must rank it, not wait for busy % that never comes.
TEST(histogram_without_busy_window_is_not_nhm) {
  FakeRadio r;   // busy_pct_on[] = -1: arm_nhm_busy() refuses
  CHECK(!probe_nhm_busy(r));
  CHECK(scan_source(probe_nhm_busy(r), false) == Source::RankOnly);
  FakeRadio j3;
  j3.busy_pct_on[0] = 5;
  CHECK(probe_nhm_busy(j3));
}

namespace {
ScanDwellOut dw(uint8_t ch, int round, double busy, double rate = 0, uint32_t foreign = 0) {
  ScanDwellOut d;
  d.ch = ch;
  d.round = round;
  d.reading = true;
  d.busy_valid = busy >= 0;
  d.busy_pct = busy < 0 ? 0 : busy;
  d.rate_per_s = rate;
  d.foreign = foreign;
  return d;
}
}  // namespace

TEST(summarize_worst_and_mean) {
  const auto s = summarize({dw(36, 0, 10, 0, 4), dw(36, 1, 30, 0, 6), dw(100, 0, 2)}, Source::Nhm,
                           nullptr);
  CHECK(s.size() == 2);
  CHECK(s[0].ch == 36 && s[0].reading && s[0].busy_worst == 30 && s[0].busy_mean == 20);
  CHECK(s[0].frames == 10 && !s[0].dfs);
  CHECK(s[1].ch == 100 && s[1].dfs);
}

TEST(summarize_nhm_without_busy_is_no_reading) {
  const auto s = summarize({dw(36, 0, -1)}, Source::Nhm, nullptr);
  CHECK(s.size() == 1 && !s[0].reading);
}

TEST(summarize_j1_uses_baseline) {
  const std::map<uint8_t, double> base = {{36, 100}, {40, 50}};
  // base_max 100, k_sat 20: (1100-100)/2000 = 50 %
  const auto s = summarize({dw(36, 0, -1, 1100), dw(40, 0, -1, 50)}, Source::Estimate, &base);
  CHECK(s[0].reading && s[0].busy_worst == 50);
  CHECK(s[1].reading && s[1].busy_worst == 0);
}

TEST(summarize_j1_without_baseline_rank_only) {
  const auto s = summarize({dw(36, 0, -1, 900), dw(40, 0, -1, 10)}, Source::RankOnly, nullptr);
  CHECK(s[0].reading && s[1].reading);
  CHECK(s[0].busy_worst > s[1].busy_worst);
}

MTEST_MAIN
