#include "src/core/net/nat/stun/binding_client.hpp"
#include "src/core/net/nat/stun/message.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/nat/stun/binding_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nat_test {
namespace {

namespace stun = libchiaking::net::nat::stun;
namespace platform = libchiaking::platform;

class StunBindingCodec : public StunBindingRig {
 protected:
  using Error = stun::BindingClient::ResponseError;

  static auto response(std::uint8_t type,
                       const std::vector<std::uint8_t>& value)
      -> std::vector<std::byte> {
    const std::size_t padded = (value.size() + 3U) & ~std::size_t{3U};
    std::vector<std::byte> packet(24U + padded);
    packet[0] = std::byte{1};
    packet[1] = std::byte{1};
    const auto body_size = static_cast<std::uint16_t>(packet.size() - 20U);
    packet[2] = static_cast<std::byte>(body_size >> 8U);
    packet[3] = static_cast<std::byte>(body_size & 0xffU);
    for (std::size_t i = 0; i < 4U; ++i)
      packet[4U + i] = stun::magic_cookie_bytes[i];
    for (std::size_t i = 0; i < transaction.size(); ++i)
      packet[8U + i] = transaction[i];
    packet[21] = static_cast<std::byte>(type);
    packet[22] = static_cast<std::byte>(value.size() >> 8U);
    packet[23] = static_cast<std::byte>(value.size() & 0xffU);
    for (std::size_t i = 0; i < value.size(); ++i)
      packet[24U + i] = static_cast<std::byte>(value[i]);
    return packet;
  }

  static auto decode(const std::vector<std::byte>& packet) {
    return stun::BindingClient::mapped_endpoint(packet, transaction);
  }

  const std::vector<std::byte> plain_v4 =
      response(0x01, {0, 1, 0x12, 0x34, 192, 0, 2, 1});
  const std::vector<std::byte> plain_v6 =
      response(0x01, {0, 2, 0x12, 0x34, 0x20, 1, 0x0d, 0xb8, 0, 1,
                      0, 2, 0,    3,    0,    4, 0,    5,    0, 6});
};

TEST_F(StunBindingCodec, WritesAnEmptyBindingRequest) {
  constexpr std::array<std::byte, 20> request_golden{
      std::byte{0},    std::byte{1},    std::byte{0},    std::byte{0},
      std::byte{0x21}, std::byte{0x12}, std::byte{0xa4}, std::byte{0x42},
      std::byte{0xb7}, std::byte{0xe7}, std::byte{0x47}, std::byte{0x05},
      std::byte{0xbc}, std::byte{0x8a}, std::byte{0x31}, std::byte{0x59},
      std::byte{0x8c}, std::byte{0xf5}, std::byte{0x20}, std::byte{0x7a}};
  EXPECT_EQ(stun::BindingClient::request(transaction), request_golden);
}

TEST_F(StunBindingCodec, WritesARequestTheStrictDecoderReadsBack) {
  constexpr stun::TransactionId stepped_transaction{
      std::byte{0xf1}, std::byte{0xde}, std::byte{0xcb}, std::byte{0xb8},
      std::byte{0xa5}, std::byte{0x92}, std::byte{0x7f}, std::byte{0x6c},
      std::byte{0x59}, std::byte{0x46}, std::byte{0x33}, std::byte{0x20}};
  constexpr std::array<std::byte, 20> stepped_request{
      std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x21}, std::byte{0x12}, std::byte{0xa4}, std::byte{0x42},
      std::byte{0xf1}, std::byte{0xde}, std::byte{0xcb}, std::byte{0xb8},
      std::byte{0xa5}, std::byte{0x92}, std::byte{0x7f}, std::byte{0x6c},
      std::byte{0x59}, std::byte{0x46}, std::byte{0x33}, std::byte{0x20}};
  EXPECT_EQ(stun::BindingClient::request(stepped_transaction), stepped_request);
  const auto read_back = stun::ReceivedMessage::decode(stepped_request);
  ASSERT_TRUE(read_back);
  EXPECT_EQ(read_back.value().message.kind, stun::MessageClass::request);
  EXPECT_EQ(read_back.value().message.transaction, stepped_transaction);
}

TEST_F(StunBindingCodec, ReadsAPlainOrXoredIpv4Mapping) {
  const auto mapped = decode(plain_v4);
  ASSERT_TRUE(mapped);
  EXPECT_EQ(mapped.value().address, platform::IpAddress::ipv4({192, 0, 2, 1}));
  EXPECT_EQ(mapped.value().port, 0x1234U);
  const auto xor_v4 =
      response(0x20, {0, 1, 0x33, 0x26, 0xe1, 0x12, 0xa6, 0x43});
  const auto xored = decode(xor_v4);
  ASSERT_TRUE(xored);
  EXPECT_EQ(xored.value(), mapped.value());
  const auto strict_v4 = stun::ReceivedMessage::decode(plain_v4);
  ASSERT_TRUE(strict_v4);
  EXPECT_EQ(strict_v4.value().message.mapped, mapped.value());
  const auto strict_xor_v4 = stun::ReceivedMessage::decode(xor_v4);
  ASSERT_TRUE(strict_xor_v4);
  EXPECT_EQ(strict_xor_v4.value().message.xor_mapped, mapped.value());
}

