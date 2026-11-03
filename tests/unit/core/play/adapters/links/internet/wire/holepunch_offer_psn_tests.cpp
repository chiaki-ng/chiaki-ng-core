#include "src/core/net/nat/holepunch/holepunch_codec.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_offer.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/holepunch_psn_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace internet = libchiaking::play::internet;
using holepunch::PsnRig;
using internet::holepunch::OfferPsnErrorKind;

class HolepunchOfferPsn : public ::testing::Test {
 protected:
  static constexpr std::string_view offer_json =
      R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"OFFER\",\"reqId\":2,\"error\":0,\"connRequest\":{\"sid\":3,\"peerSid\":4,\"skey\":\"AAAAAAAAAAAAAAAAAAAAAA==\",\"natType\":2,\"candidate\":[],\"defaultRouteMacAddr\":\"\",\"localPeerAddr\":,\"localHashedId\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAA=\"}}"}}}})";
  static constexpr std::string_view result_json =
      R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"RESULT\",\"reqId\":1,\"error\":0,\"connRequest\":{}}"}}}})";
  static constexpr std::string_view terminate_json =
      R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"TERMINATE\",\"reqId\":1,\"error\":0,\"connRequest\":{}}"}}}})";

  void SetUp() override {
    local_.sid = 1;
    local_.peer_sid = 2;
    psn_.http.answer_checks_with(200);
  }

  auto run(std::uint64_t attempt = 7, std::uint64_t own_req_id = 1) {
    return internet::holepunch::Offer::run_psn(
        attempt, own_req_id, psn_.session, psn_.push, psn_.connection,
        PsnRig::token, PsnRig::session_id, PsnRig::account_id, PsnRig::uid,
        PsnRig::family, local_, {}, psn_.clock, PsnRig::deadline);
  }

  static auto ack(std::uint16_t req_id) -> std::string {
    return PsnRig::envelope(
        nat::MessageCodec::format_short(nat::Action::result, req_id, 0));
  }

  auto own_offer() -> std::string {
    auto full =
        nat::MessageCodec::format_full(nat::Action::offer, 1, 0, local_, 12);
    EXPECT_TRUE(full);
    if (!full) return {};
    return PsnRig::envelope(full.value().view());
  }

  static auto replaced(std::string_view json, std::string_view from,
                       std::string_view to) -> std::string {
    std::string out{json};
    const auto at = out.find(from);
    EXPECT_NE(at, std::string::npos);
    if (at != std::string::npos) out.replace(at, from.size(), to);
    return out;
  }

  PsnRig psn_;
  nat::ConnRequest local_{};
};

TEST_F(HolepunchOfferPsn, ProceedsAfterAFailedSessionCheck) {
  psn_.http.answer_checks_with(503);
  psn_.connection.push(offer_json);
  psn_.connection.push(result_json);
  auto result = run();
  EXPECT_EQ(psn_.http.checks(), 1U);
  EXPECT_EQ(psn_.http.bodies().size(), 2U);
  EXPECT_TRUE(result);
}

TEST_F(HolepunchOfferPsn, TimesOutWhenTheSessionCheckOutlivesTheDeadline) {
  psn_.http.answer_checks_with(503);
  psn_.http.expire_on_check();
  psn_.connection.push(offer_json);
  psn_.connection.push(result_json);
  auto result = run();
  EXPECT_EQ(psn_.http.checks(), 1U);
  EXPECT_EQ(psn_.http.bodies().size(), 2U);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), OfferPsnErrorKind::timed_out);
}

TEST_F(HolepunchOfferPsn, AcknowledgesEveryRepeatedOfferAndSendsOursOnce) {
  psn_.connection.push(offer_json);
  psn_.connection.push(offer_json);
  psn_.connection.push(offer_json);
  psn_.connection.push(result_json);
  auto result = run();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().sid, 3U);
  ASSERT_EQ(psn_.http.bodies().size(), 4U);
  EXPECT_EQ(psn_.http.checks(), 1U);
  EXPECT_EQ(psn_.http.bodies()[0], ack(2));
  EXPECT_EQ(psn_.http.bodies()[2], ack(2));
  EXPECT_EQ(psn_.http.bodies()[3], ack(2));
  EXPECT_EQ(psn_.http.bodies()[1], own_offer());
}

