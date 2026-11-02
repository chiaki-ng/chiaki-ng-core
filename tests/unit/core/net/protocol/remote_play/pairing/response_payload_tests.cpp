#include "src/core/net/protocol/remote_play/pairing/response.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace remote_play_test {
namespace {

using libchiaking::net::protocol::remote_play::RegistrationResponseCodec;
using libchiaking::net::protocol::remote_play::RegistrationTarget;
using libchiaking::net::protocol::remote_play::ResponsePayloadError;

class RegistrationResponsePayload : public ::testing::Test {
 protected:
  static auto parse(std::string_view text,
                    RegistrationTarget target = RegistrationTarget::ps5_1) {
    return RegistrationResponseCodec::parse_payload(
        target,
        std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(text.data()), text.size()});
  }

  static auto ps5_body(bool lf) -> std::string {
    const std::string nl = lf ? "\n" : "\r\n";
    std::string body;
    body += "PS5-RegistKey: 101112131415161718191a1b1c1d1e1f" + nl;
    body += "RP-Key: 000102030405060708090a0b0c0d0e0f" + nl;
    body += "PS5-Mac: 001122334455" + nl;
    body += "RP-KeyType: 16" + nl;
    body += "PS5-Nickname: Living Room" + nl;
    body += "AP-Ssid: HomeNet" + nl;
    body += "AP-Bssid: aa:bb:cc:dd:ee:ff" + nl;
    body += "AP-Name: Console AP" + nl;
    body += "AP-Key: wpa-secret" + nl;
    body += "RP-SupportCmd: 1" + nl;
    body += "X-Unknown-Field: whatever" + nl;
    return body;
  }

  static auto starts_with(const auto& field, std::span<const std::byte> bytes)
      -> bool {
    return std::equal(bytes.begin(), bytes.end(), field.data());
  }

  static auto text(const auto& field) -> std::string {
    return std::string(field.data());
  }

  static auto fails_with(const auto& result, ResponsePayloadError error)
      -> testing::AssertionResult {
    if (result) return testing::AssertionFailure() << "the payload parsed";
    if (result.error() != error)
      return testing::AssertionFailure()
             << "failed with " << testing::PrintToString(result.error());
    return testing::AssertionSuccess();
  }

  static constexpr std::array<std::byte, 16> key16 = [] {
    std::array<std::byte, 16> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
      result[i] = static_cast<std::byte>(i);
    return result;
  }();
  static constexpr std::array<std::byte, 16> regist16 = [] {
    std::array<std::byte, 16> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
      result[i] = static_cast<std::byte>(i + 0x10);
    return result;
  }();
  static constexpr std::array<std::byte, 6> mac{
      std::byte{0x00}, std::byte{0x11}, std::byte{0x22},
      std::byte{0x33}, std::byte{0x44}, std::byte{0x55}};
  static constexpr std::string_view minimal =
      "PS5-RegistKey: 41ff\nRP-Key: "
      "000102030405060708090a0b0c0d0e0f\nPS5-Mac: 001122334455\n";
};

TEST_F(RegistrationResponsePayload, ReadsEveryFieldOfAPs5Registration) {
  const auto parsed = parse(ps5_body(false));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value().target, RegistrationTarget::ps5_1);
  EXPECT_TRUE(starts_with(parsed.value().key, key16));
  EXPECT_TRUE(starts_with(parsed.value().registration_key, regist16));
  EXPECT_TRUE(starts_with(parsed.value().mac, mac));
  EXPECT_EQ(parsed.value().key_type, 16U);
  EXPECT_EQ(text(parsed.value().nickname), "Living Room");
  EXPECT_EQ(text(parsed.value().ap_ssid), "HomeNet");
  EXPECT_EQ(text(parsed.value().ap_key), "wpa-secret");
  EXPECT_EQ(parsed.value().ap_key[11], '\0');
}

TEST_F(RegistrationResponsePayload, ReadsAPs4RegistrationWithAShortKey) {
  const auto parsed = parse(
      "PS4-RegistKey: 41ff\nRP-Key: 000102030405060708090a0b0c0d0e0f\n"
      "PS4-Mac: 001122334455\nRP-KeyType: 8\nPS4-Nickname: PS4\nX-Foo: bar\n",
      RegistrationTarget::ps4_10);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value().registration_key[0], std::byte{0x41});
  EXPECT_EQ(parsed.value().registration_key[1], std::byte{0xff});
  EXPECT_EQ(parsed.value().registration_key[2], std::byte{0});
  EXPECT_EQ(parsed.value().key_type, 8U);
  EXPECT_EQ(text(parsed.value().nickname), "PS4");
}

