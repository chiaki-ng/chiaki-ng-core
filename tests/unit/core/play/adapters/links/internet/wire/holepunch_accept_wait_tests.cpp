#include "src/core/net/nat/holepunch/holepunch_codec.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_accept.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace internet = libchiaking::play::internet;
using State = internet::holepunch::AcceptWaitState;

class HolepunchAcceptWait : public ::testing::Test {
 protected:
  static auto message(nat::Action action, std::uint16_t req_id = 0,
                      std::uint16_t error = 0) -> nat::SessionMessage {
    nat::SessionMessage out{};
    out.action = action;
    out.req_id = req_id;
    out.error = error;
    return out;
  }

  internet::holepunch::AcceptWait wait_;
};

TEST_F(HolepunchAcceptWait, BeginsOneNonZeroAttemptAtATime) {
  EXPECT_FALSE(wait_.begin(0));
  EXPECT_TRUE(wait_.begin(7));
  EXPECT_FALSE(wait_.begin(8));
  EXPECT_EQ(wait_.state(), State::waiting_accept);
}

TEST_F(HolepunchAcceptWait, WaitsThroughOtherAttemptsAndActions) {
  ASSERT_TRUE(wait_.begin(7));
  wait_.on_message(8, message(nat::Action::accept, 0x1111));
  wait_.on_message(7, message(nat::Action::offer, 0x1111));
  EXPECT_EQ(wait_.state(), State::waiting_accept);
  EXPECT_FALSE(wait_.take_ack());
}

TEST_F(HolepunchAcceptWait, AcknowledgesTheFirstAcceptWhateverItsError) {
  ASSERT_TRUE(wait_.begin(7));
  const auto accept = message(nat::Action::accept, 0x1111, 1);
  wait_.on_message(7, accept);
  EXPECT_EQ(wait_.state(), State::ack_pending);
  const auto ack = wait_.take_ack();
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->attempt, 7U);
  EXPECT_EQ(ack->peer_req_id, 0x1111);
  EXPECT_FALSE(wait_.take_ack());
  wait_.on_message(7, accept);
  EXPECT_FALSE(wait_.take_ack());
}

TEST_F(HolepunchAcceptWait, BecomesReadyOnceItsAcknowledgementIsSent) {
  ASSERT_TRUE(wait_.begin(7));
  wait_.on_message(7, message(nat::Action::accept, 0x1111, 1));
  ASSERT_TRUE(wait_.take_ack());
  EXPECT_FALSE(wait_.ack_sent(8));
  EXPECT_TRUE(wait_.ack_sent(7));
  EXPECT_EQ(wait_.state(), State::ready);
  EXPECT_FALSE(wait_.timeout(7));
  EXPECT_FALSE(wait_.begin(9));
}

TEST_F(HolepunchAcceptWait, AcknowledgesAnAcceptWithoutARequestId) {
  ASSERT_TRUE(wait_.begin(7));
  wait_.on_message(7, message(nat::Action::accept));
  EXPECT_EQ(wait_.state(), State::ack_pending);
  const auto ack = wait_.take_ack();
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->peer_req_id, 0);
}

TEST_F(HolepunchAcceptWait, IgnoresAnAcceptAfterItsAttemptTimedOut) {
  ASSERT_TRUE(wait_.begin(3));
  EXPECT_FALSE(wait_.timeout(2));
  EXPECT_TRUE(wait_.timeout(3));
  wait_.on_message(3, message(nat::Action::accept, 0x1111, 1));
  EXPECT_EQ(wait_.state(), State::failed);
  EXPECT_FALSE(wait_.take_ack());
}

TEST_F(HolepunchAcceptWait, FailsWhenItsOwnAttemptIsCancelled) {
  ASSERT_TRUE(wait_.begin(5));
  EXPECT_FALSE(wait_.cancel(4));
  EXPECT_TRUE(wait_.cancel(5));
  EXPECT_EQ(wait_.state(), State::failed);
}

TEST_F(HolepunchAcceptWait, FailsWhenItsOwnAttemptIsTerminated) {
  ASSERT_TRUE(wait_.begin(11));
  const auto terminate = message(nat::Action::terminate);
  wait_.on_message(10, terminate);
  EXPECT_EQ(wait_.state(), State::waiting_accept);
  wait_.on_message(11, terminate);
  EXPECT_EQ(wait_.state(), State::failed);
}

TEST_F(HolepunchAcceptWait, DropsAPendingAcknowledgementWhenCancelled) {
  ASSERT_TRUE(wait_.begin(12));
  wait_.on_message(12, message(nat::Action::accept, 0x1111, 1));
  EXPECT_EQ(wait_.state(), State::ack_pending);
  EXPECT_TRUE(wait_.cancel(12));
  EXPECT_FALSE(wait_.take_ack());
  EXPECT_FALSE(wait_.ack_sent(12));
}

}  // namespace
}  // namespace play_test
