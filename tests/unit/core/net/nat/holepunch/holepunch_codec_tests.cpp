#include "src/core/net/nat/holepunch/holepunch_codec.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

namespace nat_test {
namespace {

using namespace libchiaking::net::nat::holepunch;

static_assert(!std::is_copy_constructible_v<SessionMessage>);
static_assert(std::is_move_constructible_v<SessionMessage>);

TEST(HolepunchShortMessage, CarriesTheActionRequestAndErrorWithAnEmptyRequest) {
  EXPECT_EQ(MessageCodec::format_short(Action::result, 7, 0),
            R"({"action":"RESULT","reqId":7,"error":0,"connRequest":{}})");
  EXPECT_EQ(MessageCodec::format_short(Action::offer, 65535, 42),
            R"({"action":"OFFER","reqId":65535,"error":42,"connRequest":{}})");
  EXPECT_EQ(
      MessageCodec::format_short(Action::accept, 65535, 65535),
      R"({"action":"ACCEPT","reqId":65535,"error":65535,"connRequest":{}})");
  EXPECT_EQ(MessageCodec::format_short(Action::terminate, 1, 2),
            R"({"action":"TERMINATE","reqId":1,"error":2,"connRequest":{}})");
  EXPECT_EQ(MessageCodec::format_short(Action::unknown, 0, 0),
            R"({"action":"UNKNOWN","reqId":0,"error":0,"connRequest":{}})");
}

class HolepunchFullMessage : public ::testing::Test {
 protected:
  void SetUp() override {
    request.sid = 1;
    request.peer_sid = 2;
  }

  ConnRequest request{};
};

TEST_F(HolepunchFullMessage, WritesAnOfferWithTheWholeRequest) {
  auto offer = MessageCodec::format_full(Action::offer, 7, 0, request, 42);
  ASSERT_TRUE(offer.has_value());
  EXPECT_EQ(
      offer.value().view(),
      R"({"action":"OFFER","reqId":7,"error":0,"connRequest":{"sid":1,"peerSid":2,"skey":"AAAAAAAAAAAAAAAAAAAAAA==","natType":0,"candidate":[],"defaultRouteMacAddr":"","localPeerAddr":{"accountId":"42","platform":"REMOTE_PLAY"},"localHashedId":""}})");
}

TEST_F(HolepunchFullMessage, WritesAnAcceptThatDecodesBack) {
  auto accept = MessageCodec::format_full(Action::accept, 9, 0, request, 42);
  ASSERT_TRUE(accept.has_value());
  EXPECT_TRUE(accept.value().view().starts_with(
      R"({"action":"ACCEPT","reqId":9,"error":0,"connRequest":)"));
  auto message = MessageCodec::decode_session(accept.value().view());
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ(message.value().action, Action::accept);
  auto& decoded = message.value().conn_request;
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value().sid, 1U);
  EXPECT_EQ(decoded.value().peer_sid, 2U);
  for (const auto byte : decoded.value().local_hashed_id)
    EXPECT_EQ(byte, std::byte{});
}

TEST_F(HolepunchFullMessage, EscapesCandidateTextAndDecodesItBack) {
  request.candidates.push_back({CandidateType::local, "a\"b\\c\n", "", 1, 0});
  auto escaped = MessageCodec::format_full(Action::offer, 10, 0, request, 42);
  ASSERT_TRUE(escaped.has_value());
  EXPECT_NE(
      escaped.value().view().find(
          R"("candidate":[{"type":"LOCAL","addr":"a\"b\\c\n","mappedAddr":"","port":1,"mappedPort":0}])"),
      std::string_view::npos);
  auto escaped_message = MessageCodec::decode_session(escaped.value().view());
  ASSERT_TRUE(escaped_message.has_value());
  auto& escaped_decoded = escaped_message.value().conn_request;
  ASSERT_TRUE(escaped_decoded.has_value());
  ASSERT_FALSE(escaped_decoded.value().candidates.empty());
  EXPECT_EQ(escaped_decoded.value().candidates[0].addr, "a\"b\\c\n");
}

TEST_F(HolepunchFullMessage, EscapesControlCharactersAndEncodesTheHashedId) {
  ConnRequest controlled{};
  controlled.sid = 3;
  controlled.peer_sid = 4;
  controlled.nat_type = 2;
  for (std::size_t index = 0; index < controlled.local_hashed_id.size();
       ++index)
    controlled.local_hashed_id[index] = static_cast<std::byte>(index + 1);
  controlled.candidates.push_back(
      {CandidateType::stun, "a\x01\x1f\x7f", "\b\f\r\t/", 65535, 1});
  auto control =
      MessageCodec::format_full(Action::accept, 65535, 7, controlled, -9);
  ASSERT_TRUE(control.has_value());
  EXPECT_EQ(
      control.value().view(),
      R"({"action":"ACCEPT","reqId":65535,"error":7,"connRequest":{"sid":3,"peerSid":4,)"
      R"("skey":"AAAAAAAAAAAAAAAAAAAAAA==","natType":2,"candidate":[{"type":"STUN",)"
      "\"addr\":\"a\\u0001\\u001F\x7f\",\"mappedAddr\":\"\\b\\f\\r\\t/\","
      R"("port":65535,"mappedPort":1}],"defaultRouteMacAddr":"",)"
      R"("localPeerAddr":{"accountId":"-9","platform":"REMOTE_PLAY"},)"
      R"("localHashedId":"AQIDBAUGBwgJCgsMDQ4PEBESExQ="}})");
}

