#include "libchiaking/detail/diagnostics/log_queue.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_accept.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/live_providers.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace internet = libchiaking::play::internet;
using DriveError = internet::holepunch::AcceptPsnErrorKind;
using PsnError = internet::holepunch::AcceptPsnError;

class HolepunchAcceptDrive : public ::testing::Test {
 protected:
  void script(nat::Action action, std::uint16_t req_id = 0) {
    nat::SessionMessage out{};
    out.action = action;
    out.req_id = req_id;
    messages_.push_back(std::move(out));
  }

  auto driver(const libchiaking::diagnostics::Trace* trace = nullptr)
      -> internet::holepunch::AcceptDrive {
    return internet::holepunch::AcceptDrive{
        [this]() -> libchiaking::Expected<nat::SessionMessage, PsnError> {
          if (reads_ == messages_.size())
            return libchiaking::unexpected(PsnError{exhausted_});
          return std::move(messages_[reads_++]);
        },
        [this](const internet::holepunch::AckPeerAccept& ack)
            -> libchiaking::Expected<void, PsnError> {
          acks_.push_back(ack);
          if (fail_send_)
            return libchiaking::unexpected(PsnError{DriveError::send_failure});
          return {};
        },
        trace};
  }

  std::vector<nat::SessionMessage> messages_;
  DriveError exhausted_{DriveError::timed_out};
  std::size_t reads_{};
  std::vector<internet::holepunch::AckPeerAccept> acks_;
  bool fail_send_{};
};

TEST_F(HolepunchAcceptDrive, AcknowledgesTheFirstAcceptAndTracesEachState) {
  script(nat::Action::accept, 7);
  script(nat::Action::accept, 8);
  libchiaking::diagnostics::LogQueue logs{test_support::LiveProviders::steady()};
  test_support::FakeClock clock{0};
  const libchiaking::diagnostics::Trace trace{
      &logs, clock, libchiaking::diagnostics::Component::control_route};
  auto ready = driver(&trace).run(9);
  ASSERT_TRUE(ready);
  EXPECT_EQ(ready.value().state(), internet::holepunch::AcceptWaitState::ready);
  ASSERT_EQ(acks_.size(), 1U);
  EXPECT_EQ(acks_[0].attempt, 9U);
  EXPECT_EQ(acks_[0].peer_req_id, 7);
  EXPECT_EQ(reads_, 1U);
  for (const std::string_view state :
       {"state=waiting_accept", "state=ack_pending", "state=ready"}) {
    SCOPED_TRACE(state);
    const auto entry = logs.read();
    ASSERT_TRUE(entry);
    EXPECT_NE(entry.value().message().find(state), std::string_view::npos);
  }
  EXPECT_FALSE(logs.read());
}

TEST_F(HolepunchAcceptDrive, SkipsOtherActionsUntilAnAccept) {
  script(nat::Action::offer, 3);
  script(nat::Action::result, 4);
  script(nat::Action::accept, 0);
  script(nat::Action::accept, 5);
  auto ready = driver().run(9);
  EXPECT_TRUE(ready);
  ASSERT_EQ(acks_.size(), 1U);
  EXPECT_EQ(acks_[0].peer_req_id, 0);
  EXPECT_EQ(reads_, 3U);
}

TEST_F(HolepunchAcceptDrive, StopsOnATerminate) {
  script(nat::Action::terminate);
  auto result = driver().run(9);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), DriveError::terminated);
  EXPECT_TRUE(acks_.empty());
}

TEST_F(HolepunchAcceptDrive, PassesOnAReceiveFailure) {
  for (const auto error : {DriveError::cancelled, DriveError::timed_out}) {
    SCOPED_TRACE(static_cast<int>(error));
    messages_.clear();
    reads_ = 0;
    acks_.clear();
    exhausted_ = error;
    auto result = driver().run(9);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind(), error);
    EXPECT_TRUE(acks_.empty());
  }
}

TEST_F(HolepunchAcceptDrive, FailsWhenTheAcknowledgementCannotBeSent) {
  script(nat::Action::accept, 6);
  fail_send_ = true;
  auto result = driver().run(9);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), DriveError::send_failure);
  EXPECT_EQ(acks_.size(), 1U);
}

TEST_F(HolepunchAcceptDrive, RefusesAZeroAttemptWithoutReading) {
  auto result = driver().run(0);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), DriveError::invalid_attempt);
  EXPECT_EQ(reads_, 0U);
  EXPECT_TRUE(acks_.empty());
}

}  // namespace
}  // namespace play_test
