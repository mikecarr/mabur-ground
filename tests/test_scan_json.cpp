#include <string>
#include <vector>
#include "mtest.h"
#include "scan_json.h"
using namespace webgs;
using mabur::scan::Source;

TEST(caps_line) {
  maburgs::CardCaps c;
  c.chip = "RTL8812AU";
  c.gen = "jaguar1";
  const std::string s = scan_caps_line(c, Source::RankOnly, {36, 40}, 3, 30);
  CHECK(s == R"(SCAN caps {"blocked_pct":30.0,"chip":"RTL8812AU","gen":"jaguar1","plan":[36,40],"rounds":3,"source":"rank"})");
}

TEST(dwell_line_with_and_without_readings) {
  maburgs::ScanDwellOut d;
  d.ch = 36;
  d.round = 1;
  d.reading = true;
  d.busy_valid = true;
  d.busy_pct = 12.5;
  d.rate_per_s = 400;
  d.foreign = 41;
  d.floor_valid = true;
  d.floor_dbm = -95;
  CHECK(scan_dwell_line(d) ==
        R"(SCAN dwell {"busy":12.5,"ch":36,"floor":-95,"foreign":41,"own":0,"rate":400.0,"reading":true,"round":1})");
  maburgs::ScanDwellOut n;
  n.ch = 144;
  CHECK(scan_dwell_line(n) ==
        R"(SCAN dwell {"busy":null,"ch":144,"floor":null,"foreign":0,"own":0,"rate":0.0,"reading":false,"round":0})");
}

TEST(result_line_shape) {
  mabur::scan::ChanResult a;
  a.ch = 100;
  a.reading = true;
  a.busy_worst = 1;
  a.busy_mean = 0.5;
  a.dfs = true;
  mabur::scan::ChanResult b = a;
  b.ch = 104;
  mabur::scan::Opts o;
  o.target = 1;
  const std::vector<mabur::scan::ChanResult> r = {a, b};
  const std::string s = scan_result_line(r, o, {{100, 7.0}});
  CHECK(s.rfind("SCAN result {", 0) == 0);
  const auto j = nlohmann::json::parse(s.substr(12));
  CHECK(j["chans"].size() == 2);
  CHECK(j["chans"][0]["ch"] == 100 && j["chans"][0]["dfs"] == true);
  CHECK(j["chans"][0]["floor"].is_null());
  CHECK(j["pairs"][0]["lo"] == 100 && j["pairs"][0]["verdict"] == "ok40" && j["pairs"][0]["spur"] == true);
  CHECK(j["advice"]["width"] == 40 && j["advice"]["set"][0] == 104);
  CHECK(j["advice_no_dfs"]["set"].empty() && j["advice_no_dfs"]["flagged"] == true);
  CHECK(j["rates"]["100"] == 7.0);
}

MTEST_MAIN
