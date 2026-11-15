#pragma once
// Pre-flight channel scan: measure-only dwells over a channel plan on one
// card, through the same ScoutRadio seam and observe steps as ChannelScout
// (retune, settle, discard read, NHM arm, observe, NHM + energy read), so a
// scan's busy % means what the auto pick's does. It never transmits: no
// search, no DISC, no beacon window.
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "scan_advice.h"
#include "scout_radio.h"

namespace maburgs {

struct ScanRunCfg {
  std::vector<uint8_t> plan;  // channels in dwell order (scan_plan)
  int dwell_ms = 250;
  int settle_ms = 30;
  int rounds = 3;
  int busy_dbm = -83;
};

struct ScanDwellOut {
  uint8_t ch = 0;
  int round = 0;
  bool reading = false;     // the retune and the energy read both worked
  bool busy_valid = false;  // an NHM busy % for this dwell
  double busy_pct = 0;
  double rate_per_s = 0;    // (cca_ofdm + fa_ofdm) per second of observe
  uint32_t own = 0, foreign = 0;
  bool floor_valid = false;
  int8_t floor_dbm = 0;
};

// Every 20 MHz 5 GHz channel (36-64, 100-144, 149-177, step 4) whose centre
// is inside the card's tune range.
std::vector<uint8_t> scan_plan(const CardCaps& caps);

// Whether the card can run the timed NHM busy window the dwells read busy %
// from. Not the same as CardCaps::nhm_ok: a Jaguar1 card reads an NHM
// histogram but has no busy window, so its busy % would never arrive.
bool probe_nhm_busy(ScoutRadio& radio);

// NHM busy % when the card has the busy window; else the counter estimate if
// a quiet baseline exists; else ranking by counter rate only.
mabur::scan::Source scan_source(bool nhm_busy_ok, bool have_baseline);

class ScanRunner {
 public:
  ScanRunner(ScoutRadio& radio, ScanRunCfg cfg, std::function<void(int)> sleep_ms,
             std::function<int64_t()> now_ms);
  // One dwell; false once all rounds are done, stop() was called, or the
  // card is gone (lost()). A retune on an unplugged card can still report
  // success, so liveness is asked before every dwell.
  bool step(ScanDwellOut& out);
  void stop() { stop_.store(true, std::memory_order_release); }
  void set_alive(std::function<bool()> alive) { alive_ = std::move(alive); }
  bool lost() const { return lost_; }
  int round() const { return round_; }

 private:
  void dwell_(ScanDwellOut& out);   // the radio steps of one dwell; may throw
  ScoutRadio& radio_;
  ScanRunCfg cfg_;
  std::function<void(int)> sleep_;
  std::function<int64_t()> now_;
  std::function<bool()> alive_;
  std::atomic<bool> stop_{false};
  bool lost_ = false;
  int round_ = 0;
  size_t idx_ = 0;
};

// Per-channel results in plan order. baseline: quiet counter rate per
// channel, used only for Source::Estimate. RankOnly puts the mean counter
// rate in the busy fields (comparable between channels, not a %).
std::vector<mabur::scan::ChanResult> summarize(const std::vector<ScanDwellOut>& dwells,
                                               mabur::scan::Source source,
                                               const std::map<uint8_t, double>* baseline);

}  // namespace maburgs
