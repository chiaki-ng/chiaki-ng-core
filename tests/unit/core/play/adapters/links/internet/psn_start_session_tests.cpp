#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/psn_start_rig.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace play_test {
namespace {

namespace platform = libchiaking::platform;
namespace psn_remote_play = libchiaking::psn::remote_play;
using psn_start_rig::Bodies;
using psn_start_rig::StartRig;
using Order = std::vector<std::string>;
using StartError = psn_remote_play::StartError;
using WaitFailed = StartError::WaitFailed;
using CreationWaitError = psn_remote_play::CreationWaitError;

TEST(PsnStartSession, OpensOnceTheSessionIsCreatedAndTheConsoleJoined) {
  StartRig rig{{Bodies::created, Bodies::members}};
  auto opened = rig.open({}, 200);
  ASSERT_TRUE(opened);
  EXPECT_EQ(opened.value().online_id(), "userA");
  EXPECT_EQ(rig.http.order, (Order{"fqdn", "create", "view", "view"}));
  EXPECT_EQ(rig.script->connect, 1U);
  EXPECT_EQ(rig.script->receive, 2U);
  EXPECT_EQ(rig.script->close, 0U);
  EXPECT_EQ(rig.random.calls, 1U);
  EXPECT_TRUE(opened.value().connection().close());
  EXPECT_TRUE(opened.value().lease().release({}));
  EXPECT_EQ(rig.script->close, 1U);
  EXPECT_EQ(rig.http.order.back(), "release");
}

TEST(PsnStartSession, ClosesThePushConnectionWhenItFailsBeforeTheConsoleJoins) {
  StartRig rig{{Bodies::created}};
  auto failed = rig.open({}, 200);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().kind(), StartError::Kind::wait_failed);
  const auto* waited = failed.error().as<WaitFailed>();
  ASSERT_NE(waited, nullptr);
  EXPECT_EQ(waited->wait, CreationWaitError::receive_failed);
  ASSERT_TRUE(waited->push);
  EXPECT_EQ(waited->push->kind,
            psn_remote_play::PushErrorKind::websocket_failure);
  EXPECT_FALSE(waited->release_failure.has_value());
  EXPECT_TRUE(waited->release_skipped);
  EXPECT_EQ(rig.http.order, (Order{"fqdn", "create", "view"}));
  EXPECT_EQ(rig.script->close, 1U);
  EXPECT_EQ(rig.script->receive, 2U);
}

TEST(PsnStartSession, StopsWaitingWhenPsnDeletesItsOwnSession) {
  StartRig rig{{Bodies::created, Bodies::own_session_deleted}};
  auto failed = rig.open({}, 200);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().kind(), StartError::Kind::wait_failed);
  const auto* waited = failed.error().as<WaitFailed>();
  ASSERT_NE(waited, nullptr);
  ASSERT_TRUE(waited->push);
  EXPECT_EQ(waited->push->kind,
            psn_remote_play::PushErrorKind::session_deleted);
  EXPECT_TRUE(waited->release_skipped);
  EXPECT_FALSE(waited->deletion_confirmed);
  EXPECT_EQ(rig.http.order, (Order{"fqdn", "create", "view"}));
  EXPECT_EQ(rig.script->close, 1U);
  EXPECT_EQ(rig.script->receive, 2U);
}

TEST(PsnStartSession, ReleasesTheSessionAndDrainsItsDeletionWhenCancelled) {
  StartRig rig{{Bodies::created}};
  auto stop = std::make_shared<test_support::StopState>();
  rig.script->on_exhaustion = [&] { stop->stopped = true; };
  rig.script->after_exhaustion = {Bodies::members, Bodies::own_session_deleted};
  auto failed = rig.open(platform::StopToken{stop}, 200);
  ASSERT_FALSE(failed);
  const auto* waited = failed.error().as<WaitFailed>();
  ASSERT_NE(waited, nullptr);
  EXPECT_EQ(waited->wait, CreationWaitError::cancelled);
  EXPECT_FALSE(waited->release_skipped);
  EXPECT_TRUE(waited->deletion_confirmed);
  EXPECT_EQ(rig.http.order, (Order{"fqdn", "create", "view", "release"}));
  EXPECT_EQ(rig.script->close, 1U);
  EXPECT_EQ(rig.script->receive, 4U);
}

TEST(PsnStartSession, ReleasesTheSessionWhenTheWaitEndsAfterAnotherMemberLeft) {
  for (const bool cancel : {false, true}) {
    SCOPED_TRACE(cancel ? "cancelled" : "timed out");
    StartRig rig{{Bodies::created, Bodies::members, Bodies::member_deleted}};
    auto stop = std::make_shared<test_support::StopState>();
    rig.http.on_check = [&] {
      if (cancel)
        stop->stopped = true;
      else
        rig.clock.now_us = 300;
    };
    auto opened = rig.open(platform::StopToken{stop}, 300);
    ASSERT_FALSE(opened);
    const auto* waited = opened.error().as<WaitFailed>();
    ASSERT_NE(waited, nullptr);
    EXPECT_EQ(waited->wait, cancel ? CreationWaitError::cancelled
                                   : CreationWaitError::timed_out);
    EXPECT_FALSE(waited->release_skipped);
    EXPECT_TRUE(waited->deletion_confirmed);
    EXPECT_EQ(rig.script->receive, 3U);
    EXPECT_EQ(rig.script->close, 1U);
    EXPECT_EQ(rig.http.order, (Order{"fqdn", "create", "view", "release"}));
  }
}

