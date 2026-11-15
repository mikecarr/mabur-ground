#include "scan_runner.h"

#include <algorithm>
#include <exception>
#include <optional>

#include "nhm_busy.h"

namespace maburgs {

std::vector<uint8_t> scan_plan(const CardCaps& caps) {
  std::vector<uint8_t> out;
  auto add = [&](int lo, int hi) {
    for (int ch = lo; ch <= hi; ch += 4) {
      const int mhz = 5000 + 5 * ch;
      if (mhz >= caps.tune5g_lo && mhz <= caps.tune5g_hi) out.push_back(static_cast<uint8_t>(ch));
    }
  };
  add(36, 64);
  add(100, 144);
  add(149, 177);
  return out;
}

bool probe_nhm_busy(ScoutRadio& radio) { return radio.arm_nhm_busy(nhm_period_4us(10)); }

mabur::scan::Source scan_source(bool nhm_busy_ok, bool have_baseline) {
  if (nhm_busy_ok) return mabur::scan::Source::Nhm;
  return have_baseline ? mabur::scan::Source::Estimate : mabur::scan::Source::RankOnly;
}

ScanRunner::ScanRunner(ScoutRadio& radio, ScanRunCfg cfg, std::function<void(int)> sleep_ms,
                       std::function<int64_t()> now_ms)
    : radio_(radio), cfg_(std::move(cfg)), sleep_(std::move(sleep_ms)), now_(std::move(now_ms)) {}

bool ScanRunner::step(ScanDwellOut& out) {
  if (stop_.load(std::memory_order_acquire) || lost_ || cfg_.plan.empty() || round_ >= cfg_.rounds)
    return false;
  if (alive_ && !alive_()) {
    lost_ = true;
    return false;
  }
  out = ScanDwellOut{};
  out.ch = cfg_.plan[idx_];
  out.round = round_;
  if (++idx_ == cfg_.plan.size()) {
    idx_ = 0;
    ++round_;
  }
  // devourer throws when a USB register access fails, which WebUSB does under
  // heavy RX. One failed dwell reads as "no reading"; a card that is really
  // gone shows up through alive() before the next dwell.
  try {
    dwell_(out);
  } catch (const std::exception&) {
    const uint8_t ch = out.ch;
    const int round = out.round;
    out = ScanDwellOut{};
    out.ch = ch;
    out.round = round;
  }
  return true;
}

void ScanRunner::dwell_(ScanDwellOut& out) {
  if (!radio_.retune(out.ch)) return;
  sleep_(cfg_.settle_ms);
  // Discard barrier: the counters are delta-on-read, so this zeroes what
  // accumulated on the previous channel before the observe opens.
  (void)radio_.read_energy(false);
  const uint16_t period = nhm_period_4us(cfg_.dwell_ms);
  const bool armed = radio_.arm_nhm_busy(period);
  const ScoutFrames f0 = radio_.frames();
  const int64_t t0 = now_();
  sleep_(cfg_.dwell_ms);
  const NhmBusyRead nb = armed ? radio_.read_nhm_busy() : NhmBusyRead{};
  const std::optional<double> busy =
      (nb.valid && nb.period == period) ? nhm_busy_pct(nb, cfg_.busy_dbm) : std::nullopt;
  const ScoutEnergy e = radio_.read_energy(true);
  const ScoutFrames f1 = radio_.frames();
  const int64_t observe_ms = std::max<int64_t>(now_() - t0, 1);
  out.reading = e.fa_valid;
  out.busy_valid = busy.has_value();
  out.busy_pct = busy.value_or(0.0);
  out.rate_per_s = (static_cast<double>(e.cca_ofdm) + e.fa_ofdm) * 1000.0 / observe_ms;
  out.own = static_cast<uint32_t>(f1.own - f0.own);
  out.foreign = static_cast<uint32_t>(f1.foreign - f0.foreign);
  out.floor_valid = e.floor_valid;
  out.floor_dbm = e.floor_dbm;
}

std::vector<mabur::scan::ChanResult> summarize(const std::vector<ScanDwellOut>& dwells,
                                               mabur::scan::Source source,
                                               const std::map<uint8_t, double>* baseline) {
  using mabur::scan::Source;
  double base_max = 0;
  if (baseline)
    for (const auto& [ch, r] : *baseline) base_max = std::max(base_max, r);
  struct Acc {
    mabur::scan::ChanResult r;
    double sum = 0;
    int n = 0;
  };
  std::vector<Acc> acc;
  for (const auto& d : dwells) {
    auto it = std::find_if(acc.begin(), acc.end(), [&](const Acc& a) { return a.r.ch == d.ch; });
    if (it == acc.end()) {
      acc.push_back({});
      it = acc.end() - 1;
      it->r.ch = d.ch;
      it->r.dfs = mabur::scan::is_dfs(d.ch);
    }
    it->r.frames += d.foreign;
    if (d.floor_valid) it->r.floor_dbm = d.floor_dbm;
    if (!d.reading) continue;
    std::optional<double> v;
    if (source == Source::Nhm) {
      if (d.busy_valid) v = d.busy_pct;
    } else if (source == Source::Estimate && baseline) {
      const auto b = baseline->find(d.ch);
      v = mabur::scan::j1_estimate(d.rate_per_s, b != baseline->end() ? b->second : base_max,
                                   base_max);
    } else {
      v = d.rate_per_s;
    }
    if (!v) continue;
    it->r.busy_worst = it->n ? std::max(it->r.busy_worst, *v) : *v;
    it->sum += *v;
    ++it->n;
    it->r.reading = true;
  }
  std::vector<mabur::scan::ChanResult> out;
  for (auto& a : acc) {
    if (a.n) a.r.busy_mean = a.sum / a.n;
    out.push_back(a.r);
  }
  return out;
}

}  // namespace maburgs
