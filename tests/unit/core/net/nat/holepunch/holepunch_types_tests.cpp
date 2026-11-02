#include "src/core/net/nat/holepunch/holepunch_codec.hpp"
#include "src/core/net/nat/holepunch/holepunch_types.hpp"
#include "src/core/utils/json/wire_dialect.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

namespace nat_test {
namespace {

using namespace libchiaking::net::nat::holepunch;

TEST(HolepunchAction, IsClassifiedFromItsExactUppercaseName) {
  EXPECT_EQ(MessageSchemaCodec::classify_action("OFFER"), Action::offer);
  EXPECT_EQ(MessageSchemaCodec::classify_action("ACCEPT"), Action::accept);
  EXPECT_EQ(MessageSchemaCodec::classify_action("TERMINATE"),
            Action::terminate);
  EXPECT_EQ(MessageSchemaCodec::classify_action("RESULT"), Action::result);
}

TEST(HolepunchAction, IsUnknownForAnyOtherSpelling) {
  EXPECT_EQ(MessageSchemaCodec::classify_action("offer"), Action::unknown);
  EXPECT_EQ(MessageSchemaCodec::classify_action("RESULT "), Action::unknown);
  EXPECT_EQ(MessageSchemaCodec::classify_action(""), Action::unknown);
  EXPECT_EQ(MessageSchemaCodec::classify_action(std::string_view("OFFER\0", 6)),
            Action::unknown);
}

class HolepunchMessageSchema : public ::testing::Test {
 protected:
  static auto decode(std::string_view text) -> std::optional<MessageSchema> {
    auto parsed = libchiaking::json::WireDialect::read<MessageWire>(text);
    if (!parsed) return std::nullopt;
    return MessageSchemaCodec::decode_schema(parsed.value());
  }

  static auto refuses(std::string_view text) -> ::testing::AssertionResult {
    if (decode(text).has_value())
      return ::testing::AssertionFailure() << "accepted " << text;
    return ::testing::AssertionSuccess();
  }
};

TEST_F(HolepunchMessageSchema, ReadsEveryActionWithItsHeaderFields) {
  for (const auto* name : {"OFFER", "ACCEPT", "TERMINATE", "RESULT", "PING"}) {
    SCOPED_TRACE(name);
    const auto result =
        decode(std::string{"{\"action\":\""} + name +
               "\",\"reqId\":1,\"error\":2,\"connRequest\":{}}");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().action, MessageSchemaCodec::classify_action(name));
    EXPECT_EQ(result.value().req_id, 1);
    EXPECT_EQ(result.value().error, 2);
    EXPECT_FALSE(result.value().has_conn_request_payload);
  }
}

TEST_F(HolepunchMessageSchema, ReadsReorderedMembersAndIgnoresExtras) {
  const auto reordered = decode(
      R"({"connRequest":{"sid":1},"error":65535,"reqId":0,"action":"ACCEPT","extra":true})");
  ASSERT_TRUE(reordered.has_value());
  EXPECT_TRUE(reordered.value().has_conn_request_payload);
  EXPECT_EQ(reordered.value().req_id, 0);
  EXPECT_EQ(reordered.value().error, 65535);
  EXPECT_EQ(reordered.value().action, Action::accept);
}

TEST_F(HolepunchMessageSchema, RefusesAMissingMember) {
  EXPECT_TRUE(refuses(R"([])"));
  EXPECT_TRUE(refuses(R"({"reqId":1,"error":0,"connRequest":{}})"));
  EXPECT_TRUE(refuses(R"({"action":"OFFER","error":0,"connRequest":{}})"));
  EXPECT_TRUE(refuses(R"({"action":"OFFER","reqId":1,"connRequest":{}})"));
  EXPECT_TRUE(refuses(R"({"action":"OFFER","reqId":1,"error":0})"));
}

TEST_F(HolepunchMessageSchema, RefusesAMemberOfTheWrongKind) {
  for (const auto* bad : {"null", "true", "[]"}) {
    EXPECT_TRUE(refuses(std::string{"{\"action\":"} + bad +
                        ",\"reqId\":1,\"error\":0,\"connRequest\":{}}"));
    EXPECT_TRUE(refuses(std::string{"{\"action\":\"OFFER\",\"reqId\":"} + bad +
                        ",\"error\":0,\"connRequest\":{}}"));
    EXPECT_TRUE(
        refuses(std::string{"{\"action\":\"OFFER\",\"reqId\":1,\"error\":"} +
                bad + ",\"connRequest\":{}}"));
    EXPECT_TRUE(
        refuses(std::string{"{\"action\":\"OFFER\",\"reqId\":1,\"error\":0,"
                            "\"connRequest\":"} +
                bad + "}"));
  }
  EXPECT_TRUE(refuses(R"({"action":1,"reqId":1,"error":0,"connRequest":{}})"));
  EXPECT_TRUE(
      refuses(R"({"action":"OFFER","reqId":1,"error":0,"connRequest":1})"));
}

TEST_F(HolepunchMessageSchema, TruncatesOutOfRangeNumbersToSixteenBits) {
  const auto truncated =
      decode(R"({"action":"OFFER","reqId":65537,"error":-1,"connRequest":{}})");
  ASSERT_TRUE(truncated.has_value());
  EXPECT_EQ(truncated.value().req_id, 1);
  EXPECT_EQ(truncated.value().error, 65535);
}

TEST_F(HolepunchMessageSchema, ClampsNumbersBeyondSixtyFourBits) {
  const auto clamped = decode(
      R"({"action":"OFFER","reqId":99999999999,"error":-99999999999,"connRequest":{}})");
  ASSERT_TRUE(clamped.has_value());
  EXPECT_EQ(clamped.value().req_id, 65535);
  EXPECT_EQ(clamped.value().error, 0);
}

TEST_F(HolepunchMessageSchema, RefusesNumbersThatAreNotPlainIntegers) {
  for (const auto* bad : {"\"1\"", "1.0", "1e0"}) {
    EXPECT_TRUE(refuses(std::string{"{\"action\":\"OFFER\",\"reqId\":"} + bad +
                        ",\"error\":0,\"connRequest\":{}}"));
    EXPECT_TRUE(
        refuses(std::string{"{\"action\":\"OFFER\",\"reqId\":1,\"error\":"} +
                bad + ",\"connRequest\":{}}"));
  }
}

TEST(HolepunchSessionDecoding, ReadsAWellFormedMessage) {
  const auto ok = MessageCodec::decode_session(
      R"({"action":"OFFER","reqId":3,"error":4,"connRequest":{}})");
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(ok.value().action, Action::offer);
  EXPECT_EQ(ok.value().req_id, 3);
  EXPECT_EQ(ok.value().error, 4);
}

TEST(HolepunchSessionDecoding, ReportsMalformedJson) {
  const auto malformed = MessageCodec::decode_session("{");
  ASSERT_FALSE(malformed.has_value());
  EXPECT_EQ(malformed.error().kind, MessageErrorKind::json);
  EXPECT_EQ(malformed.error().json_error,
            libchiaking::json::ParseError::invalid_json);
}

TEST(HolepunchSessionDecoding, ReadsAMessageWithALargeExtraMember) {
  const auto large = MessageCodec::decode_session(
      std::string(R"({"pad":")") + std::string(17 * 1024, 'a') +
      R"(","action":"OFFER","reqId":1,"error":0,"connRequest":{}})");
  ASSERT_TRUE(large.has_value());
  EXPECT_EQ(large.value().req_id, 1);
}

