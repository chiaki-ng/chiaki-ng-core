#include "tests/support/process_threading.hpp"
#include "tests/unit/core/consoles/adapters/titles/title_start_rig.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>

#include <gtest/gtest.h>

#include <memory>

namespace consoles_test {
namespace {

using libchiaking::consoles::StartFailure;
using libchiaking::consoles::StartState;
using libchiaking::consoles::TitleStart;
using libchiaking::consoles::TitlesUnavailable;
using libchiaking::users::Unanswered;
using Reply = StartHarness::Reply;
using State = StartState;
using Why = StartFailure::Reason;

TEST(TitleStartCancel, SendsNothingWhenAlreadyStopped) {
  StartHarness harness;
  harness.stop->stopped = true;
  const auto result = harness.run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Why::cancelled);
  EXPECT_TRUE(harness.http.seen.empty());
}

TEST(TitleStartCancel, NeverPollsWhenStoppedDuringThePost) {
  StartHarness harness;
  Reply reply = StartHarness::accepted();
  reply.during = [stop = harness.stop] { stop->stopped = true; };
  harness.answers_post(reply);
  const auto result = harness.run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Why::cancelled);
  EXPECT_EQ(harness.polls(), 0);
}

TEST(TitleStartCancel, KeepsTheStateReachedWhenStoppedDuringAWait) {
  StartHarness harness;
  harness.waiter.stop_on_wait = 2;
  harness.answers_post(StartHarness::accepted());
  harness.answers_polls({StartHarness::status("executing", "command")});
  const auto result = harness.run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Why::cancelled);
  EXPECT_EQ(result.error().reached, State::starting);
  EXPECT_EQ(harness.polls(), 2);
}

TEST(TitleStartCancel, KeepsTheStateReachedWhenStoppedDuringAPoll) {
  StartHarness harness;
  Reply poll = StartHarness::status("executing");
  poll.during = [stop = harness.stop] { stop->stopped = true; };
  harness.answers_post(StartHarness::accepted());
  harness.answers_polls({poll});
  const auto result = harness.run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Why::cancelled);
  EXPECT_EQ(result.error().reached, State::pending);
}

TEST(TitleStartPause, EndsEarlyWhenStopped) {
  libchiaking::platform::SteadyClock steady;
  TitleStart::Pause pause{test_support::process_threading(), steady};
  const auto stop = std::make_shared<test_support::StopState>();
  EXPECT_TRUE(
      pause.wait_milliseconds(1, libchiaking::platform::StopToken{stop}));
  EXPECT_FALSE(
      pause.wait_milliseconds(-1, libchiaking::platform::StopToken{stop}));
  stop->stopped = true;
  EXPECT_FALSE(
      pause.wait_milliseconds(60000, libchiaking::platform::StopToken{stop}));
}

TEST(TitleStartNotStarted, KeepsTheReasonTheTitlesWereUnavailable) {
  using Titles = TitlesUnavailable::Reason;
  EXPECT_EQ(TitleStart::not_started({Titles::ps5_only}).reason, Why::ps5_only);
  EXPECT_EQ(TitleStart::not_started({Titles::not_paired}).reason,
            Why::not_paired);
  EXPECT_EQ(TitleStart::not_started({Titles::not_on_account}).reason,
            Why::not_on_account);
  EXPECT_EQ(TitleStart::not_started({Titles::forgotten}).reason,
            Why::forgotten);
  EXPECT_EQ(TitleStart::not_started({Titles::not_available}).reason,
            Why::not_available);
}

TEST(TitleStartNotStarted, IsCancelledWhenTheAccountWasCancelled) {
  const auto cancelled =
      TitleStart::not_started({TitlesUnavailable::Reason::unanswered,
                               Unanswered{Unanswered::Reason::cancelled}});
  EXPECT_EQ(cancelled.reason, Why::cancelled);
  EXPECT_EQ(cancelled.reached, State::pending);
}

TEST(TitleStartNotStarted, KeepsTheUnansweredStatus) {
  const auto lapsed = TitleStart::not_started(
      {TitlesUnavailable::Reason::unanswered,
       Unanswered{Unanswered::Reason::store_access_lapsed, 401}});
  EXPECT_EQ(lapsed.reason, Why::unanswered);
  ASSERT_TRUE(lapsed.unanswered);
  EXPECT_EQ(lapsed.unanswered->http_status, 401);
}

}  // namespace
}  // namespace consoles_test