TEST(PsnStartSession, ClosesAndReleasesWhenTheCreationWaitEnds) {
  for (const bool cancel : {false, true})
    for (const bool close_failure : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "cancel " << cancel
                                        << " close failure " << close_failure);
      StartRig rig{{Bodies::created}};
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
      ASSERT_FALSE(opened);
      EXPECT_EQ(opened.error().kind(), StartError::Kind::wait_failed);
      const auto* waited = opened.error().as<WaitFailed>();
      ASSERT_NE(waited, nullptr);
      EXPECT_EQ(waited->wait, cancel ? CreationWaitError::cancelled
                                     : CreationWaitError::timed_out);
      EXPECT_FALSE(waited->release_skipped);
      EXPECT_FALSE(waited->deletion_confirmed);
      EXPECT_EQ(rig.clock.now_us, 300U + 3'000'000U);
      EXPECT_EQ(rig.script->receive, 3U);
      EXPECT_EQ(waited->close_failure.has_value(), close_failure);
      if (close_failure) {
        EXPECT_EQ(waited->close_failure,
                  std::optional{platform::WebSocketError::backend_failure});
      }
      EXPECT_EQ(rig.script->close, 1U);
      EXPECT_EQ(rig.releases(), 1);
      ASSERT_GE(rig.http.order.size(), 2U);
      EXPECT_EQ(rig.http.order.back(), "close");
      EXPECT_EQ(rig.http.order[rig.http.order.size() - 2], "release");
    }
}

TEST(PsnStartSession, ClosesThePushConnectionWhenPsnRefusesTheCreate) {
  StartRig rig{{}};
  rig.http.create_status = 401;
  auto failed = rig.open({}, 200);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().kind(), StartError::Kind::create_failed);
  EXPECT_EQ(rig.http.order, (Order{"fqdn", "create"}));
  EXPECT_EQ(rig.script->close, 1U);
}

TEST(PsnStartSession, TimesOutWhenARefusedCreateOutlastsItsWait) {
  StartRig rig{{}};
  rig.http.create_status = 503;
  rig.http.on_create = [&] { rig.clock.now_us += 150; };
  auto failed =
      rig.open({}, libchiaking::net::RemoteWaitDeadline::per_wait(100));
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().kind(), StartError::Kind::timed_out);
  EXPECT_EQ(rig.http.order, (Order{"fqdn", "create"}));
  EXPECT_EQ(rig.script->connect, 1U);
  EXPECT_EQ(rig.script->close, 1U);
}

TEST(PsnStartSession, ReportsARefusedCreateThatEndsWithinItsWait) {
  StartRig rig{{}};
  rig.http.create_status = 503;
  rig.http.on_create = [&] { rig.clock.now_us += 50; };
  auto failed =
      rig.open({}, libchiaking::net::RemoteWaitDeadline::per_wait(100));
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().kind(), StartError::Kind::create_failed);
  EXPECT_EQ(rig.script->close, 1U);
}

TEST(PsnStartSession, GivesEveryWaitItsOwnDeadline) {
  StartRig rig{{Bodies::created, Bodies::members}};
  rig.http.on_create = [&] { rig.clock.now_us += 90; };
  rig.script->on_receive = [&] { rig.clock.now_us += 90; };
  auto opened =
      rig.open({}, libchiaking::net::RemoteWaitDeadline::per_wait(100));
  ASSERT_TRUE(opened);
  EXPECT_EQ(opened.value().online_id(), "userA");
  EXPECT_EQ(rig.http.order, (Order{"fqdn", "create", "view", "view"}));
}

TEST(PsnStartSession, RefusesAStaleAttemptWithoutContactingPsn) {
  StartRig rig{{}};
  auto stale = rig.open({}, 200, 6);
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error().kind(), StartError::Kind::invalid_attempt);
  EXPECT_TRUE(rig.http.order.empty());
  EXPECT_EQ(rig.script->connect, 0U);
}

TEST(PsnStartSession, RefusesAPassedDeadlineWithoutContactingPsn) {
  StartRig rig{{}};
  auto expired = rig.open({}, 100);
  ASSERT_FALSE(expired);
  EXPECT_EQ(expired.error().kind(), StartError::Kind::timed_out);
  EXPECT_TRUE(rig.http.order.empty());
  EXPECT_EQ(rig.script->connect, 0U);
}

}  // namespace
}  // namespace play_test
