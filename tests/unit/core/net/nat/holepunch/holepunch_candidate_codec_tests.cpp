#include "src/core/net/nat/holepunch/holepunch_candidate.hpp"
#include "src/core/net/nat/holepunch/holepunch_conn_request.hpp"
#include "src/core/utils/json/wire_dialect.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nat_test {
namespace {

using namespace libchiaking::net::nat::holepunch;

TEST(HolepunchCandidateType, IsClassifiedFromItsExactUppercaseName) {
  EXPECT_EQ(CandidateCodec::classify_type("LOCAL"), CandidateType::local);
  EXPECT_EQ(CandidateCodec::classify_type("STUN"), CandidateType::stun);
  EXPECT_EQ(CandidateCodec::classify_type("DERIVED"), CandidateType::derived);
  EXPECT_EQ(CandidateCodec::classify_type("STATIC"), CandidateType::static_);
}

TEST(HolepunchCandidateType, IsStaticForAnyOtherSpelling) {
  EXPECT_EQ(CandidateCodec::classify_type("local"), CandidateType::static_);
  EXPECT_EQ(CandidateCodec::classify_type("STUN "), CandidateType::static_);
  EXPECT_EQ(CandidateCodec::classify_type(""), CandidateType::static_);
  EXPECT_EQ(CandidateCodec::classify_type(std::string_view("LOCAL\0x", 7)),
            CandidateType::static_);
}

class HolepunchCandidateDecoding : public ::testing::Test {
 protected:
  static auto decode(std::string_view text) -> std::optional<Candidate> {
    auto parsed = libchiaking::json::WireDialect::read<CandidateWire>(text);
    if (!parsed) return std::nullopt;
    return CandidateCodec::decode(parsed.value());
  }

  static auto refuses(std::string_view text) -> ::testing::AssertionResult {
    if (decode(text).has_value())
      return ::testing::AssertionFailure() << "accepted " << text;
    return ::testing::AssertionSuccess();
  }
};

TEST_F(HolepunchCandidateDecoding, ReadsEveryField) {
  const auto local = decode(
      R"({"type":"LOCAL","addr":"192.168.1.4","mappedAddr":"","port":1,"mappedPort":0})");
  ASSERT_TRUE(local.has_value());
  EXPECT_EQ(local.value().type, CandidateType::local);
  EXPECT_EQ(local.value().addr, "192.168.1.4");
  EXPECT_TRUE(local.value().mapped_addr.empty());
  EXPECT_EQ(local.value().port, 1);
  EXPECT_EQ(local.value().mapped_port, 0);
}

TEST_F(HolepunchCandidateDecoding, ReadsEachTypeAndTreatsUnknownAsStatic) {
  const auto stun = decode(
      R"({"type":"STUN","addr":"a","mappedAddr":"b","port":65535,"mappedPort":65535})");
  ASSERT_TRUE(stun.has_value());
  EXPECT_EQ(stun.value().type, CandidateType::stun);
  const auto derived = decode(
      R"({"type":"DERIVED","addr":"","mappedAddr":"","port":0,"mappedPort":0})");
  ASSERT_TRUE(derived.has_value());
  EXPECT_EQ(derived.value().type, CandidateType::derived);
  const auto unknown = decode(
      R"({"type":"WEIRD","addr":"","mappedAddr":"","port":5,"mappedPort":6})");
  ASSERT_TRUE(unknown.has_value());
  EXPECT_EQ(unknown.value().type, CandidateType::static_);
}

TEST_F(HolepunchCandidateDecoding, AcceptsAddressesUpToFortyFiveCharacters) {
  const std::string max_len(45, 'x');
  const auto max_addr =
      decode(std::string{R"({"type":"LOCAL","addr":")"} + max_len +
             R"(","mappedAddr":")" + max_len + R"(","port":0,"mappedPort":0})");
  ASSERT_TRUE(max_addr.has_value());
  EXPECT_EQ(max_addr.value().addr.size(), 45U);
}

