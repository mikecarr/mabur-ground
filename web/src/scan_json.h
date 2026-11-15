#pragma once
// The scan's stdout lines for the page: `SCAN <kind> <json>`, one object per
// line (keys sorted). The page's lib/scan.js parses exactly these.
//   SCAN caps   {blocked_pct, chip, gen, plan, rounds, source}
//   SCAN dwell  {busy|null, ch, floor|null, foreign, own, rate, reading, round}
//   SCAN result {advice, advice_no_dfs, chans, pairs, rates}
//   SCAN done / SCAN error <text>
#include <map>
#include <string>
#include <vector>

#include "json.hpp"
#include "scan_advice.h"
#include "scan_runner.h"
#include "scout_radio.h"

namespace webgs {

inline const char* scan_source_name(mabur::scan::Source s) {
  switch (s) {
    case mabur::scan::Source::Nhm: return "nhm";
    case mabur::scan::Source::Estimate: return "estimate";
    case mabur::scan::Source::RankOnly: return "rank";
  }
  return "rank";
}

inline const char* pair_verdict_name(mabur::scan::PairVerdict v) {
  switch (v) {
    case mabur::scan::PairVerdict::Ok40: return "ok40";
    case mabur::scan::PairVerdict::Only20: return "only20";
    case mabur::scan::PairVerdict::Busy: return "busy";
    case mabur::scan::PairVerdict::RankOnly: return "rank";
  }
  return "rank";
}

inline std::string scan_caps_line(const maburgs::CardCaps& c, mabur::scan::Source src,
                                  const std::vector<uint8_t>& plan, int rounds, double blocked_pct) {
  nlohmann::json j;
  j["chip"] = c.chip;
  j["gen"] = c.gen;
  j["source"] = scan_source_name(src);
  j["plan"] = plan;
  j["rounds"] = rounds;
  j["blocked_pct"] = blocked_pct;
  return "SCAN caps " + j.dump();
}

inline std::string scan_dwell_line(const maburgs::ScanDwellOut& d) {
  nlohmann::json j;
  j["ch"] = d.ch;
  j["round"] = d.round;
  j["reading"] = d.reading;
  j["busy"] = d.busy_valid ? nlohmann::json(d.busy_pct) : nlohmann::json(nullptr);
  j["rate"] = d.rate_per_s;
  j["own"] = d.own;
  j["foreign"] = d.foreign;
  j["floor"] = d.floor_valid ? nlohmann::json(static_cast<int>(d.floor_dbm)) : nlohmann::json(nullptr);
  return "SCAN dwell " + j.dump();
}

inline nlohmann::json advice_json(const mabur::scan::Advice& a) {
  return {{"width", a.width}, {"set", a.set}, {"flagged", a.flagged}, {"reason", a.reason}};
}

// opts.include_dfs is ignored: the line carries both recommendations so the
// page's Include DFS switch needs no rescan.
inline std::string scan_result_line(const std::vector<mabur::scan::ChanResult>& r,
                                    mabur::scan::Opts opts, const std::map<uint8_t, double>& rates) {
  nlohmann::json chans = nlohmann::json::array();
  for (const auto& c : r)
    chans.push_back({{"ch", c.ch},
                     {"reading", c.reading},
                     {"worst", c.busy_worst},
                     {"mean", c.busy_mean},
                     {"frames", c.frames},
                     {"dfs", c.dfs},
                     {"floor", c.floor_dbm ? nlohmann::json(static_cast<int>(*c.floor_dbm))
                                           : nlohmann::json(nullptr)}});
  opts.include_dfs = true;
  nlohmann::json pairs = nlohmann::json::array();
  for (const auto& p : mabur::scan::pairs(r, opts))
    pairs.push_back({{"lo", p.lo},
                     {"hi", p.hi},
                     {"verdict", pair_verdict_name(p.v)},
                     {"clean", p.clean_ch},
                     {"spur", p.spur},
                     {"worst", p.worst}});
  nlohmann::json rj = nlohmann::json::object();
  for (const auto& [ch, v] : rates) rj[std::to_string(ch)] = v;
  nlohmann::json j;
  j["chans"] = chans;
  j["pairs"] = pairs;
  j["advice"] = advice_json(mabur::scan::recommend(r, opts));
  opts.include_dfs = false;
  j["advice_no_dfs"] = advice_json(mabur::scan::recommend(r, opts));
  j["rates"] = rj;
  return "SCAN result " + j.dump();
}

}  // namespace webgs