TEST_F(HolepunchOfferPsn, SkipsUnknownPushesWithoutTouchingOurOffer) {
  psn_.connection.push(
      R"({"dataType":"psn:sessionManager:sys:unknown:thing"})");
  psn_.connection.push(offer_json);
  psn_.connection.push(result_json);
  auto result = run();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().sid, 3U);
  EXPECT_EQ(psn_.connection.calls(), 3U);
  EXPECT_EQ(psn_.websocket.connects.load(), 0);
  ASSERT_EQ(psn_.http.bodies().size(), 2U);
  EXPECT_EQ(local_.peer_sid, 2U);
  EXPECT_EQ(psn_.http.bodies()[0], ack(2));
  EXPECT_EQ(psn_.http.bodies()[1], own_offer());
}

TEST_F(HolepunchOfferPsn, WrapsAPeerSessionIdBeyondSixteenBits) {
  psn_.connection.push(
      replaced(offer_json, "\\\"sid\\\":3", "\\\"sid\\\":70000"));
  psn_.connection.push(result_json);
  auto result = run();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().sid, 70000U - 65536U);
  EXPECT_EQ(local_.peer_sid, 2U);
}

TEST_F(HolepunchOfferPsn, TimesOutAtItsDeadlineWithoutListening) {
  psn_.clock.now_us = PsnRig::deadline;
  auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), OfferPsnErrorKind::timed_out);
  EXPECT_EQ(psn_.connection.calls(), 0U);
  EXPECT_TRUE(psn_.http.bodies().empty());
}

TEST_F(HolepunchOfferPsn, RejectsAMalformedSessionMessage) {
  psn_.connection.push(
      R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"body=not-json"}}}})");
  auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), OfferPsnErrorKind::invalid_message);
  EXPECT_EQ(psn_.connection.calls(), 1U);
  EXPECT_TRUE(psn_.http.bodies().empty());
}

TEST_F(HolepunchOfferPsn, ReportsAnAcknowledgementThatCannotBeSent) {
  psn_.http.fail_every_post();
  psn_.connection.push(offer_json);
  auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), OfferPsnErrorKind::send_failure);
  EXPECT_EQ(psn_.http.bodies().size(), 1U);
  EXPECT_EQ(psn_.connection.calls(), 1U);
}

TEST_F(HolepunchOfferPsn, AcknowledgesAnOfferWithoutARequestId) {
  psn_.connection.push(
      replaced(offer_json, "\\\"reqId\\\":2", "\\\"reqId\\\":0"));
  psn_.connection.push(result_json);
  auto result = run();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().sid, 3U);
  ASSERT_EQ(psn_.http.bodies().size(), 2U);
  EXPECT_EQ(psn_.http.bodies()[0], ack(0));
}

TEST_F(HolepunchOfferPsn, ToleratesAFailedPostAfterTheFirstAcknowledgement) {
  psn_.http.fail_post(1);
  psn_.connection.push(offer_json);
  psn_.connection.push(offer_json);
  psn_.connection.push(result_json);
  auto result = run();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().sid, 3U);
  EXPECT_EQ(psn_.http.bodies().size(), 3U);
}

TEST_F(HolepunchOfferPsn, FailsWhenPsnRejectsOurOffer) {
  psn_.http.reject_post(1);
  psn_.connection.push(offer_json);
  psn_.connection.push(offer_json);
  psn_.connection.push(result_json);
  auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), OfferPsnErrorKind::send_failure);
  EXPECT_EQ(result.error().send()->session()->status, 400);
  EXPECT_EQ(psn_.http.bodies().size(), 2U);
  EXPECT_EQ(psn_.connection.calls(), 1U);
}

TEST_F(HolepunchOfferPsn, StopsOnATerminate) {
  psn_.connection.push(terminate_json);
  auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), OfferPsnErrorKind::terminated);
  EXPECT_TRUE(psn_.http.bodies().empty());
}

TEST_F(HolepunchOfferPsn, ReportsAPushFailure) {
  psn_.connection.fail();
  auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), OfferPsnErrorKind::push_failure);
  EXPECT_EQ(psn_.connection.calls(), 1U);
  EXPECT_TRUE(psn_.http.bodies().empty());
}

TEST_F(HolepunchOfferPsn, TimesOutWhenTheDeadlinePassesDuringTheSend) {
  psn_.http.expire_and_cancel();
  psn_.connection.push(offer_json);
  auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), OfferPsnErrorKind::timed_out);
  EXPECT_EQ(psn_.http.bodies().size(), 1U);
}

TEST_F(HolepunchOfferPsn, RefusesInvalidArgumentsWithoutListening) {
  auto result = run(0, 1);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), OfferPsnErrorKind::invalid_attempt);
  result = run(7, 0);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), OfferPsnErrorKind::invalid_own_req_id);
  EXPECT_EQ(psn_.connection.calls(), 0U);
  EXPECT_TRUE(psn_.http.bodies().empty());
}

}  // namespace
}  // namespace play_test