TEST_F(RegistrationResponsePayload, ReadsLineFeedOnlyLines) {
  EXPECT_TRUE(parse(ps5_body(true)));
}

TEST_F(RegistrationResponsePayload, NeedsTheKeysAndTheMac) {
  for (const std::string_view drop :
       {"PS5-RegistKey: 41ff\n", "RP-Key: 000102030405060708090a0b0c0d0e0f\n",
        "PS5-Mac: 001122334455\n"}) {
    std::string payload{minimal};
    payload.erase(payload.find(drop), drop.size());
    EXPECT_TRUE(fails_with(parse(payload), ResponsePayloadError::missing_field))
        << drop;
  }
}

TEST_F(RegistrationResponsePayload, IgnoresTheFieldsOfTheOtherConsole) {
  EXPECT_TRUE(fails_with(
      parse("PS4-RegistKey: 41ff\nRP-Key: "
            "000102030405060708090a0b0c0d0e0f\nPS4-Mac: 001122334455\n"),
      ResponsePayloadError::missing_field));
}

TEST_F(RegistrationResponsePayload, IgnoresKeysAndMacsThatAreNotValidHex) {
  const std::string tail =
      "RP-Key: 000102030405060708090a0b0c0d0e0f\nPS5-Mac: 001122334455\n";
  for (const std::string regist :
       {"410", "41zz", "101112131415161718191a1b1c1d1e1f00"})
    EXPECT_TRUE(fails_with(parse("PS5-RegistKey: " + regist + "\n" + tail),
                           ResponsePayloadError::missing_field))
        << regist;
  EXPECT_TRUE(fails_with(
      parse("PS5-RegistKey: 41ff\nRP-Key: "
            "000102030405060708090a0b0c0d0e0f\nPS5-Mac: 0011223344\n"),
      ResponsePayloadError::missing_field));
  EXPECT_TRUE(fails_with(
      parse("PS5-RegistKey: 41ff\nRP-Key: "
            "000102030405060708090a0b0c0d0e0f00\nPS5-Mac: 001122334455\n"),
      ResponsePayloadError::missing_field));
}

TEST_F(RegistrationResponsePayload, NeedsARegistrationKeyThatIsNotEmpty) {
  const std::string keys = "RP-Key: 000102030405060708090a0b0c0d0e0f\n";
  for (const std::string_view regist :
       {"PS5-RegistKey:\n", "PS5-RegistKey: \n", "PS5-RegistKey: 00\n",
        "PS5-RegistKey: 0041\n"})
    EXPECT_TRUE(fails_with(
        parse(std::string{regist} + keys + "PS5-Mac: 001122334455\n"),
        ResponsePayloadError::missing_field))
        << regist;
  for (const std::string_view regist :
       {"PS4-RegistKey: \n", "PS4-RegistKey: 00\n", "PS4-RegistKey: 0041\n"})
    EXPECT_TRUE(
        fails_with(parse(std::string{regist} + keys + "PS4-Mac: 001122334455\n",
                         RegistrationTarget::ps4_10),
                   ResponsePayloadError::missing_field))
        << regist;
}

TEST_F(RegistrationResponsePayload, KeepsTheFirstOfRepeatedFields) {
  const auto parsed = parse(
      ps5_body(false) + "PS5-Mac: 00\nRP-KeyType: 7\nPS5-Nickname: Other\n");
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(starts_with(parsed.value().mac, mac));
  EXPECT_EQ(parsed.value().key_type, 16U);
  EXPECT_EQ(text(parsed.value().nickname), "Living Room");
}

TEST_F(RegistrationResponsePayload, KeepsAnUnreadableFirstMacOverLaterOnes) {
  const auto parsed = parse("PS5-Mac: zz\n" + ps5_body(false));
  ASSERT_TRUE(parsed);
  const std::array<std::byte, 6> zero_mac{};
  EXPECT_TRUE(starts_with(parsed.value().mac, zero_mac));
}

TEST_F(RegistrationResponsePayload, KeepsAnUnreadableFirstKeyOverLaterOnes) {
  const auto parsed = parse("RP-Key: zz\n" + ps5_body(false));
  ASSERT_TRUE(parsed);
  const std::array<std::byte, 16> zero_key{};
  EXPECT_TRUE(starts_with(parsed.value().key, zero_key));
}

