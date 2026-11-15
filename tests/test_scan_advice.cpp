#include <algorithm>
#include <vector>
#include "mtest.h"
#include "mabur/channel_set.h"
#include "scan_advice.h"
using namespace mabur::scan;

namespace {
ChanResult ch(uint8_t c, double worst, double mean = -1, uint32_t frames = 0) {
  ChanResult r;
  r.ch = c;
  r.reading = true;
  r.busy_worst = worst;
  r.busy_mean = mean < 0 ? worst : mean;
  r.frames = frames;
  r.dfs = is_dfs(c);
  return r;
}
Opts nhm() {
  Opts o;
  o.source = Source::Nhm;
  return o;
}
const PairResult* find_pair(const std::vector<PairResult>& v, uint8_t lo) {
  for (const auto& p : v) if (p.lo == lo) return &p;
  return nullptr;
}
bool contains(const std::vector<uint8_t>& s, uint8_t c) {
  return std::find(s.begin(), s.end(), c) != s.end();
}
}  // namespace

TEST(pair_both_clean_is_ok40) {
  const auto ps = pairs({ch(100, 1), ch(104, 2)}, nhm());
  const auto* p = find_pair(ps, 100);
  CHECK(p && p->hi == 104 && p->v == PairVerdict::Ok40);
  CHECK(p && p->spur);
}

TEST(pair_one_half_busy_is_only20) {
  const auto ps = pairs({ch(157, 35), ch(161, 15)}, nhm());
  const auto* p = find_pair(ps, 157);
  CHECK(p && p->v == PairVerdict::Only20 && p->clean_ch == 161);
}

TEST(pair_both_busy) {
  const auto ps = pairs({ch(36, 64), ch(40, 52)}, nhm());
  const auto* p = find_pair(ps, 36);
  CHECK(p && p->v == PairVerdict::Busy && p->clean_ch == 0);
}

TEST(four_clean_pairs_recommends_40) {
  const std::vector<ChanResult> r = {ch(60, 3), ch(64, 3), ch(108, 1), ch(112, 1),
                                     ch(124, 2), ch(128, 2), ch(132, 2.5), ch(136, 2.5),
                                     ch(140, 4), ch(144, 4)};
  const Advice a = recommend(r, nhm());
  CHECK(a.width == 40);
  CHECK((a.set == std::vector<uint8_t>{112, 128, 136, 64}));
  CHECK(!mabur::channel_set_issue(a.set, 40, "x").has_value());
  CHECK(!a.flagged);
}

TEST(spur_ranked_below_equal_clean) {
  const std::vector<ChanResult> r = {ch(100, 1), ch(104, 1), ch(124, 1), ch(128, 1)};
  Opts o = nhm();
  o.target = 2;
  const Advice a = recommend(r, o);
  CHECK(a.width == 40);
  CHECK((a.set == std::vector<uint8_t>{128, 104}));
}

TEST(two_clean_pairs_recommends_20) {
  // 100+104 and 108+112 clean; 36 and 149 clean on their own (their pair halves busy).
  const std::vector<ChanResult> r = {ch(100, 1), ch(104, 2), ch(108, 3), ch(112, 4),
                                     ch(36, 5), ch(40, 60), ch(149, 6), ch(153, 70)};
  const Advice a = recommend(r, nhm());
  CHECK(a.width == 20);
  CHECK((a.set == std::vector<uint8_t>{100, 104, 108, 112}));
  CHECK(a.reason.find("2 clean pairs") != std::string::npos);
  CHECK(!a.flagged);
}

TEST(fewer_than_four_clean_flags_and_fills) {
  const Advice a = recommend({ch(36, 70), ch(40, 50), ch(44, 90)}, nhm());
  CHECK(a.width == 20);
  CHECK((a.set == std::vector<uint8_t>{40, 36, 44}));
  CHECK(a.flagged);
}

TEST(no_reading_channel_never_recommended) {
  ChanResult dead = ch(104, 0);
  dead.reading = false;
  const std::vector<ChanResult> r = {ch(100, 1), dead, ch(36, 2), ch(44, 3), ch(149, 4)};
  const Advice a = recommend(r, nhm());
  CHECK(!contains(a.set, 104));
  const auto* p = find_pair(pairs(r, nhm()), 100);
  CHECK(p && p->v == PairVerdict::RankOnly);
}

TEST(exclude_dfs_drops_dfs_from_set) {
  const std::vector<ChanResult> r = {ch(100, 1), ch(104, 1), ch(108, 1), ch(112, 1),
                                     ch(36, 5), ch(40, 6), ch(149, 7), ch(161, 8)};
  Opts o = nhm();
  o.include_dfs = false;
  const Advice a = recommend(r, o);
  for (uint8_t c : a.set) CHECK(!is_dfs(c));
  CHECK(a.set.size() == 4);
}

TEST(rank_only_has_no_ok_labels) {
  const std::vector<ChanResult> r = {ch(100, 1), ch(104, 1), ch(108, 2), ch(112, 2),
                                     ch(36, 3), ch(40, 4)};
  Opts o;
  o.source = Source::RankOnly;
  for (const auto& p : pairs(r, o)) CHECK(p.v == PairVerdict::RankOnly);
  const Advice a = recommend(r, o);
  CHECK(a.set.size() == 4);
  CHECK(a.width == 20);
}

TEST(recommend_stays_on_the_link_grid) {
  // 169/173/177 are scanned but are not channels the link can sit on.
  const Advice a = recommend({ch(169, 0), ch(173, 0), ch(36, 5), ch(44, 6), ch(149, 7), ch(161, 8)},
                             nhm());
  CHECK(!contains(a.set, 169) && !contains(a.set, 173));
}

TEST(dfs_and_spur_tables) {
  CHECK(is_dfs(52) && is_dfs(144) && !is_dfs(48) && !is_dfs(149));
  CHECK(is_spur_pair(52) && is_spur_pair(100) && is_spur_pair(116) && is_spur_pair(149) &&
        is_spur_pair(157));
  CHECK(!is_spur_pair(124) && !is_spur_pair(36));
}

TEST(j1_estimate_clamps) {
  CHECK(j1_estimate(5, 5, 5) == 0);
  CHECK(j1_estimate(5 + 20 * 5, 5, 5) == 100);
  CHECK(j1_estimate(1e9, 5, 5) == 100);
  CHECK(j1_estimate(0, 5, 5) == 0);
  CHECK(j1_estimate(5 + 10 * 5, 5, 5) == 50);
}

MTEST_MAIN
