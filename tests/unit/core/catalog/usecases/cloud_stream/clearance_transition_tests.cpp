#include "tests/unit/core/catalog/usecases/cloud_stream/launch_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace catalog_test {
namespace {

class CloudStreamClearanceTransitions : public ::testing::Test,
                                        protected CloudStreamCases {
 protected:
  using Scenario =
      std::function<libchiaking::catalog::LaunchVerdict(CloudStreamLaunch&)>;

  static auto trace(CloudStreamLaunch& launch,
                    const libchiaking::catalog::LaunchVerdict& verdict)
      -> std::string {
    auto text =
        verdict.is_allowed()
            ? std::string{"cleared"}
            : std::format("refused {}", static_cast<int>(kind_of(verdict)));
    text += " |";
    for (const auto check : launch.checks)
      text += std::format(" {}", static_cast<int>(check));
    text += std::format(" | rec{} res{} lib{} gk{} all{}",
                        launch.rig.http.count(ClearanceRig::ps5_record),
                        launch.rig.http.count(ClearanceRig::resolver),
                        launch.rig.http.count(ClearanceRig::owned_lookup),
                        launch.gk(), launch.rig.http.count(""));
    return text;
  }

  static auto scenarios()
      -> std::vector<std::pair<std::string_view, Scenario>> {
    return {
        {"owned",
         [](CloudStreamLaunch& launch) {
           return launch.clear(owning_rivet(), rivet);
         }},
        {"handed_streaming",
         [](CloudStreamLaunch& launch) {
           return launch.clear(owning_rivet(), rivet, streaming_allowed());
         }},
        {"unlisted",
         [](CloudStreamLaunch& launch) {
           return launch.clear(owning_nothing(), bus);
         }},
        {"unclaimed",
         [](CloudStreamLaunch& launch) {
           return launch.clear(owning_nothing(), show);
         }},
        {"unknown_owned",
         [](CloudStreamLaunch& launch) {
           launch.rig.ps5_record_answers(200, show_record());
           return launch.clear(owning_unknown(), show);
         }},
        {"unknown_not_found",
         [](CloudStreamLaunch& launch) {
           launch.rig.ps5_record_answers(404, "{}");
           return launch.clear(owning_unknown(), show);
         }},
        {"streaming_denied",
         [](CloudStreamLaunch& launch) {
           launch.rig.cloud_streaming_denied("TIER_20");
           return launch.clear(owning_rivet(), rivet);
         }},
        {"streaming_unreadable",
         [](CloudStreamLaunch& launch) {
           launch.rig.cloud_streaming_unreadable(500);
           return launch.clear(owning_rivet(), rivet);
         }},
        {"entitlement_missing",
         [](CloudStreamLaunch& launch) {
           launch.rig.ps5_record_answers(404, "{}");
           return launch.clear(owning_rivet(), rivet);
         }},
        {"entitlement_unreadable",
         [](CloudStreamLaunch& launch) {
           launch.rig.ps5_record_answers(500, "");
           return launch.clear(owning_rivet(), rivet);
         }},
        {"entitlement_inactive",
         [](CloudStreamLaunch& launch) {
           launch.rig.ps5_record_answers(200, trial_reading("false", "true"));
           return launch.clear(owning_rivet(), rivet);
         }},
        {"resolver_unreadable",
         [](CloudStreamLaunch& launch) {
           launch.rig.resolver_answers(500, "");
           return launch.clear(owning_rivet(), rivet);
         }},
        {"excluded",
         [](CloudStreamLaunch& launch) {
           launch.rig.excluding(R"("PPSA07028_00")");
           return launch.clear(owning_rivet(), rivet);
         }},
        {"preordered",
         [](CloudStreamLaunch& launch) {
           launch.rig.ps5_record_answers(
               200, preordered("2031-01-01T00:00:00.000Z"));
           return launch.clear(owning_rivet(), rivet);
         }},
        {"trial",
         [](CloudStreamLaunch& launch) {
           launch.rig.ps5_record_answers(200, trial_reading("true", "true"));
           return launch.clear(holding_a_trial(), rivet);
         }},
        {"trial_taken_down",
         [](CloudStreamLaunch& launch) {
           launch.rig.ps5_record_answers(200, trial_reading("false", "false"));
           const auto verdict = launch.clear(holding_a_trial(), rivet);
           const auto again = launch.clear(holding_a_trial(), rivet);
           return again.is_allowed() ? again : verdict;
         }},
        {"ended_trial",
         [](CloudStreamLaunch& launch) {
           return launch.clear(holding_an_ended_trial(), rivet);
         }},
        {"first_of_bundle",
         [](CloudStreamLaunch& launch) {
           return launch.clear(owning_the_first_game(), first);
         }},
    };
  }

  static auto pins()
      -> std::vector<std::pair<std::string_view, std::string_view>> {
    return {
        {"owned", "cleared | 2 4 5 6 7 | rec1 res1 lib0 gk0 all4"},
        {"handed_streaming", "cleared | 4 5 6 7 | rec1 res1 lib0 gk0 all3"},
        {"unlisted", "refused 0 | | rec0 res0 lib0 gk0 all0"},
        {"unclaimed", "refused 3 | 2 | rec0 res0 lib0 gk0 all1"},
        {"unknown_owned", "cleared | 2 3 4 5 6 7 | rec2 res1 lib0 gk0 all5"},
        {"unknown_not_found", "refused 3 | 2 3 | rec1 res0 lib0 gk0 all4"},
        {"streaming_denied", "refused 4 | 2 | rec0 res0 lib0 gk0 all2"},
        {"streaming_unreadable", "refused 10 | 2 | rec0 res0 lib0 gk0 all1"},
        {"entitlement_missing", "refused 11 | 2 4 | rec1 res0 lib0 gk0 all2"},
        {"entitlement_unreadable",
         "refused 10 | 2 4 | rec1 res0 lib0 gk0 all2"},
        {"entitlement_inactive", "refused 14 | 2 4 | rec1 res0 lib0 gk0 all2"},
        {"resolver_unreadable", "refused 10 | 2 4 5 | rec1 res1 lib0 gk0 all3"},
        {"excluded", "refused 6 | 2 4 5 6 | rec1 res1 lib0 gk0 all4"},
        {"preordered", "refused 9 | 2 4 5 6 7 | rec1 res1 lib0 gk0 all4"},
        {"trial", "cleared | 2 4 5 6 7 | rec1 res1 lib0 gk0 all4"},
        {"trial_taken_down", "refused 12 | | rec1 res0 lib0 gk0 all2"},
        {"ended_trial", "cleared | 2 4 5 6 7 | rec1 res1 lib0 gk0 all4"},
        {"first_of_bundle", "cleared | 2 4 5 6 7 | rec1 res1 lib0 gk0 all4"},
    };
  }
};

TEST_F(CloudStreamClearanceTransitions, EveryScenarioEndsWhereTheTableSays) {
  const auto table = pins();
  std::size_t matched = 0;
  for (const auto& [name, scenario] : scenarios()) {
    CloudStreamLaunch launch;
    const auto verdict = scenario(launch);
    const auto actual = trace(launch, verdict);
    const auto pin = std::ranges::find_if(
        table, [&](const auto& item) { return item.first == name; });
    if (pin == table.end()) {
      ADD_FAILURE() << "{\"" << name << "\", \"" << actual << "\"},";
      continue;
    }
    ++matched;
    EXPECT_EQ(actual, pin->second) << name;
  }
  EXPECT_EQ(matched, table.size());
}

}  // namespace
}  // namespace catalog_test
