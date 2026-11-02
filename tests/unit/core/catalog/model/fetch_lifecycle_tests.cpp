#include "src/core/catalog/model/fetch_lifecycle.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace catalog_test {
namespace {

using libchiaking::catalog::FetchActivity;
using libchiaking::catalog::FetchStep;
using libchiaking::catalog::PassedStep;
using libchiaking::catalog::StepOutcome;
using libchiaking::catalog::Unanswered;
using libchiaking::catalog::fetch_lifecycle::ActivityChanged;
using libchiaking::catalog::fetch_lifecycle::Cancelled;
using libchiaking::catalog::fetch_lifecycle::Failed;
using libchiaking::catalog::fetch_lifecycle::Finished;
using libchiaking::catalog::fetch_lifecycle::Lifecycle;
using libchiaking::catalog::fetch_lifecycle::LifecycleContext;
using libchiaking::catalog::fetch_lifecycle::PagesRead;
using libchiaking::catalog::fetch_lifecycle::Reset;
using libchiaking::catalog::fetch_lifecycle::Start;
using libchiaking::catalog::fetch_lifecycle::StepBegan;
using libchiaking::catalog::fetch_lifecycle::StepEnded;
using libchiaking::catalog::fetch_lifecycle::TimeServerAsked;

class FetchLifecycle : public ::testing::Test {
 protected:
  static constexpr std::uint64_t attempt{1};
  static constexpr std::array working_steps{
      FetchStep::checking_sign_in,  FetchStep::cloud_list,
      FetchStep::owned_library,     FetchStep::premium_category,
      FetchStep::for_sale_category, FetchStep::streaming_access,
      FetchStep::matching,          FetchStep::trusted_time,
      FetchStep::exclusions,
  };

  static auto reach(Lifecycle& machine, FetchStep target,
                    std::uint64_t id = attempt) -> testing::AssertionResult {
    if (!machine.process(Start{id}))
      return testing::AssertionFailure()
             << "attempt " << id << " did not start";
    for (std::size_t at = 1; at < working_steps.size(); ++at) {
      if (working_steps[at - 1] == target) return testing::AssertionSuccess();
      if (!machine.process(StepEnded{id, StepOutcome::fetched}))
        return testing::AssertionFailure()
               << "step " << at - 1 << " did not end";
      if (!machine.process(StepBegan{id, working_steps[at], std::nullopt}))
        return testing::AssertionFailure() << "step " << at << " did not begin";
    }
    return testing::AssertionSuccess();
  }

  static auto run_through(Lifecycle& machine, std::uint64_t id = attempt)
      -> testing::AssertionResult {
    if (auto reached = reach(machine, FetchStep::exclusions, id); !reached)
      return reached;
    if (!machine.process(StepEnded{id, StepOutcome::skipped}))
      return testing::AssertionFailure() << "the last step did not end";
    if (!machine.process(Finished{id}))
      return testing::AssertionFailure()
             << "attempt " << id << " did not finish";
    return testing::AssertionSuccess();
  }