TEST(HolepunchSessionDecoding, RefusesNestingThatIsTooDeep) {
  const std::string deep =
      std::string(R"({"extra":)") + std::string(70, '[') +
      std::string(70, ']') +
      R"(,"action":"OFFER","reqId":1,"error":0,"connRequest":{}})";
  const auto too_deep = MessageCodec::decode_session(deep);
  ASSERT_FALSE(too_deep.has_value());
  EXPECT_EQ(too_deep.error().kind, MessageErrorKind::json);
  EXPECT_EQ(too_deep.error().json_error,
            libchiaking::json::ParseError::too_deep);
}

TEST(HolepunchSessionDecoding, KeepsTheLastOfADuplicatedMember) {
  const auto dup = MessageCodec::decode_session(
      R"({"action":"OFFER","reqId":1,"reqId":2,"error":0,"connRequest":{}})");
  ASSERT_TRUE(dup.has_value());
  EXPECT_EQ(dup.value().req_id, 2);
}

TEST(HolepunchSessionDecoding, AcceptsLeadingZerosAndTrailingText) {
  const auto zeros = MessageCodec::decode_session(
      R"({"action":"OFFER","reqId":04,"error":00,"connRequest":{}})"
      "\r\n<html>");
  ASSERT_TRUE(zeros.has_value());
  EXPECT_EQ(zeros.value().req_id, 4);
  EXPECT_EQ(zeros.value().error, 0);
}

TEST(HolepunchSessionDecoding, ReportsAnInvalidSchema) {
  const auto wrong = MessageCodec::decode_session(
      R"({"action":123,"reqId":1,"error":0,"connRequest":{}})");
  ASSERT_FALSE(wrong.has_value());
  EXPECT_EQ(wrong.error().kind, MessageErrorKind::invalid_schema);
}

}  // namespace
}  // namespace nat_test