TEST_F(StunBindingCodec, ReadsAPlainOrXoredIpv6Mapping) {
  const auto mapped_v6 = decode(plain_v6);
  ASSERT_TRUE(mapped_v6);
  EXPECT_EQ(mapped_v6.value().address,
            platform::IpAddress::ipv6(
                {0x20, 1, 0x0d, 0xb8, 0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6}));
  EXPECT_EQ(mapped_v6.value().port, 0x1234U);
  const auto xor_v6 = response(
      0x20, {0,    2,    0x33, 0x26, 0x01, 0x13, 0xa9, 0xfa, 0xb7, 0xe6,
             0x47, 0x07, 0xbc, 0x89, 0x31, 0x5d, 0x8c, 0xf0, 0x20, 0x7c});
  const auto xored_v6 = decode(xor_v6);
  ASSERT_TRUE(xored_v6);
  EXPECT_EQ(xored_v6.value(), mapped_v6.value());
  const auto strict_v6 = stun::ReceivedMessage::decode(plain_v6);
  ASSERT_TRUE(strict_v6);
  EXPECT_EQ(strict_v6.value().message.mapped, mapped_v6.value());
  const auto strict_xor_v6 = stun::ReceivedMessage::decode(xor_v6);
  ASSERT_TRUE(strict_xor_v6);
  EXPECT_EQ(strict_xor_v6.value().message.xor_mapped, mapped_v6.value());
}

TEST_F(StunBindingCodec, SkipsAPaddedUnknownAttributeBeforeTheMapping) {
  auto padded = response(0x80, {0x61});
  padded.insert(padded.end(), plain_v4.begin() + 20, plain_v4.end());
  padded[3] = static_cast<std::byte>(padded.size() - 20U);
  const auto after_padding = decode(padded);
  ASSERT_TRUE(after_padding);
  EXPECT_EQ(after_padding.value(), decode(plain_v4).value());
}

TEST_F(StunBindingCodec, ReportsAShortPacket) {
  for (std::size_t length = 0; length < 20U; ++length) {
    const auto short_packet = std::vector<std::byte>(
        plain_v4.begin(),
        plain_v4.begin() + static_cast<std::ptrdiff_t>(length));
    const auto decoded = decode(short_packet);
    ASSERT_FALSE(decoded) << length;
    EXPECT_EQ(decoded.error(), Error::short_packet) << length;
  }
}

TEST_F(StunBindingCodec, ReportsWhichHeaderFieldIsWrong) {
  auto bad = plain_v4;
  bad[1] = std::byte{2};
  EXPECT_EQ(decode(bad).error(), Error::invalid_type);
  bad = plain_v4;
  bad[3] = std::byte{9};
  EXPECT_EQ(decode(bad).error(), Error::invalid_length);
  bad = plain_v4;
  bad[7] = std::byte{0};
  EXPECT_EQ(decode(bad).error(), Error::invalid_cookie);
  bad = plain_v4;
  bad[8] = std::byte{0};
  EXPECT_EQ(decode(bad).error(), Error::invalid_transaction);
  bad = plain_v4;
  bad.pop_back();
  EXPECT_EQ(decode(bad).error(), Error::invalid_length);
}

TEST_F(StunBindingCodec, ReportsAMalformedOrMissingMapping) {
  auto bad = plain_v4;
  bad[23] = std::byte{9};
  EXPECT_EQ(decode(bad).error(), Error::invalid_attribute);
  bad = plain_v4;
  bad[25] = std::byte{3};
  EXPECT_EQ(decode(bad).error(), Error::invalid_attribute);
  bad = plain_v4;
  bad[21] = std::byte{0x80};
  EXPECT_EQ(decode(bad).error(), Error::no_mapped_address);
}

TEST_F(StunBindingCodec, PassesAZeroPortOrAddressThrough) {
  auto bad = plain_v4;
  bad[26] = std::byte{0};
  bad[27] = std::byte{0};
  ASSERT_TRUE(decode(bad));
  EXPECT_EQ(decode(bad).value().port, 0);
  bad = plain_v4;
  for (std::size_t i = 28U; i < 32U; ++i) bad[i] = std::byte{};
  ASSERT_TRUE(decode(bad));
  EXPECT_TRUE(decode(bad).value().address.is_unspecified());
}

class StunBindingAnswer : public StunBindingRig {};

TEST_F(StunBindingAnswer, TakesAResponseToTheTransaction) {
  const auto packet = valid_response();
  const auto valid = stun::BindingClient::answer(packet, transaction);
  ASSERT_TRUE(valid);
  ASSERT_TRUE(valid.value());
  EXPECT_EQ(*valid.value(), expected_mapping());
}

TEST_F(StunBindingAnswer, PassesOverAResponseToAnotherTransaction) {
  auto stale = valid_response();
  stale[8] = std::byte{0};
  const auto stale_result = stun::BindingClient::answer(stale, transaction);
  ASSERT_TRUE(stale_result);
  EXPECT_FALSE(stale_result.value());
}

TEST_F(StunBindingAnswer, ReportsAMalformedResponse) {
  auto bad = valid_response();
  bad[3] = std::byte{9};
  const auto malformed = stun::BindingClient::answer(bad, transaction);
  ASSERT_FALSE(malformed);
  EXPECT_EQ(malformed.error(),
            stun::BindingClient::ResponseError::invalid_length);
}

}  // namespace
}  // namespace nat_test
