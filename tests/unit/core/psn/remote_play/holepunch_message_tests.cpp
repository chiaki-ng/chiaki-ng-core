#include "src/core/net/nat/holepunch/holepunch_codec.hpp"
#include "src/core/psn/remote_play/holepunch_message.hpp"
#include "src/core/psn/remote_play/push_client.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/psn/notifications.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <variant>

namespace psn_test {
namespace {

using libchiaking::net::nat::holepunch::Action;
using libchiaking::net::nat::holepunch::MessageError;
using libchiaking::net::nat::holepunch::MessageErrorKind;
using libchiaking::psn::remote_play::HolepunchMessage;
using libchiaking::psn::rest::Notification;
using libchiaking::psn::rest::NotificationType;
using BodyError = HolepunchMessage::BodyError;
using PayloadError = HolepunchMessage::PayloadError;
using PayloadErrorKind = HolepunchMessage::PayloadErrorKind;

class HolepunchMessageFixture : public ::testing::Test {
 protected:
  static auto message(
      std::string_view json,
      NotificationType type = NotificationType::session_message_created)
      -> Notification {
    return test_support::psn::Notifications::typed(type, json);
  }

  static auto has_body(std::string_view payload, std::string_view expected)
      -> testing::AssertionResult {
    const auto body = HolepunchMessage::body_of(payload);
    if (!body)
      return testing::AssertionFailure()
             << payload
             << " has no body: " << testing::PrintToString(body.error());
    if (body.value().view() != expected)
      return testing::AssertionFailure()
             << payload << " has body " << body.value().view();
    return testing::AssertionSuccess();
  }

  static auto has_no_body(std::string_view payload, BodyError reason)
      -> testing::AssertionResult {
    const auto body = HolepunchMessage::body_of(payload);
    if (body)
      return testing::AssertionFailure()
             << "found body " << body.value().view();
    if (body.error() != reason)
      return testing::AssertionFailure()
             << "refused as " << testing::PrintToString(body.error());
    return testing::AssertionSuccess();
  }

  static auto has_payload(std::string_view json, std::string_view expected)
      -> testing::AssertionResult {
    auto input = message(json);
    const auto payload = HolepunchMessage::payload_of(input);
    if (!payload)
      return testing::AssertionFailure()
             << json << " has no payload: "
             << testing::PrintToString(payload.error().kind);
    if (payload.value().view() != expected)
      return testing::AssertionFailure()
             << json << " has payload " << payload.value().view();
    return testing::AssertionSuccess();
  }

  static auto lacks_payload(std::string_view json) -> testing::AssertionResult {
    auto input = message(json);
    const auto payload = HolepunchMessage::payload_of(input);
    if (payload)
      return testing::AssertionFailure()
             << json << " has payload " << payload.value().view();
    if (payload.error().kind != PayloadErrorKind::invalid_schema)
      return testing::AssertionFailure()
             << json << " was refused as "
             << testing::PrintToString(payload.error().kind);
    return testing::AssertionSuccess();
  }

  static constexpr std::string_view result_json =
      R"({"body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"RESULT\",\"reqId\":1,\"error\":0,\"connRequest\":{}}"}}}})";
};

class HolepunchMessageBody : public HolepunchMessageFixture {};

TEST_F(HolepunchMessageBody, FollowsTheFirstBodyMarker) {
  EXPECT_TRUE(has_body("ver=1.0, type=text, body={\"k\":1}", "{\"k\":1}"));
  EXPECT_TRUE(has_body("body=abc", "abc"));
  EXPECT_TRUE(has_body("xx body= y z", " y z"));
  EXPECT_TRUE(has_body("body=Abody=B", "Abody=B"));
}

TEST_F(HolepunchMessageBody, IsMissingWithoutTheMarkerOrText) {
  EXPECT_TRUE(has_no_body("ver=1.0, type=text", BodyError::missing_body));
  EXPECT_TRUE(has_no_body("prefix body=", BodyError::missing_body));
}

TEST_F(HolepunchMessageBody, RejectsANulAnywhere) {
  EXPECT_TRUE(has_no_body(std::string_view{"body=a\0b", 8},
                          BodyError::invalid_payload));
  EXPECT_TRUE(has_no_body(std::string_view{"x\0body=a", 8},
                          BodyError::invalid_payload));
}

class HolepunchMessagePayload : public HolepunchMessageFixture {};

TEST_F(HolepunchMessagePayload, IsReadFromTheSessionMessage) {
  EXPECT_TRUE(has_payload(
      R"({"body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={}"}}}})",
      "ver=1.0, type=text, body={}"));
}

TEST_F(HolepunchMessagePayload, RequiresASessionMessageNotification) {
  auto wrong_type = message(
      R"({"body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={}"}}}})",
      NotificationType::members_created);
  const auto wrong_result = HolepunchMessage::payload_of(wrong_type);
  ASSERT_FALSE(wrong_result.has_value());
  EXPECT_EQ(wrong_result.error().kind, PayloadErrorKind::invalid_type);
}

