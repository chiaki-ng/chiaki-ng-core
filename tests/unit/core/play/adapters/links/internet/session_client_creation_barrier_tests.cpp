#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/session_client_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <utility>
#include <vector>

namespace play_test {
namespace {

namespace psn_remote_play = libchiaking::psn::remote_play;
namespace rest = libchiaking::psn::rest;
using psn_remote_play::CreationBarrier;
using Error = psn_remote_play::CreationWaitError;
using Notes = session_client_rig::Notifications;
using Type = rest::NotificationType;
using Next = libchiaking::Expected<rest::Notification, Error>;

class CreationBarrierTransitions : public ::testing::Test {
 protected:
  static auto created_a() -> rest::Notification {
    return Notes::typed(Type::remote_play_session_created, Notes::created_a);
  }
  static auto members() -> rest::Notification {
    return Notes::typed(Type::members_created, "{}");
  }
};

TEST_F(CreationBarrierTransitions, IsReadyOnceCreatedThenJoined) {
  CreationBarrier barrier{7};
  EXPECT_FALSE(barrier.ready());
  EXPECT_FALSE(barrier.failed());
  EXPECT_TRUE(barrier.on_notification(7, created_a()));
  EXPECT_FALSE(barrier.ready());
  EXPECT_TRUE(barrier.on_notification(7, members()));
  EXPECT_TRUE(barrier.ready());
  EXPECT_FALSE(barrier.failed());
  EXPECT_EQ(barrier.online_id(), "userA");
  EXPECT_EQ(barrier.attempt(), 7U);
}

TEST_F(CreationBarrierTransitions, IsReadyOnceJoinedThenCreated) {
  CreationBarrier barrier{7};
  EXPECT_TRUE(barrier.on_notification(7, members()));
  EXPECT_FALSE(barrier.ready());
  EXPECT_TRUE(barrier.on_notification(7, created_a()));
  EXPECT_TRUE(barrier.ready());
  EXPECT_EQ(barrier.online_id(), "userA");
}

TEST_F(CreationBarrierTransitions, TakesDuplicatesAndLateRepeatsInStride) {
  CreationBarrier barrier{1};
  EXPECT_TRUE(barrier.on_notification(1, created_a()));
  EXPECT_TRUE(barrier.on_notification(1, created_a()));
  EXPECT_TRUE(barrier.on_notification(1, members()));
  EXPECT_TRUE(barrier.ready());
  EXPECT_TRUE(barrier.on_notification(1, created_a()));
  EXPECT_TRUE(barrier.ready());
  EXPECT_FALSE(barrier.failed());
}

TEST_F(CreationBarrierTransitions, IgnoresAStaleAttempt) {
  CreationBarrier barrier{2};
  EXPECT_TRUE(barrier.on_notification(1, created_a()));
  EXPECT_FALSE(barrier.ready());
  EXPECT_FALSE(barrier.failed());
  barrier.on_timeout(1);
  barrier.on_cancel(1);
  EXPECT_FALSE(barrier.failed());
}

TEST_F(CreationBarrierTransitions, IgnoresAnUnrelatedNotification) {
  CreationBarrier barrier{3};
  EXPECT_TRUE(barrier.on_notification(3, Notes::typed(Type::unknown, "{}")));
  EXPECT_FALSE(barrier.ready());
  EXPECT_FALSE(barrier.failed());
}

TEST_F(CreationBarrierTransitions, FailsForGoodOnAMalformedCreation) {
  CreationBarrier barrier{4};
  const auto malformed = barrier.on_notification(
      4, Notes::typed(Type::remote_play_session_created, "{}"));
  ASSERT_FALSE(malformed);
  EXPECT_TRUE(malformed.error().is<rest::bad_reply::InvalidSchema>());
  EXPECT_TRUE(barrier.failed());
  EXPECT_FALSE(barrier.ready());
  EXPECT_TRUE(barrier.on_notification(4, created_a()));
  EXPECT_TRUE(barrier.failed());
}

TEST_F(CreationBarrierTransitions, FailsOnASecondOnlineId) {
  CreationBarrier barrier{5};
  EXPECT_TRUE(barrier.on_notification(5, created_a()));
  const auto conflict = barrier.on_notification(
      5, Notes::typed(Type::remote_play_session_created, Notes::created_b));
  ASSERT_FALSE(conflict);
  EXPECT_TRUE(conflict.error().is<rest::bad_reply::InvalidOnlineId>());
  EXPECT_TRUE(barrier.failed());
  EXPECT_FALSE(barrier.ready());
}

TEST_F(CreationBarrierTransitions, FailsOnATimeoutOrCancelOfItsAttempt) {
  CreationBarrier barrier{6};
  barrier.on_timeout(6);
  EXPECT_TRUE(barrier.failed());
  barrier.on_cancel(6);
  EXPECT_TRUE(barrier.failed());
}

TEST_F(CreationBarrierTransitions, StaysReadyAfterALateTimeout) {
  CreationBarrier barrier{6};
  EXPECT_TRUE(barrier.on_notification(6, created_a()));
  EXPECT_TRUE(barrier.on_notification(6, members()));
  barrier.on_timeout(6);
  EXPECT_TRUE(barrier.ready());
  EXPECT_FALSE(barrier.failed());
}

class CreationBarrierWait : public CreationBarrierTransitions {};

TEST_F(CreationBarrierWait, ReadsUntilReadyInEitherOrder) {
  for (const bool joined_first : {false, true}) {
    SCOPED_TRACE(joined_first ? "joined first" : "created first");
    CreationBarrier barrier{7};
    std::vector<rest::Notification> script;
    script.push_back(Notes::typed(Type::unknown, "{}"));
    if (joined_first) {
      script.push_back(members());
      script.push_back(created_a());
    } else {
      script.push_back(created_a());
      script.push_back(members());
    }
    std::size_t index = 0;
    auto next = [&]() -> Next {
      if (index >= script.size()) {
        ADD_FAILURE() << "read past the script";
        return libchiaking::unexpected(Error::receive_failed);
      }
      return std::move(script[index++]);
    };
    auto result = barrier.wait(7, next);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), "userA");
    EXPECT_EQ(index, 3U);
  }
}

