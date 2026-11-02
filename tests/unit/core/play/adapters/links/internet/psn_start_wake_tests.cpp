#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/psn_start_rig.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace play_test {
namespace {

namespace platform = libchiaking::platform;
namespace psn_remote_play = libchiaking::psn::remote_play;
using psn_start_rig::Bodies;
using psn_start_rig::StartRig;
using Order = std::vector<std::string>;
using Lines = std::vector<std::string_view>;
using CreationWaitError = psn_remote_play::CreationWaitError;
using PsnFailure = libchiaking::play::PsnFailure;

class PsnStartWake : public ::testing::Test {
 protected:
  enum class Family { ps4, ps5 };

  static void keeps_the_session_open_when_the_ready_wait_ends(Family family) {
    for (const bool cancel : {false, true})
      for (const bool close_failure : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << "cancel " << cancel << " close failure "
                     << close_failure);
        StartRig rig{{Bodies::created, Bodies::members, Bodies::ready_members}};
        auto stop = std::make_shared<test_support::StopState>();
        rig.script->fail_close = close_failure;
        rig.script->on_close = [&] { rig.http.order.push_back("close"); };
        rig.script->on_exhaustion = [&] {
          rig.clock.now_us = 300;
          stop->stopped = cancel;
        };
        rig.script->on_drain = [&] {
          EXPECT_EQ(rig.clock.now_us, 300U);
          rig.clock.now_us = 300 + 3'000'000;
        };
        auto opened = rig.open(platform::StopToken{stop}, 300);
        ASSERT_TRUE(opened);
        auto result =
            family == Family::ps5
                ? rig.wake_ps5(opened.value(), platform::StopToken{stop}, 300)
                : rig.wake_ps4(opened.value(), platform::StopToken{stop}, 300);
        ASSERT_FALSE(result);
        const auto* failure = result.error().as<PsnFailure>();
        ASSERT_NE(failure, nullptr);
        const auto* waited = failure->as<PsnFailure::Wait>();
        ASSERT_NE(waited, nullptr);
        EXPECT_EQ(waited->error, cancel ? CreationWaitError::cancelled
                                        : CreationWaitError::timed_out);
        EXPECT_EQ(rig.script->close, 0U);
        EXPECT_EQ(rig.releases(), 0);
        EXPECT_TRUE(opened.value().connection());
        EXPECT_FALSE(opened.value().lease().release_attempted());
      }
  }
};

TEST_F(PsnStartWake, Ps5KeepsCheckingTheSessionUntilTheConsoleIsReady) {
  for (const bool reverse : {false, true}) {
    SCOPED_TRACE(reverse ? "reversed" : "in order");
    StartRig rig{reverse ? Lines{Bodies::members, Bodies::created,
                                 Bodies::ready_custom, Bodies::ready_members}
                         : Lines{Bodies::created, Bodies::members,
                                 Bodies::ready_members, Bodies::ready_custom}};
    rig.http.check_status = 503;
    rig.script->on_receive = [&] { rig.http.order.push_back("notification"); };
    auto opened = rig.open({}, 300);
    ASSERT_TRUE(opened);
    auto started = rig.wake_ps5(opened.value(), {}, 300);
    EXPECT_TRUE(started);
    EXPECT_EQ(
        rig.http.order,
        (Order{"fqdn", "create", "notification", "view", "notification", "view",
               "start", "notification", "status", "notification", "status"}));
    EXPECT_TRUE(opened.value().connection().close());
    EXPECT_TRUE(opened.value().lease().release({}));
  }
}

TEST_F(PsnStartWake, Ps5KeepsTheSessionOpenWhenTheReadyWaitEnds) {
  keeps_the_session_open_when_the_ready_wait_ends(Family::ps5);
}

TEST_F(PsnStartWake, Ps4KeepsTheSessionOpenWhenTheReadyWaitEnds) {
  keeps_the_session_open_when_the_ready_wait_ends(Family::ps4);
}

TEST_F(PsnStartWake, Ps5StartsOnceTheConsoleJoinedAndSentItsData) {
  StartRig rig{{Bodies::created, Bodies::members, Bodies::ready_members,
                Bodies::ready_custom}};
  auto opened = rig.open({}, 200);
  ASSERT_TRUE(opened);
  auto started = rig.wake_ps5(opened.value(), {}, 300);
  ASSERT_TRUE(started);
  EXPECT_EQ(started.value().device_uid, StartRig::uid);
  EXPECT_EQ(started.value().custom_data1, StartRig::data);
  EXPECT_EQ(rig.http.order, (Order{"fqdn", "create", "view", "view", "start",
                                   "status", "status"}));
  EXPECT_EQ(rig.script->receive, 4U);
  EXPECT_EQ(rig.script->close, 0U);
  EXPECT_TRUE(opened.value().connection().close());
  EXPECT_TRUE(opened.value().lease().release({}));
}

