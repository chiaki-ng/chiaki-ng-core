#include "tests/support/stop_states.hpp"
#include "tests/unit/core/consoles/usecases/home_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <vector>

namespace consoles_test {
namespace {

namespace progress = libchiaking::progress;
using libchiaking::consoles::ConsoleName;
using libchiaking::consoles::Firmware;
using libchiaking::consoles::Generation;
using libchiaking::consoles::PairingFailure;
using libchiaking::consoles::PairingProgress;
using libchiaking::consoles::PairingStep;
using libchiaking::platform::StopToken;
using Reason = PairingFailure::Reason;
using Steps = std::vector<PairingStep>;

class StepsHeard final : public libchiaking::consoles::PairingWatcher {
 public:
  void on_progress(const PairingProgress& progress) override {
    heard.push_back(progress);
  }

  auto steps() const -> Steps {
    Steps steps;
    for (const auto& progress : heard) steps.push_back(progress.step);
    return steps;
  }

  std::vector<PairingProgress> heard;
};

class PairAtHomeProgress : public ::testing::Test {
 protected:
  static auto reported_in_order(const std::vector<PairingProgress>& heard)
      -> testing::AssertionResult {
    std::chrono::microseconds last{};
    for (std::size_t index = 0; index < heard.size(); ++index) {
      const auto& each = heard[index];
      if (each.attempt != 1)
        return testing::AssertionFailure()
               << "report " << index << " is attempt " << each.attempt;
      if (each.route)
        return testing::AssertionFailure()
               << "report " << index << " has a route";
      if (each.elapsed < last)
        return testing::AssertionFailure()
               << "report " << index << " went back in time";
      last = each.elapsed;
      if (!(each.status() ==
            progress::Status{std::nullopt, 1, each.elapsed, each.wait}))
        return testing::AssertionFailure()
               << "report " << index << " has another status";
      if (each.step == PairingStep::waiting_for_answer) {
        if (!(each.wait ==
              progress::Wait{progress::WaitReason::console_answering,
                             std::nullopt}))
          return testing::AssertionFailure()
                 << "report " << index << " waits for something else";
      } else if (each.wait || each.status().is_waiting()) {
        return testing::AssertionFailure()
               << "report " << index << " waits outside the answer";
      }
    }
    return testing::AssertionSuccess();
  }

  auto paired_at_home(StepsHeard& watcher) {
    HomeConsole console{home.network};
    console.serve("RES3", home.welcome());
    const auto user = home.signed_in.as("npsso", 7);
    const auto result = home.known.pair_at_home(
        *user, Home<>::host("127.0.0.1"), Generation::ps5, Home<>::pin(),
        home.never, &watcher);
    console.stop_serving();
    return result;
  }

  Home<> home;
};

TEST_F(PairAtHomeProgress, ReportsEachStepInOrder) {
  StepsHeard watcher;
  ASSERT_TRUE(paired_at_home(watcher));
  EXPECT_EQ(watcher.steps(),
            (Steps{PairingStep::reaching_console, PairingStep::sending_request,
                   PairingStep::waiting_for_answer, PairingStep::saving}));
  EXPECT_TRUE(reported_in_order(watcher.heard));
}

TEST_F(PairAtHomeProgress, ReportsTheSameStepsForAFoundConsole) {
  StepsHeard watcher;
  ASSERT_TRUE(paired_at_home(watcher));
  Home<> den_home;
  HomeConsole found_console{den_home.network};
  found_console.serve("RES3", den_home.welcome());
  const auto found = den_home.facts.sighted(
      {Home<>::den(), Firmware{Firmware::Family::ps5_1},
       ConsoleName::create("PS5-Den", ConsoleName::Source::discovered),
       Home<>::host("127.0.0.1"), false, std::nullopt});
  const auto owner = den_home.signed_in.as("npsso", 7);
  StepsHeard found_watcher;
  const auto again = den_home.known.pair_at_home(
      *owner, *found, Home<>::pin(), den_home.never, &found_watcher);
  found_console.stop_serving();
  ASSERT_TRUE(again);
  EXPECT_EQ(again.value(), found);
  EXPECT_EQ(found_watcher.steps(), watcher.steps());
}

TEST_F(PairAtHomeProgress, AFailedPairingNeverReportsSaving) {
  HomeConsole console{home.network};
  console.serve("RES3", HomeConsole::bytes(HomeConsole::refusal));
  const auto user = home.signed_in.as("npsso", 7);
  StepsHeard watcher;
  const auto result =
      home.known.pair_at_home(*user, Home<>::host("127.0.0.1"), Generation::ps5,
                              Home<>::pin(), home.never, &watcher);
  console.stop_serving();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Reason::refused);
  const auto steps = watcher.steps();
  ASSERT_FALSE(steps.empty());
  EXPECT_EQ(steps.front(), PairingStep::reaching_console);
  EXPECT_TRUE(std::ranges::is_sorted(steps));
  EXPECT_EQ(std::ranges::adjacent_find(steps), steps.end());
  EXPECT_EQ(std::ranges::find(steps, PairingStep::saving), steps.end());
}

TEST_F(PairAtHomeProgress, AStoppedPairingReportsNothing) {
  const auto user = home.signed_in.as("npsso", 7);
  const auto stop = std::make_shared<test_support::AtomicStop>();
  stop->stopped.store(true);
  StepsHeard unheard;
  const auto stopped =
      home.known.pair_at_home(*user, Home<>::host("127.0.0.1"), Generation::ps5,
                              Home<>::pin(), StopToken{stop}, &unheard);
  EXPECT_FALSE(stopped);
  EXPECT_TRUE(unheard.heard.empty());
}

}  // namespace
}  // namespace consoles_test