TEST_F(RegistrationResponsePayload, StopsReadingAtANul) {
  std::string payload = ps5_body(false);
  const auto valid = payload.size();
  payload += std::string(1, '\0') + "garbage line here\n";
  EXPECT_TRUE(parse(payload));
  payload.resize(valid);
  payload[3] = '\0';
  EXPECT_TRUE(fails_with(parse(payload), ResponsePayloadError::missing_field));
}

TEST_F(RegistrationResponsePayload, RefusesAnOversizedPayload) {
  std::string payload(1501, 'a');
  payload += "\n";
  EXPECT_TRUE(fails_with(parse(payload), ResponsePayloadError::oversize));
}

TEST_F(RegistrationResponsePayload, RefusesALineWithoutAName) {
  EXPECT_TRUE(fails_with(parse("garbage line here\n"),
                         ResponsePayloadError::malformed));
  EXPECT_TRUE(fails_with(
      parse("PS5-RegistKey:41ff\nRP-Key: "
            "000102030405060708090a0b0c0d0e0f\nPS5-Mac: 001122334455\n: "
            "novalue\n"),
      ResponsePayloadError::malformed));
}

TEST_F(RegistrationResponsePayload, ReadsATruncatedLastLineOnlyIfItIsOptional) {
  std::string payload = ps5_body(false);
  payload.resize(payload.size() - 5);
  EXPECT_TRUE(parse(payload));
  EXPECT_TRUE(fails_with(
      parse("PS5-RegistKey: 41\nRP-Key: "
            "000102030405060708090a0b0c0d0e0f\nPS5-Mac: 001122334455"),
      ResponsePayloadError::missing_field));
}

TEST_F(RegistrationResponsePayload, ReadsAnInvalidKeyTypeAsZero) {
  const std::string base{minimal};
  for (const std::string_view value :
       {"4294967296", "0x1ffffffff", "12x", "junk"}) {
    const auto parsed =
        parse(base + "RP-KeyType: " + std::string{value} + "\n");
    ASSERT_TRUE(parsed) << value;
    EXPECT_EQ(parsed.value().key_type, 0U) << value;
  }
}

TEST_F(RegistrationResponsePayload, ReadsAKeyTypeWithALeadingZeroAsDecimal) {
  const auto parsed = parse(std::string{minimal} + "RP-KeyType: 010\n");
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value().key_type, 10U);
}

TEST_F(RegistrationResponsePayload, LeavesAnAbsentKeyTypeZero) {
  const auto parsed = parse(minimal);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value().key_type, 0U);
}

TEST_F(RegistrationResponsePayload, RefusesAnOverlongAccessPointKey) {
  const auto parsed =
      parse(std::string{minimal} + "AP-Key: " + std::string(80, 'k') + "\n");
  EXPECT_TRUE(fails_with(parsed, ResponsePayloadError::invalid_value));
}

TEST_F(RegistrationResponsePayload, KeepsAnAccessPointKeyThatFillsItsRecord) {
  const auto parsed =
      parse(std::string{minimal} + "AP-Key: " + std::string(79, 'k') + "\n");
  ASSERT_TRUE(parsed);
  EXPECT_EQ(text(parsed.value().ap_key), std::string(79, 'k'));
}

TEST_F(RegistrationResponsePayload, PairsWithoutADisplayFieldThatDoesNotFit) {
  const auto parsed =
      parse(std::string{minimal} + "AP-Bssid: " + std::string(32, 'b') +
            "\nAP-Ssid: " + std::string(48, 's') +
            "\nAP-Name: " + std::string(32, 'n') +
            "\nPS5-Nickname: " + std::string(32, 'p') + "\n");
  ASSERT_TRUE(parsed);
  EXPECT_EQ(text(parsed.value().ap_bssid), "");
  EXPECT_EQ(text(parsed.value().ap_ssid), "");
  EXPECT_EQ(text(parsed.value().ap_name), "");
  EXPECT_EQ(text(parsed.value().nickname), "");
}

TEST_F(RegistrationResponsePayload, KeepsARepeatedNicknameThatFits) {
  const auto parsed =
      parse(std::string{minimal} + "PS5-Nickname: " + std::string(32, 'p') +
            "\nPS5-Nickname: Living Room\n");
  ASSERT_TRUE(parsed);
  EXPECT_EQ(text(parsed.value().nickname), "Living Room");
}

TEST_F(RegistrationResponsePayload, KeepsANicknameThatFillsItsRecord) {
  const auto parsed = parse(std::string{minimal} +
                            "PS5-Nickname: " + std::string(31, 'p') + "\n");
  ASSERT_TRUE(parsed);
  EXPECT_EQ(text(parsed.value().nickname), std::string(31, 'p'));
}

}  // namespace
}  // namespace remote_play_test