  LifecycleContext context;
  Lifecycle machine{context};
};

TEST_F(FetchLifecycle, PassesEveryStepInOrder) {
  EXPECT_EQ(machine.step(), FetchStep::idle);
  EXPECT_TRUE(machine.process(Start{attempt}));
  EXPECT_EQ(machine.step(), FetchStep::checking_sign_in);
  EXPECT_TRUE(machine.process(StepEnded{attempt, StepOutcome::held}));
  EXPECT_TRUE(machine.process(StepBegan{attempt, FetchStep::cloud_list,
                                        FetchActivity::walking_public_store}));
  EXPECT_TRUE(machine.process(PagesRead{attempt, 1, std::nullopt}));
  EXPECT_TRUE(machine.process(PagesRead{attempt, 2, 7}));
  EXPECT_EQ(machine.snapshot().status().position,
            (libchiaking::progress::Position{2, 7}));
  EXPECT_TRUE(machine.process(
      ActivityChanged{attempt, FetchActivity::checking_streaming_copies}));
  EXPECT_EQ(context.pages_read, 0U);
  EXPECT_FALSE(context.pages_expected);
  EXPECT_TRUE(machine.process(StepEnded{attempt, StepOutcome::fetched}));
  for (std::size_t at = 2; at < working_steps.size(); ++at) {
    EXPECT_TRUE(machine.process(StepBegan{attempt, working_steps[at],
                                          FetchActivity::reusing_held_answer}));
    EXPECT_EQ(machine.step(), working_steps[at]);
    EXPECT_EQ(machine.snapshot().step(), working_steps[at]);
    EXPECT_TRUE(machine.process(StepEnded{attempt, StepOutcome::held}));
  }
  EXPECT_TRUE(machine.process(Finished{attempt}));
  EXPECT_EQ(machine.step(), FetchStep::ready);
  ASSERT_FALSE(context.effects.empty());
  const auto last = context.effects.back().state;
  EXPECT_EQ(last.step(), FetchStep::ready);
  EXPECT_FALSE(last.is_running());
  EXPECT_EQ(last.attempt(), attempt);
  ASSERT_EQ(last.passed().size(), working_steps.size());
  EXPECT_EQ(last.passed()[0],
            (PassedStep{FetchStep::checking_sign_in, StepOutcome::held}));
  EXPECT_EQ(last.passed()[1],
            (PassedStep{FetchStep::cloud_list, StepOutcome::fetched}));
  for (const auto& published : context.effects)
    EXPECT_EQ(published.state.attempt(), attempt);
}

TEST_F(FetchLifecycle, RefusesToJumpAStepOrBeginOneBeforeTheLastEnds) {
  EXPECT_TRUE(machine.process(Start{attempt}));
  EXPECT_FALSE(
      machine.process(StepBegan{attempt, FetchStep::cloud_list, std::nullopt}));
  EXPECT_TRUE(machine.process(StepEnded{attempt, StepOutcome::held}));
  EXPECT_FALSE(machine.process(
      StepBegan{attempt, FetchStep::owned_library, std::nullopt}));
  EXPECT_FALSE(machine.process(StepEnded{attempt, StepOutcome::held}));
  EXPECT_FALSE(machine.process(Finished{attempt}));
  EXPECT_EQ(machine.step(), FetchStep::checking_sign_in);
  EXPECT_TRUE(
      machine.process(StepBegan{attempt, FetchStep::cloud_list, std::nullopt}));
  EXPECT_TRUE(machine.process(
      ActivityChanged{attempt, FetchActivity::walking_public_store}));
}

TEST_F(FetchLifecycle, TakesNoActivityOrPagesForAnEndedStep) {
  ASSERT_TRUE(reach(machine, FetchStep::owned_library));
  EXPECT_TRUE(machine.process(StepEnded{attempt, StepOutcome::degraded}));
  EXPECT_FALSE(machine.process(
      ActivityChanged{attempt, FetchActivity::scanning_ps5_library}));
  EXPECT_FALSE(machine.process(PagesRead{attempt, 3, std::nullopt}));
  ASSERT_FALSE(context.passed.empty());
  EXPECT_EQ(context.passed.back(),
            (PassedStep{FetchStep::owned_library, StepOutcome::degraded}));
}

TEST_F(FetchLifecycle, NamesTheTimeServerOnlyWhileTheTrustedTimeStepAsks) {
  ASSERT_TRUE(reach(machine, FetchStep::matching));
  EXPECT_FALSE(
      machine.process(TimeServerAsked{attempt, "time.cloudflare.com"}));
  EXPECT_TRUE(machine.process(StepEnded{attempt, StepOutcome::fetched}));
  EXPECT_TRUE(machine.process(StepBegan{attempt, FetchStep::trusted_time,
                                        FetchActivity::syncing_clock}));
  EXPECT_FALSE(machine.snapshot().time_server());
  EXPECT_TRUE(machine.process(TimeServerAsked{attempt, "time.cloudflare.com"}));
  EXPECT_EQ(machine.snapshot().time_server(),
            std::optional<std::string>{"time.cloudflare.com"});
  const auto published = context.effects.size();
  EXPECT_FALSE(
      machine.process(TimeServerAsked{attempt, "time.cloudflare.com"}));
  EXPECT_EQ(context.effects.size(), published);
  EXPECT_FALSE(machine.process(TimeServerAsked{2, "time1.google.com"}));
  EXPECT_TRUE(machine.process(TimeServerAsked{attempt, "time1.google.com"}));
  EXPECT_EQ(context.effects.back().state.time_server(),
            std::optional<std::string>{"time1.google.com"});
  EXPECT_TRUE(machine.process(StepEnded{attempt, StepOutcome::fetched}));
  EXPECT_FALSE(machine.snapshot().time_server());
  EXPECT_FALSE(machine.process(TimeServerAsked{attempt, "time2.google.com"}));
}

TEST_F(FetchLifecycle, CanFailAtEveryWorkingStep) {
  for (const auto step : working_steps) {
    LifecycleContext failing;
    Lifecycle failed{failing};
    ASSERT_TRUE(reach(failed, step));
    EXPECT_TRUE(failed.process(
        Failed{attempt, Unanswered{Unanswered::Reason::unavailable}}));
    EXPECT_EQ(failed.step(), FetchStep::unanswered);
    ASSERT_FALSE(failing.effects.empty());
    const auto last = failing.effects.back().state;
    EXPECT_EQ(last.step(), FetchStep::unanswered);
    ASSERT_TRUE(last.failure());
    EXPECT_EQ(last.failure()->reason(),
              libchiaking::catalog::GamesReason::unavailable);
    EXPECT_FALSE(failing.active_attempt_id);
  }
}

TEST_F(FetchLifecycle, CanBeCancelledAtEveryWorkingStep) {
  for (const auto step : working_steps) {
    LifecycleContext cancelling;
    Lifecycle cancelled{cancelling};
    ASSERT_TRUE(reach(cancelled, step));
    EXPECT_TRUE(cancelled.process(Cancelled{attempt}));
    EXPECT_EQ(cancelled.step(), FetchStep::cancelled);
    ASSERT_FALSE(cancelling.effects.empty());
    EXPECT_EQ(cancelling.effects.back().state.step(), FetchStep::cancelled);
    EXPECT_FALSE(cancelling.effects.back().state.failure());
  }
}

TEST_F(FetchLifecycle, DropsEventsOfAnotherAttempt) {
  ASSERT_TRUE(reach(machine, FetchStep::cloud_list, 4));
  const auto published = context.effects.size();
  EXPECT_FALSE(machine.process(PagesRead{3, 1, std::nullopt}));
  EXPECT_FALSE(machine.process(StepEnded{5, StepOutcome::held}));
  EXPECT_FALSE(machine.process(Cancelled{3}));
  EXPECT_FALSE(machine.process(Finished{3}));
  EXPECT_EQ(context.effects.size(), published);
  EXPECT_EQ(machine.step(), FetchStep::cloud_list);
}

TEST_F(FetchLifecycle, StartsANewAttemptOnlyAfterTheLastEnded) {
  EXPECT_FALSE(machine.process(Start{0}));
  EXPECT_TRUE(machine.process(Start{1}));
  EXPECT_FALSE(machine.process(Start{2}));
  EXPECT_TRUE(machine.process(Cancelled{1}));
  EXPECT_FALSE(machine.process(Start{1}));
  EXPECT_TRUE(machine.process(Start{2}));
  EXPECT_EQ(machine.step(), FetchStep::checking_sign_in);
  ASSERT_FALSE(context.effects.empty());
  EXPECT_TRUE(context.effects.back().state.passed().empty());
  EXPECT_TRUE(
      machine.process(Failed{2, Unanswered{Unanswered::Reason::malformed}}));
  ASSERT_TRUE(run_through(machine, 3));
  EXPECT_EQ(machine.step(), FetchStep::ready);
  EXPECT_EQ(context.effects.back().state.attempt(), 3U);
  EXPECT_FALSE(context.effects.back().state.failure());
}

TEST_F(FetchLifecycle, ResetsOnlyAnEndedFetchToIdle) {
  EXPECT_FALSE(machine.process(Reset{}));
  EXPECT_TRUE(machine.process(Start{1}));
  EXPECT_FALSE(machine.process(Reset{}));
  EXPECT_EQ(machine.step(), FetchStep::checking_sign_in);
  EXPECT_TRUE(machine.process(Cancelled{1}));
  EXPECT_TRUE(machine.process(Reset{}));
  EXPECT_EQ(machine.step(), FetchStep::idle);
  ASSERT_FALSE(context.effects.empty());
  const auto last = context.effects.back().state;
  EXPECT_EQ(last.step(), FetchStep::idle);
  EXPECT_TRUE(last.passed().empty());
  EXPECT_EQ(last.attempt(), 1U);
  ASSERT_TRUE(run_through(machine, 2));
  EXPECT_TRUE(machine.process(Reset{}));
  EXPECT_EQ(machine.step(), FetchStep::idle);
}

}  // namespace
}  // namespace catalog_test