TEST_F(HolepunchCandidateDecoding, RefusesAnOverlongOrNulAddress) {
  const std::string too_long(46, 'x');
  EXPECT_TRUE(refuses(std::string{R"({"type":"LOCAL","addr":")"} + too_long +
                      R"(","mappedAddr":"","port":0,"mappedPort":0})"));
  EXPECT_TRUE(
      refuses(std::string{R"({"type":"LOCAL","addr":"","mappedAddr":")"} +
              too_long + R"(","port":0,"mappedPort":0})"));
  EXPECT_TRUE(refuses(
      R"({"type":"LOCAL","addr":"a\u0000b","mappedAddr":"","port":0,"mappedPort":0})"));
}

TEST_F(HolepunchCandidateDecoding, RefusesMissingOrMistypedFields) {
  EXPECT_TRUE(refuses(R"({})"));
  EXPECT_TRUE(refuses(R"([])"));
  EXPECT_TRUE(
      refuses(R"({"type":"LOCAL","addr":"","mappedAddr":"","port":1})"));
  EXPECT_TRUE(
      refuses(R"({"type":"LOCAL","addr":"","mappedAddr":"","mappedPort":1})"));
  EXPECT_TRUE(refuses(
      R"({"type":1,"addr":"","mappedAddr":"","port":0,"mappedPort":0})"));
  EXPECT_TRUE(refuses(
      R"({"type":"LOCAL","addr":1,"mappedAddr":"","port":0,"mappedPort":0})"));
  EXPECT_TRUE(refuses(
      R"({"type":"LOCAL","addr":"","mappedAddr":null,"port":0,"mappedPort":0})"));
}

TEST_F(HolepunchCandidateDecoding, WrapsPortsToSixteenBits) {
  const auto wrapped = decode(
      R"({"type":"LOCAL","addr":"","mappedAddr":"","port":65536,"mappedPort":-1})");
  ASSERT_TRUE(wrapped.has_value());
  EXPECT_EQ(wrapped.value().port, 0);
  EXPECT_EQ(wrapped.value().mapped_port, 65535);
}

TEST_F(HolepunchCandidateDecoding, RefusesPortsThatAreNotPlainIntegers) {
  for (const auto* bad : {"1.0", "1e2", "\"1\"", "null", "true", "[]", "{}"}) {
    EXPECT_TRUE(refuses(
        std::string{R"({"type":"LOCAL","addr":"","mappedAddr":"","port":)"} +
        bad + R"(,"mappedPort":0})"));
    EXPECT_TRUE(refuses(
        std::string{
            R"({"type":"LOCAL","addr":"","mappedAddr":"","port":0,"mappedPort":)"} +
        bad + "}"));
  }
}

class HolepunchCandidateListDecoding : public ::testing::Test {
 protected:
  static auto decode(std::string_view text)
      -> std::optional<std::vector<Candidate>> {
    auto parsed = libchiaking::json::WireDialect::read<ConnRequestWire>(text);
    if (!parsed) return std::nullopt;
    return CandidateCodec::decode_list(parsed.value().candidate);
  }

  static auto refuses(std::string_view text) -> ::testing::AssertionResult {
    if (decode(text).has_value())
      return ::testing::AssertionFailure() << "accepted " << text;
    return ::testing::AssertionSuccess();
  }
};

TEST_F(HolepunchCandidateListDecoding, ReadsAnEmptyList) {
  const auto empty = decode(R"({"candidate":[]})");
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty.value().empty());
}

TEST_F(HolepunchCandidateListDecoding, ReadsEveryCandidateInOrder) {
  const auto two = decode(
      R"({"candidate":[)"
      R"({"type":"STUN","addr":"a","mappedAddr":"b","port":1234,"mappedPort":5678},)"
      R"({"type":"LOCAL","addr":"192.168.1.4","mappedAddr":"","port":1,"mappedPort":0}]})");
  ASSERT_TRUE(two.has_value());
  const auto& list = two.value();
  ASSERT_EQ(list.size(), 2U);
  EXPECT_EQ(list[0].type, CandidateType::stun);
  EXPECT_EQ(list[0].addr, "a");
  EXPECT_EQ(list[0].mapped_addr, "b");
  EXPECT_EQ(list[0].port, 1234);
  EXPECT_EQ(list[0].mapped_port, 5678);
  EXPECT_EQ(list[1].type, CandidateType::local);
  EXPECT_EQ(list[1].port, 1);
  EXPECT_EQ(list[1].mapped_port, 0);
}

