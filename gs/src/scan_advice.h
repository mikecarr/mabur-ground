#pragma once
// Pre-flight channel scan advice: from per-channel busy readings, the 40 MHz
// pair verdicts and a recommended channel set at 20 or 40 MHz. Pure,
// header-only; shared by the web GS scan and any GS that runs one.
//
// "busy" is the share of airtime above the GS's busy threshold, as the
// scout measures it; a channel is clean below blocked_pct. A pair is only as
// good as its worse half, because a 40 MHz link occupies both.
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "mabur/ht40.h"

namespace mabur::scan {

// Where the busy numbers came from: the card's NHM histogram, an estimate
// from FA/CCA counter rates against a saved quiet baseline, or counter rates
// alone (good for ranking only, so no clean/busy verdicts).
enum class Source { Nhm, Estimate, RankOnly };

struct ChanResult {
  uint8_t ch = 0;
  bool reading = false;   // false: no valid dwell (retune failed, read failed)
  double busy_worst = 0;  // worst round, %
  double busy_mean = 0;   // mean over rounds, %
  uint32_t frames = 0;    // frames heard over all rounds
  bool dfs = false;
  std::optional<int8_t> floor_dbm;
};

enum class PairVerdict { Ok40, Only20, Busy, RankOnly };

struct PairResult {
  uint8_t lo = 0, hi = 0;
  PairVerdict v = PairVerdict::RankOnly;
  uint8_t clean_ch = 0;  // Only20: the clean half; 0 otherwise
  bool spur = false;
  double worst = 0;      // the worse half's busy_worst
};

struct Advice {
  uint8_t width = 20;
  std::vector<uint8_t> set;
  bool flagged = false;  // fewer usable channels than the target, or not all clean
  std::string reason;
};

struct Opts {
  double blocked_pct = 30;
  bool include_dfs = true;
  size_t target = 4;
  Source source = Source::Nhm;
};

// A 20 MHz 5 GHz channel the link can be configured on (UNII-1/2/2e/3:
// 36-64, 100-144, 149-165, every fourth). The scan also covers 169-177, but
// never proposes them.
constexpr bool link_grid_5g(uint8_t ch) {
  if (ch >= 36 && ch <= 64) return (ch - 36) % 4 == 0;
  if (ch >= 100 && ch <= 144) return (ch - 100) % 4 == 0;
  if (ch >= 149 && ch <= 165) return (ch - 149) % 4 == 0;
  return false;
}

// UNII-2A and UNII-2C: transmitters there must detect radar.
constexpr bool is_dfs(uint8_t ch) { return ch >= 52 && ch <= 144; }

// Pairs whose 40 MHz centre is a spur centre on the drone's 8822E: retunes
// there take 65-91 ms (docs/bw40.md "Channels").
constexpr bool is_spur_pair(uint8_t lo) {
  return lo == 52 || lo == 100 || lo == 116 || lo == 149 || lo == 157;
}

// Busy % estimate from an FA+CCA counter rate on a card without NHM: the
// rate above this channel's quiet baseline, scaled so k_sat times the
// busiest baseline channel's rate reads as fully busy.
inline double j1_estimate(double rate, double base_ch, double base_max, double k_sat = 20) {
  const double den = std::max(k_sat * base_max, 1e-9);
  return std::clamp(100.0 * (rate - base_ch) / den, 0.0, 100.0);
}

namespace detail {
inline const ChanResult* find(const std::vector<ChanResult>& r, uint8_t ch) {
  for (const auto& c : r) if (c.ch == ch) return &c;
  return nullptr;
}
inline bool usable(const ChanResult& c, const Opts& o) {
  return c.reading && link_grid_5g(c.ch) && (o.include_dfs || !is_dfs(c.ch));
}
inline auto chan_key(const ChanResult& c) {
  return std::make_tuple(c.busy_worst, c.busy_mean, c.frames);
}
}  // namespace detail

// One entry per standard pair with at least one half scanned, in band order.
// A pair with a half missing or unread can't be judged: RankOnly.
inline std::vector<PairResult> pairs(const std::vector<ChanResult>& r, const Opts& o) {
  std::vector<PairResult> out;
  for (uint8_t lo = 36; lo <= 157; lo = static_cast<uint8_t>(lo + 1)) {
    if (!link_grid_5g(lo) || mabur::ht40_offset(lo) != 1) continue;
    const uint8_t hi = mabur::ht40_pair_other(lo);
    const ChanResult* a = detail::find(r, lo);
    const ChanResult* b = detail::find(r, hi);
    if (!a && !b) continue;
    PairResult p;
    p.lo = lo;
    p.hi = hi;
    p.spur = is_spur_pair(lo);
    if (!a || !b || !a->reading || !b->reading) {
      out.push_back(p);
      continue;
    }
    p.worst = std::max(a->busy_worst, b->busy_worst);
    if (o.source != Source::RankOnly) {
      const bool ca = a->busy_worst < o.blocked_pct, cb = b->busy_worst < o.blocked_pct;
      p.v = ca && cb ? PairVerdict::Ok40 : ca || cb ? PairVerdict::Only20 : PairVerdict::Busy;
      if (p.v == PairVerdict::Only20) p.clean_ch = ca ? lo : hi;
    }
    out.push_back(p);
  }
  return out;
}

// 40 MHz when at least `target` usable pairs are clean (named by their upper
// channel, so every member shares one ht40_offset); otherwise the `target`
// least-busy usable 20 MHz channels.
inline Advice recommend(const std::vector<ChanResult>& r, const Opts& o) {
  Advice a;
  struct Cand {
    const PairResult* p;
    double mean;
    uint32_t frames;
  };
  const auto ps = pairs(r, o);
  std::vector<Cand> ok;
  for (const auto& p : ps) {
    if (p.v != PairVerdict::Ok40) continue;
    const ChanResult* lo = detail::find(r, p.lo);
    const ChanResult* hi = detail::find(r, p.hi);
    if (!detail::usable(*lo, o) || !detail::usable(*hi, o)) continue;
    ok.push_back({&p, lo->busy_mean + hi->busy_mean, lo->frames + hi->frames});
  }
  if (ok.size() >= o.target) {
    // Spur pairs lose only ties on the worst reading.
    std::stable_sort(ok.begin(), ok.end(), [](const Cand& x, const Cand& y) {
      return std::make_tuple(x.p->worst, x.p->spur, x.mean, x.frames) <
             std::make_tuple(y.p->worst, y.p->spur, y.mean, y.frames);
    });
    a.width = 40;
    for (size_t i = 0; i < o.target; ++i) a.set.push_back(ok[i].p->hi);
    a.reason = std::to_string(ok.size()) + " clean pairs → 40 MHz has room";
    return a;
  }
  std::vector<const ChanResult*> ch;
  for (const auto& c : r) if (detail::usable(c, o)) ch.push_back(&c);
  std::stable_sort(ch.begin(), ch.end(), [](const ChanResult* x, const ChanResult* y) {
    return detail::chan_key(*x) < detail::chan_key(*y);
  });
  const size_t n = std::min(o.target, ch.size());
  for (size_t i = 0; i < n; ++i) a.set.push_back(ch[i]->ch);
  if (ch.size() < o.target) {
    a.flagged = true;
    a.reason = "only " + std::to_string(ch.size()) + " usable channels";
    return a;
  }
  if (o.source == Source::RankOnly) {
    a.reason = "ranked by activity only (no busy % on this card)";
    return a;
  }
  for (size_t i = 0; i < n; ++i)
    if (ch[i]->busy_worst >= o.blocked_pct) a.flagged = true;
  a.reason = "only " + std::to_string(ok.size()) + " clean pairs → 20 MHz";
  return a;
}

}  // namespace mabur::scan
