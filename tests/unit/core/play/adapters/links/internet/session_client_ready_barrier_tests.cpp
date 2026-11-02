#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/session_client_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test {
namespace {

namespace psn_remote_play = libchiaking::psn::remote_play;
namespace rest = libchiaking::psn::rest;
using psn_remote_play::ConsoleReadyBarrier;
using Error = psn_remote_play::CreationWaitError;
using Notes = session_client_rig::Notifications;
using Type = rest::NotificationType;
using Next = libchiaking::Expected<rest::Notification, Error>;

class ConsoleReadyTransitions : public ::testing::Test {
 protected:
  static constexpr std::uint64_t attempt = 9;
  static constexpr std::array<std::byte, 32> zero_uid{};
  static constexpr std::array<std::byte, 16> zero_data{};

  static auto expected_zero() -> std::optional<std::array<std::byte, 32>> {
    return std::array<std::byte, 32>{};
  }
  static auto member(std::string_view body) -> rest::Notification {
    return Notes::typed(Type::members_created, body);
  }
  static auto data1(std::string_view body) -> rest::Notification {
    return Notes::typed(Type::custom_data1_updated, body);
  }

  const std::string uid_zero = Notes::member_uid_body(std::string(64, '0'));
  const std::string uid_one = Notes::member_uid_body(std::string(64, '1'));
  const std::string data_zero = Notes::custom_data1_body(Notes::data_zero);
  const std::string data_one = Notes::custom_data1_body(Notes::data_one);
};

TEST_F(ConsoleReadyTransitions, IsReadyOnceJoinedThenGivenData) {
  ConsoleReadyBarrier barrier{attempt, expected_zero()};
  EXPECT_FALSE(barrier.device_uid());
  EXPECT_FALSE(barrier.custom_data1());
  EXPECT_FALSE(barrier.ready());
  EXPECT_FALSE(barrier.failed());
  EXPECT_TRUE(barrier.on_notification(attempt, member(uid_zero)));
  EXPECT_FALSE(barrier.ready());
  EXPECT_TRUE(barrier.on_notification(attempt, data1(data_zero)));
  EXPECT_TRUE(barrier.ready());
  EXPECT_FALSE(barrier.failed());
  EXPECT_EQ(barrier.device_uid(), zero_uid);
  EXPECT_EQ(barrier.custom_data1(), zero_data);
}

TEST_F(ConsoleReadyTransitions, IsReadyOnceGivenDataThenJoined) {
  ConsoleReadyBarrier barrier{attempt, expected_zero()};
  EXPECT_TRUE(barrier.on_notification(attempt, data1(data_zero)));
  EXPECT_FALSE(barrier.ready());
  EXPECT_TRUE(barrier.on_notification(attempt, member(uid_zero)));
  EXPECT_TRUE(barrier.ready());
}

TEST_F(ConsoleReadyTransitions, LearnsTheFirstUidWhenNoneIsExpected) {
  ConsoleReadyBarrier barrier{attempt, std::nullopt};
  EXPECT_TRUE(barrier.on_notification(attempt, member(uid_one)));
  EXPECT_TRUE(barrier.on_notification(attempt, data1(data_zero)));
  EXPECT_TRUE(barrier.ready());
}

TEST_F(ConsoleReadyTransitions, FailsOnAnUnexpectedUid) {
  ConsoleReadyBarrier barrier{attempt, expected_zero()};
  const auto mismatch = barrier.on_notification(attempt, member(uid_one));
  ASSERT_FALSE(mismatch);
  EXPECT_TRUE(mismatch.error().is<rest::bad_reply::InvalidDeviceUid>());
  EXPECT_TRUE(barrier.failed());
  EXPECT_FALSE(barrier.ready());
}

TEST_F(ConsoleReadyTransitions, TakesTheLatestUidWhileWaiting) {
  ConsoleReadyBarrier barrier{attempt, std::nullopt};
  EXPECT_TRUE(barrier.on_notification(attempt, member(uid_zero)));
  EXPECT_TRUE(barrier.on_notification(attempt, member(uid_zero)));
  EXPECT_FALSE(barrier.ready());
  EXPECT_FALSE(barrier.failed());
  EXPECT_TRUE(barrier.on_notification(attempt, member(uid_one)));
  EXPECT_FALSE(barrier.failed());
  ASSERT_TRUE(barrier.device_uid());
  EXPECT_NE(*barrier.device_uid(), zero_uid);
}

TEST_F(ConsoleReadyTransitions, TakesTheLatestDataWhileWaiting) {
  ConsoleReadyBarrier barrier{attempt, expected_zero()};
  EXPECT_TRUE(barrier.on_notification(attempt, data1(data_zero)));
  EXPECT_TRUE(barrier.on_notification(attempt, data1(data_zero)));
  EXPECT_TRUE(barrier.on_notification(attempt, data1(data_one)));
  EXPECT_FALSE(barrier.failed());
  ASSERT_TRUE(barrier.custom_data1());
  EXPECT_NE(*barrier.custom_data1(), zero_data);
}

TEST_F(ConsoleReadyTransitions, IgnoresAStaleAttemptAndUnrelatedNotifications) {
  ConsoleReadyBarrier barrier{attempt, expected_zero()};
  EXPECT_TRUE(barrier.on_notification(attempt + 1, member(uid_one)));
  EXPECT_TRUE(
      barrier.on_notification(attempt, Notes::typed(Type::unknown, "{}")));
  EXPECT_FALSE(barrier.ready());
  EXPECT_FALSE(barrier.failed());
  barrier.on_timeout(attempt + 1);
  barrier.on_cancel(attempt + 1);
  EXPECT_FALSE(barrier.failed());
}

TEST_F(ConsoleReadyTransitions, FailsForGoodOnAMalformedNotification) {
  ConsoleReadyBarrier barrier{attempt, expected_zero()};
  const auto malformed = barrier.on_notification(attempt, member("{}"));
  ASSERT_FALSE(malformed);
  EXPECT_TRUE(malformed.error().is<rest::bad_reply::InvalidSchema>());
  EXPECT_TRUE(barrier.failed());
  EXPECT_TRUE(barrier.on_notification(attempt, member(uid_zero)));
  EXPECT_TRUE(barrier.failed());
  EXPECT_FALSE(barrier.ready());
}

TEST_F(ConsoleReadyTransitions, FailsOnATimeoutOfItsAttempt) {
  ConsoleReadyBarrier barrier{attempt, expected_zero()};
  barrier.on_timeout(attempt);
  EXPECT_TRUE(barrier.failed());
  EXPECT_FALSE(barrier.ready());
}

TEST_F(ConsoleReadyTransitions, FailsOnACancelOfItsAttempt) {
  ConsoleReadyBarrier barrier{attempt, expected_zero()};
  barrier.on_cancel(attempt);
  EXPECT_TRUE(barrier.failed());
}

TEST_F(ConsoleReadyTransitions, StaysReadyWhateverArrivesLater) {
  ConsoleReadyBarrier barrier{attempt, expected_zero()};
  EXPECT_TRUE(barrier.on_notification(attempt, member(uid_zero)));
  EXPECT_TRUE(barrier.on_notification(attempt, data1(data_zero)));
  EXPECT_TRUE(barrier.ready());
  EXPECT_TRUE(barrier.on_notification(attempt, member(uid_one)));
  EXPECT_TRUE(barrier.on_notification(attempt, data1(data_one)));
  barrier.on_timeout(attempt);
  barrier.on_cancel(attempt);
  EXPECT_TRUE(barrier.ready());
  EXPECT_FALSE(barrier.failed());
}

class ConsoleReadyWait : public ConsoleReadyTransitions {};

TEST_F(ConsoleReadyWait, ReadsUntilReadyInEitherOrder) {
  for (const bool data_first : {false, true}) {
    SCOPED_TRACE(data_first ? "data first" : "joined first");
    ConsoleReadyBarrier barrier{9, zero_uid};
    std::vector<rest::Notification> script;
    script.push_back(Notes::typed(Type::unknown, "{}"));
    if (data_first) {
      script.push_back(data1(data_zero));
      script.push_back(member(uid_zero));
    } else {
      script.push_back(member(uid_zero));
      script.push_back(data1(data_zero));
    }
    std::size_t index = 0;
    auto next = [&]() -> Next {
      if (index >= script.size()) {
        ADD_FAILURE() << "read past the script";
        return libchiaking::unexpected(Error::receive_failed);
      }
      return std::move(script[index++]);
    };
    auto ready = barrier.wait(9, next);
    ASSERT_TRUE(ready);
    EXPECT_EQ(ready.value().device_uid, zero_uid);
    EXPECT_EQ(ready.value().custom_data1, zero_data);
    EXPECT_EQ(index, 3U);
  }
}

TEST_F(ConsoleReadyWait, RefusesAStaleAttemptWithoutReading) {
  ConsoleReadyBarrier barrier{9, zero_uid};
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

TEST_F(ConsoleReadyWait, KeepsReadingPastAnyNumberOfUnrelatedNotifications) {
  ConsoleReadyBarrier barrier{9, zero_uid};
  unsigned calls = 0;
  auto next = [&]() -> Next {
    ++calls;
    if (calls <= 200) return Notes::typed(Type::unknown, "{}");
    if (calls == 201) return member(uid_zero);
    return data1(data_zero);
  };
  auto unbounded = barrier.wait(9, next);
  ASSERT_TRUE(unbounded);
  EXPECT_EQ(unbounded.value().device_uid, zero_uid);
  EXPECT_EQ(calls, 202U);
}

TEST_F(ConsoleReadyWait, FailsOnAMalformedNotification) {
  ConsoleReadyBarrier barrier{9, zero_uid};
  auto malformed = []() -> Next { return member("{}"); };
  auto result = barrier.wait(9, malformed);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), Error::invalid_notification);
}

TEST_F(ConsoleReadyWait, PassesOnATimeoutOrCancel) {
  for (const auto error : {Error::timed_out, Error::cancelled}) {
    SCOPED_TRACE(::testing::PrintToString(error));
    ConsoleReadyBarrier barrier{9, zero_uid};
    auto next = [error]() -> Next { return libchiaking::unexpected(error); };
    auto result = barrier.wait(9, next);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), error);
    EXPECT_TRUE(barrier.failed());
  }
}

}  // namespace
}  // namespace play_test