TEST_F(PsnStartWake, Ps5ReportsARefusedStartCommand) {
  StartRig rig{{Bodies::created, Bodies::members}};
  rig.http.start_status = 401;
  auto opened = rig.open({}, 200);
  ASSERT_TRUE(opened);
  auto outcome = rig.wake_ps5(opened.value(), {}, 300);
  ASSERT_NO_FATAL_FAILURE(rig.expect_psn_failure(outcome, false));
  EXPECT_EQ(rig.http.order, (Order{"fqdn", "create", "view", "view", "start"}));
  EXPECT_EQ(rig.script->close, 0U);
  EXPECT_TRUE(opened.value().connection());
}

TEST_F(PsnStartWake, Ps5ReportsAReadyWaitThatFails) {
  StartRig rig{{Bodies::created, Bodies::members, Bodies::ready_members}};
  auto opened = rig.open({}, 200);
  ASSERT_TRUE(opened);
  auto outcome = rig.wake_ps5(opened.value(), {}, 300);
  ASSERT_NO_FATAL_FAILURE(rig.expect_psn_failure(outcome, true));
  EXPECT_EQ(rig.http.order,
            (Order{"fqdn", "create", "view", "view", "start", "status"}));
  EXPECT_EQ(rig.script->close, 0U);
  EXPECT_TRUE(opened.value().connection());
}

TEST_F(PsnStartWake, Ps4WakesThroughTheProfileServer) {
  StartRig rig{{Bodies::created, Bodies::members, Bodies::ready_members,
                Bodies::ready_custom}};
  auto opened = rig.open({}, 200);
  ASSERT_TRUE(opened);
  auto started = rig.wake_ps4(opened.value(), {}, 300);
  ASSERT_TRUE(started);
  EXPECT_EQ(started.value().device_uid, StartRig::uid);
  EXPECT_EQ(rig.http.order, (Order{"fqdn", "create", "view", "view", "profile",
                                   "wake", "status", "status"}));
  EXPECT_EQ(rig.script->receive, 4U);
  EXPECT_EQ(rig.script->close, 0U);
  EXPECT_TRUE(opened.value().connection().close());
  EXPECT_TRUE(opened.value().lease().release({}));
}

TEST_F(PsnStartWake, Ps4ReportsARefusedProfileLookup) {
  StartRig rig{{Bodies::created, Bodies::members, Bodies::ready_members,
                Bodies::ready_custom}};
  rig.http.profile_status = 401;
  auto opened = rig.open({}, 200);
  ASSERT_TRUE(opened);
  auto started = rig.wake_ps4(opened.value(), {}, 300);
  ASSERT_NO_FATAL_FAILURE(rig.expect_psn_failure(started, false));
  EXPECT_EQ(rig.script->close, 0U);
  EXPECT_EQ(rig.releases(), 0);
}

TEST_F(PsnStartWake, Ps4ReportsARefusedWake) {
  StartRig rig{{Bodies::created, Bodies::members, Bodies::ready_members,
                Bodies::ready_custom}};
  rig.http.wake_status = 401;
  auto opened = rig.open({}, 200);
  ASSERT_TRUE(opened);
  auto started = rig.wake_ps4(opened.value(), {}, 300);
  ASSERT_NO_FATAL_FAILURE(rig.expect_psn_failure(started, false));
  EXPECT_EQ(rig.script->close, 0U);
  EXPECT_EQ(rig.releases(), 0);
}

TEST_F(PsnStartWake, Ps4ReportsAReadyWaitThatFails) {
  StartRig rig{{Bodies::created, Bodies::members, Bodies::ready_members}};
  auto opened = rig.open({}, 200);
  ASSERT_TRUE(opened);
  auto started = rig.wake_ps4(opened.value(), {}, 300);
  ASSERT_NO_FATAL_FAILURE(rig.expect_psn_failure(started, true));
  EXPECT_EQ(rig.script->close, 0U);
  EXPECT_EQ(rig.releases(), 0);
}

}  // namespace
}  // namespace play_test