TEST_F(HolepunchCandidateListDecoding, RefusesAMissingOrMalformedList) {
  EXPECT_TRUE(refuses(R"({"other":[]})"));
  EXPECT_TRUE(refuses(R"({"candidate":{}})"));
  EXPECT_TRUE(refuses(R"({"candidate":1})"));
  EXPECT_TRUE(refuses("[]"));
  EXPECT_TRUE(refuses(
      R"({"candidate":[{"type":"LOCAL","addr":"a","mappedAddr":"","port":1,"mappedPort":0},)"
      R"({"type":"LOCAL","addr":"","mappedAddr":"","port":0}]})"));
  EXPECT_TRUE(refuses(R"({"candidate":["not-an-object"]})"));
  EXPECT_TRUE(refuses(
      R"({"candidate":["str",{"type":"LOCAL","addr":"a","mappedAddr":"","port":0,"mappedPort":0}]})"));
}

TEST_F(HolepunchCandidateListDecoding, IgnoresOtherMembers) {
  const auto ignored =
      decode(R"({"meta":"junk","candidate":[],"nonce":12345,"extra":[1,2,3]})");
  ASSERT_TRUE(ignored.has_value());
  EXPECT_TRUE(ignored.value().empty());
}

TEST_F(HolepunchCandidateListDecoding, RefusesMoreThanTheMaximumCount) {
  std::vector<CandidateWire> listed(
      CandidateCodec::max_count + 1U,
      CandidateWire{"LOCAL", "a", "",
                    libchiaking::json::ps::Number::from_token("1"),
                    libchiaking::json::ps::Number::from_token("0")});
  EXPECT_FALSE(CandidateCodec::decode_list(listed).has_value());
}

class HolepunchCandidateEncoding : public ::testing::Test {
 protected:
  template <class Value>
  static auto spelled(const Value& value) -> std::string {
    const auto written = libchiaking::json::ps::write<
        libchiaking::json::ps::body_options_with_nulls>(value);
    EXPECT_TRUE(written);
    if (!written) return {};
    return std::string{written.value().view()};
  }

  const Candidate local{CandidateType::local, "192.168.1.42", "203.0.113.7",
                        12345, 54321};
  const Candidate stun{CandidateType::stun, "2001:db8::1", "2001:db8::2", 0,
                       65535};
};

TEST_F(HolepunchCandidateEncoding, WritesEachType) {
  const auto local_json = CandidateCodec::encode(local);
  ASSERT_TRUE(local_json.has_value());
  EXPECT_EQ(
      spelled(local_json.value()),
      R"({"type":"LOCAL","addr":"192.168.1.42","mappedAddr":"203.0.113.7","port":12345,"mappedPort":54321})");
  const auto stun_json = CandidateCodec::encode(stun);
  ASSERT_TRUE(stun_json.has_value());
  EXPECT_EQ(
      spelled(stun_json.value()),
      R"({"type":"STUN","addr":"2001:db8::1","mappedAddr":"2001:db8::2","port":0,"mappedPort":65535})");
  const Candidate derived{CandidateType::derived, "fe80::1", "", 9, 10};
  EXPECT_EQ(
      spelled(CandidateCodec::encode(derived).value()),
      R"({"type":"DERIVED","addr":"fe80::1","mappedAddr":"","port":9,"mappedPort":10})");
  const Candidate static_candidate{CandidateType::static_, "1.2.3.4", "", 1, 2};
  EXPECT_EQ(
      spelled(CandidateCodec::encode(static_candidate).value()),
      R"({"type":"STATIC","addr":"1.2.3.4","mappedAddr":"","port":1,"mappedPort":2})");
}