TEST_F(CreationBarrierWait, FailsOnAMalformedCreation) {
  CreationBarrier barrier{7};
  auto next = []() -> Next {
    return Notes::typed(Type::remote_play_session_created, "{}");
  };
  auto result = barrier.wait(7, next);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), Error::invalid_notification);
  EXPECT_TRUE(barrier.failed());
}

TEST_F(CreationBarrierWait, PassesOnATimeoutOrCancel) {
  for (const auto error : {Error::timed_out, Error::cancelled}) {
    SCOPED_TRACE(::testing::PrintToString(error));
    CreationBarrier barrier{7};
    auto next = [error]() -> Next { return libchiaking::unexpected(error); };
    auto result = barrier.wait(7, next);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), error);
    EXPECT_TRUE(barrier.failed());
  }
}

TEST_F(CreationBarrierWait, RefusesAStaleAttemptWithoutReading) {
  CreationBarrier barrier{7};
  unsigned calls = 0;
  auto next = [&]() -> Next {
    ++calls;
    return Notes::typed(Type::unknown, "{}");
  };
  auto stale = barrier.wait(8, next);
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error(), Error::stale_attempt);
  EXPECT_EQ(calls, 0U);
}

TEST_F(CreationBarrierWait, KeepsReadingPastAnyNumberOfUnrelatedNotifications) {
  CreationBarrier barrier{7};
  unsigned calls = 0;
  auto next = [&]() -> Next {
    ++calls;
    if (calls <= 200) return Notes::typed(Type::unknown, "{}");
    if (calls == 201) return created_a();
    return members();
  };
  auto unbounded = barrier.wait(7, next);
  ASSERT_TRUE(unbounded);
  EXPECT_EQ(unbounded.value(), "userA");
  EXPECT_EQ(calls, 202U);
}

}  // namespace
}  // namespace play_test
