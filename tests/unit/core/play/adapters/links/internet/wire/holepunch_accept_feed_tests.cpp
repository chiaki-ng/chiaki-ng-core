#include "src/core/play/adapters/links/internet/wire/holepunch_accept.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/psn/notifications.hpp"

#include <gtest/gtest.h>

#include <string_view>
#include <variant>

namespace play_test {
namespace {

namespace internet = libchiaking::play::internet;
using State = internet::holepunch::AcceptWaitState;

class HolepunchAcceptFeed : public ::testing::Test {
 protected:
  static constexpr std::string_view accept_json =
      R"({"body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"ACCEPT\",\"reqId\":5,\"error\":0,\"connRequest\":{}}"}}}})";
  static constexpr std::string_view terminate_json =
      R"({"body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"TERMINATE\",\"reqId\":0,\"error\":0,\"connRequest\":{}}"}}}})";
  static constexpr std::string_view malformed_json =
      R"({"body":{"data":{"sessionMessage":{"payload":"body=not-json"}}}})";

  static auto notification(std::string_view json)
      -> libchiaking::psn::rest::Notification {
    return test_support::psn::Notifications::typed(
        libchiaking::psn::rest::NotificationType::session_message_created,
        json);
  }

  auto feed(std::uint64_t attempt, std::string_view json) {
    return internet::holepunch::Accept::feed_wait(wait_, attempt,
                                                  notification(json));
  }

  internet::holepunch::AcceptWait wait_;
};

TEST_F(HolepunchAcceptFeed, IgnoresAnotherAttemptsAccept) {
  ASSERT_TRUE(wait_.begin(9));
  EXPECT_TRUE(feed(8, accept_json));
  EXPECT_EQ(wait_.state(), State::waiting_accept);
}

TEST_F(HolepunchAcceptFeed, RejectsAMalformedMessageAndKeepsWaiting) {
  ASSERT_TRUE(wait_.begin(9));
  const auto malformed = feed(9, malformed_json);
  ASSERT_FALSE(malformed);
  EXPECT_TRUE(
      std::holds_alternative<libchiaking::net::nat::holepunch::MessageError>(
          malformed.error()));
  EXPECT_EQ(wait_.state(), State::waiting_accept);
}

TEST_F(HolepunchAcceptFeed, FailsOnATerminate) {
  ASSERT_TRUE(wait_.begin(9));
  EXPECT_TRUE(feed(8, accept_json));
  ASSERT_FALSE(feed(9, malformed_json));
  EXPECT_TRUE(feed(9, terminate_json));
  EXPECT_EQ(wait_.state(), State::failed);
}

TEST_F(HolepunchAcceptFeed, AcknowledgesItsOwnAttemptsAccept) {
  ASSERT_TRUE(wait_.begin(9));
  EXPECT_TRUE(feed(9, accept_json));
  EXPECT_EQ(wait_.state(), State::ack_pending);
  const auto ack = wait_.take_ack();
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->attempt, 9U);
  EXPECT_EQ(ack->peer_req_id, 5);
  EXPECT_TRUE(wait_.ack_sent(9));
  EXPECT_EQ(wait_.state(), State::ready);
}

}  // namespace
}  // namespace play_test