TEST_F(HolepunchMessagePayload, RequiresTheFullPath) {
  EXPECT_TRUE(lacks_payload(R"({"data":{"sessionMessage":{"payload":"x"}}})"));
  EXPECT_TRUE(lacks_payload(R"({"body":{"sessionMessage":{"payload":"x"}}})"));
  EXPECT_TRUE(lacks_payload(R"({"body":{"data":{"payload":"x"}}})"));
  EXPECT_TRUE(lacks_payload(R"({"body":{"data":{"sessionMessage":{}}}})"));
  EXPECT_TRUE(
      lacks_payload(R"({"body":{"data":{"sessionMessage":{"payload":42}}}})"));
  EXPECT_TRUE(lacks_payload(R"({"body":null})"));
}

TEST_F(HolepunchMessagePayload, IsReadFromTheLastDuplicateBody) {
  EXPECT_TRUE(has_payload(
      R"({"body":{},"body":{"data":{"sessionMessage":{"payload":"last"}}}})",
      "last"));
}

TEST_F(HolepunchMessagePayload, IsReadFromWireDialectText) {
  EXPECT_TRUE(has_payload(
      "{\"dataType\":\"psn:sessionManager:sys:rps:sessionMessage:created\","
      "\"seq\":01,\"body\":{\"data\":{\"sessionMessage\":{\"payload\":\"ver=1."
      "0\\uDC00\xC3\x28\"}}}}\r\n<html>",
      "ver=1.0\xEF\xBF\xBD\xC3\x28"));
}

TEST_F(HolepunchMessagePayload, IsReadFromALargeNotification) {
  std::string large_body = R"({"pad":")";
  large_body.append(20 * 1024, 'a');
  large_body += R"(","body":{"data":{"sessionMessage":{"payload":"big"}}}})";
  EXPECT_TRUE(has_payload(large_body, "big"));
}

class HolepunchMessageDecode : public HolepunchMessageFixture {};

TEST_F(HolepunchMessageDecode, ReadsAResultWithoutAConnRequest) {
  auto input = message(result_json);
  const auto result = HolepunchMessage::decode(input);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().action, Action::result);
  EXPECT_EQ(result.value().req_id, 1);
  EXPECT_EQ(result.value().error, 0);
  EXPECT_FALSE(result.value().conn_request.has_value());
}

TEST_F(HolepunchMessageDecode, ReadsAnOfferWithItsConnRequest) {
  auto input = message(
      R"({"body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"OFFER\",\"reqId\":2,\"error\":0,\"connRequest\":{\"sid\":3,\"peerSid\":4,\"skey\":\"AAAAAAAAAAAAAAAAAAAAAA==\",\"natType\":2,\"candidate\":[],\"defaultRouteMacAddr\":\"\",\"localPeerAddr\":,\"localHashedId\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAA=\"}}"}}}})");
  const auto offer = HolepunchMessage::decode(input);
  ASSERT_TRUE(offer.has_value());
  EXPECT_EQ(offer.value().action, Action::offer);
  EXPECT_EQ(offer.value().req_id, 2);
  ASSERT_TRUE(offer.value().conn_request.has_value());
  EXPECT_EQ(offer.value().conn_request->sid, 3);
  EXPECT_EQ(offer.value().conn_request->peer_sid, 4);
}

TEST_F(HolepunchMessageDecode, RefusesAnotherNotificationType) {
  auto input = message(result_json, NotificationType::members_created);
  const auto wrong = HolepunchMessage::decode(input);
  ASSERT_FALSE(wrong.has_value());
  ASSERT_TRUE(std::holds_alternative<PayloadError>(wrong.error()));
  EXPECT_EQ(std::get<PayloadError>(wrong.error()).kind,
            PayloadErrorKind::invalid_type);
}

TEST_F(HolepunchMessageDecode, RefusesAPayloadWithoutABody) {
  auto input =
      message(R"({"body":{"data":{"sessionMessage":{"payload":"ver=1.0"}}}})");
  const auto missing = HolepunchMessage::decode(input);
  ASSERT_FALSE(missing.has_value());
  ASSERT_TRUE(std::holds_alternative<BodyError>(missing.error()));
  EXPECT_EQ(std::get<BodyError>(missing.error()), BodyError::missing_body);
}

TEST_F(HolepunchMessageDecode, RefusesABodyThatIsNotJson) {
  auto input = message(
      R"({"body":{"data":{"sessionMessage":{"payload":"body=not-json"}}}})");
  const auto invalid = HolepunchMessage::decode(input);
  ASSERT_FALSE(invalid.has_value());
  ASSERT_TRUE(std::holds_alternative<MessageError>(invalid.error()));
  EXPECT_EQ(std::get<MessageError>(invalid.error()).kind,
            MessageErrorKind::json);
}

}  // namespace
}  // namespace psn_test
