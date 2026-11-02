#include "src/core/psn/rest/cloud_assisted_navigation/cloud_assisted_navigation.hpp"
#include "tests/unit/core/consoles/adapters/titles/title_start_rig.hpp"

#include <gtest/gtest.h>

#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::consoles::StartFailure;
using libchiaking::consoles::StartState;
using libchiaking::consoles::TitleStart;
using libchiaking::psn::rest::CommandState;
using libchiaking::users::Unanswered;
using Reply = StartHarness::Reply;
using State = StartState;
using States = StartHarness::States;
using Waits = std::vector<int>;
using Why = StartFailure::Reason;

static_assert(TitleStart::default_deadline == std::chrono::milliseconds{10000});
static_assert(TitleStart::default_poll == std::chrono::milliseconds{1500});
static_assert(TitleStart::shortest_poll == std::chrono::milliseconds{500});
static_assert(TitleStart::longest_poll == std::chrono::milliseconds{10000});

class TitleStartPoll : public ::testing::Test {
 protected:
  void SetUp() override { harness.answers_post(StartHarness::accepted()); }

  StartHarness harness;
};

TEST_F(TitleStartPoll, ProgressesThroughTheStates) {
  harness.answers_polls({StartHarness::status("executing"),
                         StartHarness::status("executing", "command"),
                         StartHarness::status("executing", "COMMAND"),
                         StartHarness::status("completed", "command")});
  const auto result = harness.run();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value(), State::completed);
  EXPECT_EQ(harness.progress,
            (States{State::pending, State::starting, State::completed}));
  EXPECT_EQ(harness.polls(), 4);
  EXPECT_EQ(harness.waiter.waits, (Waits{1500, 1500, 1500}));
}

TEST_F(TitleStartPoll, ReportsAFailedCommand) {
  harness.answers_polls({StartHarness::status("executing", "command"),
                         StartHarness::status("failed")});
  const auto result = harness.run();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value(), State::failed);
  EXPECT_EQ(harness.progress,
            (States{State::pending, State::starting, State::failed}));
}

TEST_F(TitleStartPoll, ReportsTheStateReachedAtTheDeadline) {
  harness.timeout_ms = 5000;
  harness.answers_polls({StartHarness::status("executing", "command")});
  const auto result = harness.run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Why::timed_out);
  EXPECT_EQ(result.error().reached, State::starting);
  EXPECT_EQ(harness.polls(), 4);
  EXPECT_EQ(harness.waiter.waits, (Waits{1500, 1500, 1500, 500}));
  EXPECT_EQ(harness.progress, (States{State::pending, State::starting}));
}

TEST_F(TitleStartPoll, WaitsOutTheDefaultDeadlineWhilePending) {
  harness.answers_polls({{200, "{}"}});
  const auto result = harness.run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Why::timed_out);
  EXPECT_EQ(result.error().reached, State::pending);
  EXPECT_EQ(harness.polls(), 7);
  EXPECT_EQ(std::accumulate(harness.waiter.waits.begin(),
                            harness.waiter.waits.end(), 0),
            10000);
  EXPECT_EQ(harness.progress, (States{State::pending}));
}

TEST_F(TitleStartPoll, BoundsEachRequestByTheRemainingTime) {
  harness.timeout_ms = 4000;
  harness.poll_interval_ms = 1000;
  Reply slow = StartHarness::status("executing");
  slow.elapsed_ms = 1200;
  harness.answers_polls({slow});
  const auto result = harness.run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Why::timed_out);
  EXPECT_EQ(result.error().reached, State::pending);
  ASSERT_GE(harness.http.seen.size(), 3U);
  EXPECT_EQ(harness.http.seen.front().total_timeout.count(), 4000);
  EXPECT_EQ(harness.http.seen[1].total_timeout.count(), 4000);
  EXPECT_EQ(harness.http.seen[2].total_timeout.count(), 1800);
  EXPECT_EQ(harness.http.seen[2].connect_timeout.count(), 1800);
  EXPECT_EQ(harness.polls(), 2);
  EXPECT_EQ(harness.waiter.waits, (Waits{1000, 600}));
}

