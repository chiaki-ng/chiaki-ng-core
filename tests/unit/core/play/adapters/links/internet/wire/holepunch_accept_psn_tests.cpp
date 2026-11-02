#include "src/core/net/nat/holepunch/holepunch_codec.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_accept.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/holepunch_accept_rig.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/holepunch_psn_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string_view>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace internet = libchiaking::play::internet;
using holepunch::PsnRig;
using holepunch::SelectedAccept;
using PsnErrorKind = internet::holepunch::AcceptPsnErrorKind;

class HolepunchAcceptPsn : public ::testing::Test {
 protected:
  static constexpr std::string_view terminate_json =
      R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"TERMINATE\",\"reqId\":1,\"error\":0,\"connRequest\":{}}"}}}})";

  auto run(std::uint64_t attempt = 7,
           libchiaking::platform::StopToken stop = {}) {
    return internet::holepunch::Accept::run_psn(
        attempt, psn_.session, psn_.push, psn_.connection, PsnRig::token,
        PsnRig::session_id, PsnRig::account_id, PsnRig::uid, PsnRig::family,
        stop, psn_.clock, PsnRig::deadline);
  }

  PsnRig psn_;
};

TEST_F(HolepunchAcceptPsn, SkipsUnknownPushesAndAcknowledgesTheAccept) {
  psn_.connection.push(
      R"({"dataType":"psn:sessionManager:sys:unknown:thing"})");
  psn_.connection.push(SelectedAccept::accept_json);
  auto result = run();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().state(),
            internet::holepunch::AcceptWaitState::ready);
  EXPECT_EQ(psn_.connection.calls(), 2U);
  EXPECT_EQ(psn_.websocket.connects.load(), 0);
  ASSERT_EQ(psn_.http.bodies().size(), 1U);
  EXPECT_EQ(psn_.http.bodies()[0],
            PsnRig::envelope(
                nat::MessageCodec::format_short(nat::Action::result, 9, 0)));
}

TEST_F(HolepunchAcceptPsn, TimesOutAtItsDeadlineWithoutListening) {
  psn_.clock.now_us = PsnRig::deadline;
  auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), PsnErrorKind::timed_out);
  EXPECT_EQ(psn_.connection.calls(), 0U);
  EXPECT_TRUE(psn_.http.bodies().empty());
}

TEST_F(HolepunchAcceptPsn, StopsWhenCancelledBeforeListening) {
  auto state = std::make_shared<test_support::StopState>();
  state->stopped = true;
  auto result = run(7, libchiaking::platform::StopToken{state});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), PsnErrorKind::cancelled);
  EXPECT_EQ(psn_.connection.calls(), 0U);
  EXPECT_TRUE(psn_.http.bodies().empty());
}

TEST_F(HolepunchAcceptPsn, RejectsAMalformedSessionMessage) {
  psn_.connection.push(
      R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"body=not-json"}}}})");
  auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), PsnErrorKind::invalid_message);
  EXPECT_EQ(psn_.connection.calls(), 1U);
  EXPECT_TRUE(psn_.http.bodies().empty());
}

TEST_F(HolepunchAcceptPsn, ReportsAnAcknowledgementThatCannotBeSent) {
  psn_.http.fail_every_post();
  psn_.connection.push(SelectedAccept::accept_json);
  auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), PsnErrorKind::send_failure);
  EXPECT_EQ(psn_.http.bodies().size(), 1U);
  EXPECT_EQ(psn_.connection.calls(), 1U);
}

TEST_F(HolepunchAcceptPsn, TimesOutWhenTheDeadlinePassesDuringTheSend) {
  psn_.http.expire_and_cancel();
  psn_.connection.push(SelectedAccept::accept_json);
  auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), PsnErrorKind::timed_out);
  EXPECT_EQ(psn_.http.bodies().size(), 1U);
}

TEST_F(HolepunchAcceptPsn, StopsOnATerminate) {
  psn_.connection.push(terminate_json);
  auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), PsnErrorKind::terminated);
  EXPECT_TRUE(psn_.http.bodies().empty());
}

TEST_F(HolepunchAcceptPsn, ReportsAPushFailure) {
  psn_.connection.fail();
  auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), PsnErrorKind::push_failure);
  EXPECT_EQ(psn_.connection.calls(), 1U);
  EXPECT_TRUE(psn_.http.bodies().empty());
}

TEST_F(HolepunchAcceptPsn, RefusesAZeroAttemptWithoutListening) {
  auto result = run(0);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), PsnErrorKind::invalid_attempt);
  EXPECT_EQ(psn_.connection.calls(), 0U);
  EXPECT_TRUE(psn_.http.bodies().empty());
}

}  // namespace
}  // namespace play_test
