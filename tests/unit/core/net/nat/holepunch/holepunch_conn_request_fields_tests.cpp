#include "src/core/net/nat/holepunch/holepunch_conn_request.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/nat/holepunch/holepunch_conn_request_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nat_test {
namespace {

using namespace libchiaking::net::nat::holepunch;

class HolepunchRequestIds : public ConnRequestRig {
 protected:
  static auto decode(std::string_view text) -> std::optional<ConnRequestIds> {
    auto parsed = read(text);
    if (!parsed) return std::nullopt;
    return ConnRequestCodec::decode_ids(*parsed);
  }

  static auto refuses(std::string_view text) -> ::testing::AssertionResult {
    if (decode(text).has_value())
      return ::testing::AssertionFailure() << "accepted " << text;
    return ::testing::AssertionSuccess();
  }
};

TEST_F(HolepunchRequestIds, ReadsZeroIds) {
  const auto zero = decode(R"({"sid":0,"peerSid":0})");
  ASSERT_TRUE(zero.has_value());
  EXPECT_EQ(zero.value().sid, 0U);
  EXPECT_EQ(zero.value().peer_sid, 0U);
}

TEST_F(HolepunchRequestIds, ClampsLargeIdsToThirtyOneBits) {
  const auto max = decode(R"({"sid":4294967295,"peerSid":2147483647})");
  ASSERT_TRUE(max.has_value());
  EXPECT_EQ(max.value().sid, 2147483647U);
  EXPECT_EQ(max.value().peer_sid, 2147483647U);
}

TEST_F(HolepunchRequestIds, WrapsNegativeIds) {
  const auto negative = decode(R"({"sid":-1,"peerSid":-99999999999})");
  ASSERT_TRUE(negative.has_value());
  EXPECT_EQ(negative.value().sid, 4294967295U);
  EXPECT_EQ(negative.value().peer_sid, 2147483648U);
  const auto minus_zero = decode(R"({"sid":-0,"peerSid":70000})");
  ASSERT_TRUE(minus_zero.has_value());
  EXPECT_EQ(minus_zero.value().sid, 0U);
  EXPECT_EQ(minus_zero.value().peer_sid, 70000U);
}

TEST_F(HolepunchRequestIds, ReadsReorderedMembersAndIgnoresExtras) {
  const auto reordered = decode(R"({"peerSid":7,"extra":true,"sid":3})");
  ASSERT_TRUE(reordered.has_value());
  EXPECT_EQ(reordered.value().sid, 3U);
  EXPECT_EQ(reordered.value().peer_sid, 7U);
}

TEST_F(HolepunchRequestIds, RefusesMissingOrMistypedIds) {
  EXPECT_TRUE(refuses(R"({})"));
  EXPECT_TRUE(refuses(R"([])"));
  EXPECT_TRUE(refuses(R"("x")"));
  EXPECT_TRUE(refuses(R"(null)"));
  EXPECT_TRUE(refuses(R"(1)"));
  EXPECT_TRUE(refuses(R"({"sid":1})"));
  EXPECT_TRUE(refuses(R"({"peerSid":1})"));
  EXPECT_TRUE(refuses(R"({"sid":"1","peerSid":2})"));
  EXPECT_TRUE(refuses(R"({"sid":1,"peerSid":"2"})"));
  EXPECT_TRUE(refuses(R"({"sid":null,"peerSid":2})"));
  EXPECT_TRUE(refuses(R"({"sid":true,"peerSid":2})"));
  EXPECT_TRUE(refuses(R"({"sid":[1],"peerSid":2})"));
  EXPECT_TRUE(refuses(R"({"sid":{"v":1},"peerSid":2})"));
}

TEST_F(HolepunchRequestIds, RefusesIdsThatAreNotPlainIntegers) {
  for (const auto* bad : {"1.0", "1e0", "1E2", "-1.5"}) {
    EXPECT_TRUE(refuses(std::string{"{\"sid\":"} + bad + ",\"peerSid\":1}"));
    EXPECT_TRUE(refuses(std::string{"{\"sid\":1,\"peerSid\":"} + bad + "}"));
  }
}

class HolepunchRequestNatType : public ConnRequestRig {
 protected:
  static auto decode(std::string_view text) -> std::optional<std::uint8_t> {
    auto parsed = read(text);
    if (!parsed) return std::nullopt;
    return ConnRequestCodec::decode_nat(*parsed);
  }