TEST_F(HolepunchFullMessage, RefusesAnActionThatCarriesNoRequest) {
  for (const auto invalid_action :
       {Action::unknown, Action::terminate, Action::result}) {
    const auto invalid =
        MessageCodec::format_full(invalid_action, 1, 0, request, 42);
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error(), FullMessageError::invalid_action);
  }
}

TEST_F(HolepunchFullMessage, RefusesAnOverlongCandidateAddress) {
  request.candidates.push_back(
      {CandidateType::local, std::string(46U, 'a'), "", 0, 0});
  const auto invalid_candidate =
      MessageCodec::format_full(Action::offer, 1, 0, request, 42);
  ASSERT_FALSE(invalid_candidate.has_value());
  EXPECT_EQ(invalid_candidate.error(), FullMessageError::invalid_candidate);
}

TEST(HolepunchSessionMessage, LeavesAnEmptyRequestAbsent) {
  const auto empty = MessageCodec::decode_session(
      R"({"action":"RESULT","reqId":9,"error":0,"connRequest":{}})");
  ASSERT_TRUE(empty.has_value());
  EXPECT_EQ(empty.value().action, Action::result);
  EXPECT_EQ(empty.value().req_id, 9);
  EXPECT_EQ(empty.value().error, 0);
  EXPECT_FALSE(empty.value().conn_request.has_value());
}

TEST(HolepunchSessionMessage, ReadsTheRequestOfAnOffer) {
  const auto offer = MessageCodec::decode_session(
      R"({"action":"OFFER","reqId":7,"error":0,"connRequest":{"sid":1,"peerSid":2,"skey":"AAAAAAAAAAAAAAAAAAAAAA==","natType":2,"candidate":[],"defaultRouteMacAddr":"","localHashedId":"AAAAAAAAAAAAAAAAAAAAAAAAAAA="}})");
  ASSERT_TRUE(offer.has_value());
  EXPECT_EQ(offer.value().action, Action::offer);
  EXPECT_EQ(offer.value().req_id, 7);
  EXPECT_EQ(offer.value().error, 0);
  ASSERT_TRUE(offer.value().conn_request.has_value());
  EXPECT_EQ(offer.value().conn_request->sid, 1);
  EXPECT_EQ(offer.value().conn_request->peer_sid, 2);
  EXPECT_TRUE(offer.value().conn_request->candidates.empty());
}

TEST(HolepunchSessionMessage, RefusesARequestWithoutASessionKey) {
  const auto missing_skey = MessageCodec::decode_session(
      R"({"action":"OFFER","reqId":7,"error":0,"connRequest":{"sid":1,"peerSid":2,"natType":2,"candidate":[],"defaultRouteMacAddr":"","localHashedId":"AAAAAAAAAAAAAAAAAAAAAAAAAAA="}})");
  ASSERT_FALSE(missing_skey.has_value());
  EXPECT_EQ(missing_skey.error().kind, MessageErrorKind::invalid_schema);
}

TEST(HolepunchSessionMessage, RefusesANullRequest) {
  const auto wrong_conn_request = MessageCodec::decode_session(
      R"({"action":"OFFER","reqId":7,"error":0,"connRequest":null})");
  ASSERT_FALSE(wrong_conn_request.has_value());
  EXPECT_EQ(wrong_conn_request.error().kind, MessageErrorKind::invalid_schema);
}

TEST(HolepunchSessionMessage, ReportsMalformedJson) {
  const auto malformed = MessageCodec::decode_session("{");
  ASSERT_FALSE(malformed.has_value());
  EXPECT_EQ(malformed.error().kind, MessageErrorKind::json);
  EXPECT_EQ(malformed.error().json_error,
            libchiaking::json::ParseError::invalid_json);
}

TEST(HolepunchSessionMessage, KeepsTheLastOfADuplicatedMember) {
  const auto duplicate = MessageCodec::decode_session(
      R"({"action":"RESULT","reqId":1,"reqId":2,"error":0,"connRequest":{}})");
  ASSERT_TRUE(duplicate.has_value());
  EXPECT_EQ(duplicate.value().req_id, 2);
}

TEST(HolepunchSessionMessage, AcceptsLeadingZerosAndTrailingText) {
  const auto zeros = MessageCodec::decode_session(
      R"({"action":"RESULT","reqId":04,"error":0,"connRequest":{}} trailing)");
  ASSERT_TRUE(zeros.has_value());
  EXPECT_EQ(zeros.value().req_id, 4);
}

TEST(HolepunchSessionMessage, RefusesAnOversizedBody) {
  const auto oversized =
      MessageCodec::decode_session(std::string(17 * 1024, 'a'));
  ASSERT_FALSE(oversized.has_value());
  EXPECT_EQ(oversized.error().kind, MessageErrorKind::json);
  EXPECT_EQ(oversized.error().json_error,
            libchiaking::json::ParseError::invalid_json);
}

}  // namespace
}  // namespace nat_test