TEST(TitleStartFrequencyControl, IsHonouredAndClamped) {
  struct Case {
    std::string header;
    int wait;
  };
  for (const auto& entry : std::vector<Case>{{"3", 3000},
                                             {" 2 ", 2000},
                                             {"1", 1000},
                                             {"20", 10000},
                                             {"999999999", 10000},
                                             {"0", 1500},
                                             {"-1", 1500},
                                             {"1.5", 1500},
                                             {"abc", 1500},
                                             {"", 1500},
                                             {"1234567890", 1500}}) {
    StartHarness harness;
    harness.timeout_ms = 60000;
    harness.answers_post(StartHarness::accepted());
    harness.answers_polls({StartHarness::with_header(
                               StartHarness::status("executing"), entry.header),
                           StartHarness::status("completed")});
    const auto result = harness.run();
    ASSERT_TRUE(result) << entry.header;
    EXPECT_EQ(result.value(), State::completed) << entry.header;
    EXPECT_EQ(harness.waiter.waits, (Waits{entry.wait})) << entry.header;
  }
}

TEST_F(TitleStartPoll, FollowsTheFrequencyControlOfEachReply) {
  harness.timeout_ms = 60000;
  harness.answers_polls(
      {StartHarness::with_header(StartHarness::status("executing"), "3"),
       StartHarness::with_header(StartHarness::status("executing"), "20"),
       StartHarness::with_header(StartHarness::status("executing"), "junk"),
       StartHarness::status("executing"), StartHarness::status("completed")});
  const auto result = harness.run();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value(), State::completed);
  EXPECT_EQ(harness.waiter.waits, (Waits{3000, 10000, 1500, 1500}));
}

TEST_F(TitleStartPoll, IgnoresFailuresUntilASuccess) {
  harness.answers_polls({{500, "err"},
                         Reply::transport(),
                         {200, "not json"},
                         {404, ""},
                         StartHarness::status("completed")});
  const auto result = harness.run();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value(), State::completed);
  EXPECT_EQ(harness.polls(), 5);
}

TEST(TitleStartPollRefusal, EndsTheStart) {
  for (const int code : {401, 403}) {
    StartHarness harness;
    harness.account.renews_store_access(StartHarness::renewed_token);
    harness.answers_post(StartHarness::accepted());
    harness.answers_polls(
        {StartHarness::status("executing", "command"), {code, ""}});
    const auto result = harness.run();
    ASSERT_FALSE(result) << code;
    EXPECT_EQ(result.error().reason, Why::unanswered) << code;
    ASSERT_TRUE(result.error().unanswered) << code;
    EXPECT_EQ(result.error().unanswered->reason,
              Unanswered::Reason::store_access_lapsed)
        << code;
    EXPECT_EQ(result.error().unanswered->http_status, code) << code;
    EXPECT_EQ(result.error().reached, State::starting) << code;
  }
}

TEST(TitleStartCommandStatus, IsReadFromEveryShape) {
  struct Case {
    std::string body;
    State state;
  };
  for (const auto& entry : std::vector<Case>{
           {"{\"command\":{\"commandStatus\":\" Completed \",\"actions\":[]}}",
            State::completed},
           {"{\"commandStatus\":\"executing\",\"actions\":[{\"action\":{"
            "\"actionType\":\"command\"}}]}",
            State::starting},
           {"{\"commands\":[]}", State::pending},
           {"{}", State::pending},
           {"[]", State::pending}}) {
    StartHarness harness;
    harness.timeout_ms = 1000;
    harness.answers_post(StartHarness::accepted());
    harness.answers_polls({{200, entry.body}});
    const auto result = harness.run();
    if (entry.state == State::completed) {
      ASSERT_TRUE(result) << entry.body;
      EXPECT_EQ(result.value(), entry.state) << entry.body;
    } else {
      ASSERT_FALSE(result) << entry.body;
      EXPECT_EQ(result.error().reason, Why::timed_out) << entry.body;
      EXPECT_EQ(result.error().reached, entry.state) << entry.body;
    }
  }
}

TEST(TitleStartCommandStatus, AdvancesOnlyOnTheCommandAction) {
  EXPECT_EQ(TitleStart::next(State::starting,
                             CommandState{"executing", "", std::nullopt}),
            State::starting);
  EXPECT_EQ(
      TitleStart::next(State::pending,
                       CommandState{"executing", "connect", std::nullopt}),
      State::pending);
  EXPECT_EQ(TitleStart::next(State::pending,
                             CommandState{"COMPLETED", "", std::nullopt}),
            State::completed);
}

TEST(TitleStartCommandStatus, TrimsTheCommandId) {
  StartHarness harness;
  harness.answers_post({202, "{\"commandId\":\" abc \"}"});
  harness.http.on("GET",
                  std::string{StartHarness::commands_url} +
                      "?platform=PS5&commandIds=abc&includeFields=actions,"
                      "commandDetail&commandStatus=executing,completed",
                  {StartHarness::status("completed")});
  const auto result = harness.run();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value(), State::completed);
  EXPECT_TRUE(harness.http.unmatched.empty());
}

}  // namespace
}  // namespace consoles_test