  static auto refuses(std::string_view text) -> ::testing::AssertionResult {
    if (decode(text).has_value())
      return ::testing::AssertionFailure() << "accepted " << text;
    return ::testing::AssertionSuccess();
  }
};

TEST_F(HolepunchRequestNatType, ReadsTheTypeAmongOtherMembers) {
  EXPECT_EQ(decode(R"({"sid":1,"natType":0})"), std::optional<std::uint8_t>{0});
  EXPECT_EQ(decode(R"({"natType":2,"peerSid":3})"),
            std::optional<std::uint8_t>{2});
  EXPECT_EQ(decode(R"({"natType":255})"), std::optional<std::uint8_t>{255});
  EXPECT_EQ(decode(R"({"extra":true,"natType":1})"),
            std::optional<std::uint8_t>{1});
}

TEST_F(HolepunchRequestNatType, RefusesAMissingOrMistypedType) {
  EXPECT_TRUE(refuses(R"({})"));
  EXPECT_TRUE(refuses(R"([])"));
  EXPECT_TRUE(refuses(R"("x")"));
  EXPECT_TRUE(refuses(R"(null)"));
  EXPECT_TRUE(refuses(R"(1)"));
  EXPECT_TRUE(refuses(R"({"sid":1})"));
  EXPECT_TRUE(refuses(R"({"natType":"2"})"));
  EXPECT_TRUE(refuses(R"({"natType":null})"));
  EXPECT_TRUE(refuses(R"({"natType":true})"));
  EXPECT_TRUE(refuses(R"({"natType":[2]})"));
  EXPECT_TRUE(refuses(R"({"natType":{"v":2}})"));
  for (const auto* bad : {"1.0", "1e0", "1E2"})
    EXPECT_TRUE(refuses(std::string{"{\"natType\":"} + bad + "}"));
}

TEST_F(HolepunchRequestNatType, WrapsToEightBits) {
  EXPECT_EQ(decode(R"({"natType":258})"), std::optional<std::uint8_t>{2});
  EXPECT_EQ(decode(R"({"natType":-1})"), std::optional<std::uint8_t>{255});
}

class HolepunchRequestMac : public ConnRequestRig {
 protected:
  using Mac = std::array<std::byte, libchiaking::mac_address_size>;

  static auto decode(std::string_view text) -> std::optional<Mac> {
    auto parsed = read(text);
    if (!parsed) return std::nullopt;
    return ConnRequestCodec::decode_mac(*parsed);
  }

  static auto refuses(std::string_view text) -> ::testing::AssertionResult {
    if (decode(text).has_value())
      return ::testing::AssertionFailure() << "accepted " << text;
    return ::testing::AssertionSuccess();
  }

  static auto bytes(std::array<unsigned, 6> values) -> Mac {
    Mac out{};
    for (std::size_t i = 0; i < out.size(); ++i)
      out[i] = static_cast<std::byte>(values[i]);
    return out;
  }
};

TEST_F(HolepunchRequestMac, ReadsColonSeparatedHexInEitherCase) {
  const auto expected = bytes({0x01, 0x23, 0x45, 0x67, 0x89, 0xab});
  EXPECT_EQ(decode(R"({"defaultRouteMacAddr":"01:23:45:67:89:ab"})"),
            std::optional<Mac>{expected});
  EXPECT_EQ(
      decode(R"({"extra":true,"defaultRouteMacAddr":"01:23:45:67:89:AB"})"),
      std::optional<Mac>{expected});
  EXPECT_EQ(decode(R"({"defaultRouteMacAddr":"fe:dc:BA:98:76:54"})"),
            std::optional<Mac>{bytes({0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54})});
}

TEST_F(HolepunchRequestMac, ReadsTextOfAnotherLengthAsZeros) {
  for (const auto* text :
       {R"({"defaultRouteMacAddr":""})", R"({"defaultRouteMacAddr":"short"})",
        R"({"defaultRouteMacAddr":"01:23:45:67:89:ab:cd"})"}) {
    EXPECT_EQ(decode(text), std::optional<Mac>{Mac{}}) << text;
  }
}

TEST_F(HolepunchRequestMac, ReadsAnEmbeddedNulAsZeros) {
  EXPECT_EQ(decode(R"({"defaultRouteMacAddr":"01:23:45:67:89:\u0000"})"),
            std::optional<Mac>{bytes({0, 0, 0, 0, 0, 0})});
  EXPECT_EQ(decode(R"({"defaultRouteMacAddr":"01:23:45:67:89:a\u0000"})"),
            std::optional<Mac>{bytes({0, 0, 0, 0, 0, 0})});
}

TEST_F(HolepunchRequestMac, RefusesAMissingOrMistypedAddress) {
  EXPECT_TRUE(refuses(R"({})"));
  EXPECT_TRUE(refuses(R"([])"));
  EXPECT_TRUE(refuses(R"({"defaultRouteMacAddr":null})"));
  EXPECT_TRUE(refuses(R"({"defaultRouteMacAddr":17})"));
  EXPECT_TRUE(refuses(R"({"defaultRouteMacAddr":true})"));
  EXPECT_TRUE(refuses(R"({"defaultRouteMacAddr":[]})"));
  EXPECT_TRUE(refuses(R"({"defaultRouteMacAddr":{}})"));
}

TEST_F(HolepunchRequestMac, RefusesAMalformedAddress) {
  EXPECT_TRUE(refuses(R"({"defaultRouteMacAddr":"01-23-45-67-89-ab"})"));
  EXPECT_TRUE(refuses(R"({"defaultRouteMacAddr":"0x:1:2:3:4:5:6789"})"));
  EXPECT_TRUE(refuses(R"({"defaultRouteMacAddr":"01:23:45:67:8 :9a"})"));
  EXPECT_TRUE(refuses(R"({"defaultRouteMacAddr":"01:23:45:67:89:zz"})"));
  EXPECT_TRUE(refuses(R"({"defaultRouteMacAddr":"01:23:45:67:89:a "})"));
  EXPECT_TRUE(refuses(R"({"defaultRouteMacAddr":"01:23:45:67:89:a:"})"));
  EXPECT_TRUE(refuses(R"({"defaultRouteMacAddr":" 1:-2:+3:0x4:5:ff"})"));
  EXPECT_TRUE(refuses(R"({"defaultRouteMacAddr":"1:2:3:4:5:789abcd"})"));
}

}  // namespace
}  // namespace nat_test
