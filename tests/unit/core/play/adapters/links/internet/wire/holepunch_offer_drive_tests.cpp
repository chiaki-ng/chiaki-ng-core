#include "libchiaking/detail/diagnostics/log_queue.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_offer.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/live_providers.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace internet = libchiaking::play::internet;
using DriveError = internet::holepunch::OfferPsnErrorKind;
using PsnError = internet::holepunch::OfferPsnError;

class HolepunchOfferDrive : public ::testing::Test {
 protected:
  struct Step {
    nat::SessionMessage message{};
    DriveError error{};
    bool failed{};
  };

  void offer(std::uint16_t req_id, std::uint32_t sid) {
    nat::SessionMessage message{};
    message.action = nat::Action::offer;
    message.req_id = req_id;
    message.conn_request.emplace();
    message.conn_request->sid = sid;
    steps_.push_back({std::move(message)});
  }

  void result(std::uint16_t req_id) {
    nat::SessionMessage message{};
    message.action = nat::Action::result;
    message.req_id = req_id;
    steps_.push_back({std::move(message)});
  }

  void terminate() {
    nat::SessionMessage message{};
    message.action = nat::Action::terminate;
    steps_.push_back({std::move(message)});
  }

  void fail_receive(DriveError error) { steps_.push_back({{}, error, true}); }

  auto driver(const libchiaking::diagnostics::Trace* trace = nullptr)
      -> internet::holepunch::OfferDrive {
    return internet::holepunch::OfferDrive{
        [this]() -> libchiaking::Expected<nat::SessionMessage, PsnError> {
          EXPECT_LT(receives_, steps_.size());
          if (receives_ >= steps_.size())
            return libchiaking::unexpected(PsnError{DriveError::timed_out});
          auto& step = steps_[receives_++];
          if (step.failed) return libchiaking::unexpected(PsnError{step.error});
          return std::move(step.message);
        },
        [this](const internet::holepunch::OfferEffect& effect)
            -> libchiaking::Expected<void, PsnError> {
          sent_.push_back(effect);
          if (sends_++ == fail_send_at_)
            return libchiaking::unexpected(PsnError{DriveError::send_failure});
          return {};
        },
        trace};
  }

  std::vector<Step> steps_;
  std::vector<internet::holepunch::OfferEffect> sent_;
  std::size_t receives_{};
  std::size_t sends_{};
  std::size_t fail_send_at_{std::numeric_limits<std::size_t>::max()};
};

TEST_F(HolepunchOfferDrive, AnswersTheOfferAndTracesEachState) {
  offer(0x1111, 11);
  result(0x2222);
  libchiaking::diagnostics::LogQueue logs{test_support::LiveProviders::steady()};
  test_support::FakeClock clock{0};
  const libchiaking::diagnostics::Trace trace{
      &logs, clock, libchiaking::diagnostics::Component::control_route};
  auto outcome = driver(&trace).run(7, 0x2222);
  ASSERT_TRUE(outcome);
  EXPECT_EQ(outcome.value().sid, 11U);
  ASSERT_EQ(sent_.size(), 2U);
  for (const std::string_view state :
       {"state=waiting_offer", "state=waiting_result",
        "state=ready_to_probe"}) {
    SCOPED_TRACE(state);
    const auto entry = logs.read();
    ASSERT_TRUE(entry);
    EXPECT_NE(entry.value().message().find(state), std::string_view::npos);
  }
  EXPECT_FALSE(logs.read());
  const auto* ack = std::get_if<internet::holepunch::AckPeerOffer>(&sent_[0]);
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->attempt, 7U);
  EXPECT_EQ(ack->peer_req_id, 0x1111);
  const auto* own = std::get_if<internet::holepunch::SendOwnOffer>(&sent_[1]);
  ASSERT_TRUE(own);
  EXPECT_EQ(own->attempt, 7U);
  EXPECT_EQ(own->own_req_id, 0x2222);
}

TEST_F(HolepunchOfferDrive, KeepsTheFirstOfferAndAcknowledgesRepeats) {
  offer(1, 12);
  offer(2, 13);
  result(9);
  result(3);
  auto outcome = driver().run(4, 3);
  ASSERT_TRUE(outcome);
  EXPECT_EQ(outcome.value().sid, 12U);
  ASSERT_EQ(sent_.size(), 3U);
  const auto* ack = std::get_if<internet::holepunch::AckPeerOffer>(&sent_[2]);
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->attempt, 4U);
  EXPECT_EQ(ack->peer_req_id, 2);
  EXPECT_EQ(receives_, 4U);
}

TEST_F(HolepunchOfferDrive, FailsWhenARepeatedAcknowledgementCannotBeSent) {
  fail_send_at_ = 2;
  offer(1, 12);
  offer(2, 13);
  result(3);
  auto outcome = driver().run(4, 3);
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().kind(), DriveError::send_failure);
  EXPECT_EQ(sends_, 3U);
  EXPECT_EQ(receives_, 2U);
}

TEST_F(HolepunchOfferDrive, StopsOnATerminate) {
  offer(1, 12);
  terminate();
  auto outcome = driver().run(4, 3);
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().kind(), DriveError::terminated);
  EXPECT_EQ(sent_.size(), 2U);
}

TEST_F(HolepunchOfferDrive, PassesOnAReceiveFailure) {
  for (const auto error : {DriveError::cancelled, DriveError::timed_out}) {
    SCOPED_TRACE(static_cast<int>(error));
    steps_.clear();
    sent_.clear();
    receives_ = 0;
    sends_ = 0;
    fail_receive(error);
    auto outcome = driver().run(4, 3);
    ASSERT_FALSE(outcome);
    EXPECT_EQ(outcome.error().kind(), error);
    EXPECT_TRUE(sent_.empty());
  }
}

TEST_F(HolepunchOfferDrive, FailsWhenEitherInitialSendFails) {
  for (std::size_t fail_at = 0; fail_at < 2; ++fail_at) {
    SCOPED_TRACE(fail_at);
    steps_.clear();
    sent_.clear();
    receives_ = 0;
    sends_ = 0;
    fail_send_at_ = fail_at;
    offer(1, 12);
    auto outcome = driver().run(4, 3);
    ASSERT_FALSE(outcome);
    EXPECT_EQ(outcome.error().kind(), DriveError::send_failure);
    EXPECT_EQ(sends_, fail_at + 1);
    EXPECT_EQ(receives_, 1U);
  }
}

TEST_F(HolepunchOfferDrive, RefusesInvalidArgumentsWithoutTalking) {
  EXPECT_EQ(driver().run(0, 3).error().kind(), DriveError::invalid_attempt);
  EXPECT_EQ(driver().run(4, 0).error().kind(), DriveError::invalid_own_req_id);
  EXPECT_EQ(driver().run(4, 65536).error().kind(), DriveError::invalid_own_req_id);
  EXPECT_EQ(receives_, 0U);
  EXPECT_EQ(sends_, 0U);
}

}  // namespace
}  // namespace play_test