TEST_F(HolepunchCandidateEncoding, EscapesQuotesAndControlCharacters) {
  const Candidate quoted{CandidateType::local, "a\"b", "c\\d", 1, 2};
  EXPECT_EQ(spelled(CandidateCodec::encode(quoted).value()),
            "{\"type\":\"LOCAL\",\"addr\":\"a\\\"b\",\"mappedAddr\":\"c\\\\d\","
            "\"port\":1,\"mappedPort\":2}");
  const Candidate control{CandidateType::local, "a\x01", "b\n", 1, 2};
  EXPECT_EQ(spelled(CandidateCodec::encode(control).value()),
            "{\"type\":\"LOCAL\",\"addr\":\"a\\u0001\",\"mappedAddr\":\"b\\n\","
            "\"port\":1,\"mappedPort\":2}");
  const Candidate high_control{CandidateType::local, "a\x1f\x7f", "\b\f\r\t", 1,
                               2};
  EXPECT_EQ(spelled(CandidateCodec::encode(high_control).value()),
            "{\"type\":\"LOCAL\",\"addr\":\"a\\u001F\x7f\",\"mappedAddr\":"
            "\"\\b\\f\\r\\t\","
            "\"port\":1,\"mappedPort\":2}");
}

TEST_F(HolepunchCandidateEncoding, RefusesAnOverlongNulOrInvalidUtf8Address) {
  auto invalid = local;
  invalid.addr = std::string(46, 'a');
  EXPECT_FALSE(CandidateCodec::encode(invalid));
  invalid.addr = std::string("a\0b", 3);
  EXPECT_FALSE(CandidateCodec::encode(invalid));
  invalid.addr = std::string("a\xff", 2);
  EXPECT_FALSE(CandidateCodec::encode(invalid));
  invalid = local;
  invalid.mapped_addr = std::string(46, 'a');
  EXPECT_FALSE(CandidateCodec::encode(invalid));
  invalid.mapped_addr = std::string("a\0b", 3);
  EXPECT_FALSE(CandidateCodec::encode(invalid));
  invalid.mapped_addr = std::string("a\xff", 2);
  EXPECT_FALSE(CandidateCodec::encode(invalid));
}

TEST_F(HolepunchCandidateEncoding, RefusesAnUnknownType) {
  auto invalid = local;
  invalid.type = static_cast<CandidateType>(255);
  const auto bad_type = CandidateCodec::encode(invalid);
  ASSERT_FALSE(bad_type.has_value());
  EXPECT_EQ(bad_type.error(), CandidateEncodeError::invalid_type);
}

TEST_F(HolepunchCandidateEncoding, WritesAListInOrder) {
  EXPECT_EQ(spelled(CandidateCodec::encode_list({}).value()), "[]");
  const std::array<Candidate, 2> pair{local, stun};
  EXPECT_EQ(
      spelled(CandidateCodec::encode_list(pair).value()),
      R"([{"type":"LOCAL","addr":"192.168.1.42","mappedAddr":"203.0.113.7","port":12345,"mappedPort":54321},{"type":"STUN","addr":"2001:db8::1","mappedAddr":"2001:db8::2","port":0,"mappedPort":65535}])");
}

TEST_F(HolepunchCandidateEncoding, RefusesAListWithAnInvalidCandidate) {
  auto bad = local;
  bad.addr = std::string(46, 'a');
  const std::array<Candidate, 3> mixed{local, bad, stun};
  const auto invalid_list = CandidateCodec::encode_list(mixed);
  ASSERT_FALSE(invalid_list.has_value());
  EXPECT_EQ(invalid_list.error(), CandidateListEncodeError::invalid_candidate);
}

TEST_F(HolepunchCandidateEncoding, RefusesAListThatIsTooLongOrTooLarge) {
  const std::vector<Candidate> too_many(CandidateCodec::max_count + 1U, local);
  const auto too_many_result = CandidateCodec::encode_list(too_many);
  ASSERT_FALSE(too_many_result.has_value());
  EXPECT_EQ(too_many_result.error(), CandidateListEncodeError::too_many);
  const Candidate heavy{CandidateType::local, std::string(45, '1'),
                        std::string(45, '2'), 65535, 65535};
  const std::vector<Candidate> huge(100U, heavy);
  const auto huge_result = CandidateCodec::encode_list(huge);
  ASSERT_FALSE(huge_result.has_value());
  EXPECT_EQ(huge_result.error(), CandidateListEncodeError::too_large);
}

}  // namespace
}  // namespace nat_test
