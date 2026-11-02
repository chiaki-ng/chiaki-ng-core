#pragma once

#include "src/core/net/nat/stun/message.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nat_test {

class StunMessageRig : public ::testing::Test {
 protected:
  using Message = libchiaking::net::nat::stun::Message;
  using ReceivedMessage = libchiaking::net::nat::stun::ReceivedMessage;
  using TransactionId = libchiaking::net::nat::stun::TransactionId;
  using Endpoint = libchiaking::platform::SocketEndpoint;
  using Fingerprint = libchiaking::net::nat::stun::Fingerprint;

  struct RawAttribute {
    std::uint16_t type{};
    std::vector<std::byte> value;
    std::size_t padding{};
  };

  static constexpr std::string_view vector_password = "VOkJxbRl1RmTxUk/WvJxBt";

  static constexpr std::string_view sample_request =
      "000100582112a442b7e7a701bc34d686fa87dfae"
      "802200105354554e207465737420636c69656e74"
      "002400046e0001ff80290008932ff9b151263b36"
      "000600096576746a3a68367659202020"
      "000800149aeaa70cbfd8cb56781ef2b5b2d3f249c1b571a2"
      "80280004e57a3bcf";

  static constexpr std::string_view sample_ipv4_response =
      "0101003c2112a442b7e7a701bc34d686fa87dfae"
      "8022000b7465737420766563746f7220"
      "002000080001a147e112a643"
      "000800142b91f599fd9e90c38c7489f92af9ba53f06be7d7"
      "80280004c07d4c96";

  static constexpr std::string_view sample_ipv6_response =
      "010100482112a442b7e7a701bc34d686fa87dfae"
      "8022000b7465737420766563746f7220"
      "002000140002a1470113a9faa5d3f179bc25f4b5bed2b9d9"
      "00080014a382954e4be67bf11784c97c8292c275bfe3ed41"
      "80280004c8fb0b4c";

  static auto from_hex(std::string_view text) -> std::vector<std::byte> {
    std::string compact;
    for (const auto symbol : text) {
      if (symbol != ' ') compact.push_back(symbol);
    }
    return test_support::Bytes::unhex(compact);
  }

  static auto key() -> std::vector<std::byte> {
    return test_support::Bytes::of(vector_password);
  }

  static auto transaction(std::uint8_t seed) -> TransactionId {
    TransactionId id{};
    for (std::size_t index = 0; index < id.size(); ++index)
      id[index] = static_cast<std::byte>(seed + index * 17U);
    return id;
  }

  static auto v4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d,
                 std::uint16_t port) -> Endpoint {
    return Endpoint{libchiaking::platform::IpAddress::ipv4({a, b, c, d}), port};
  }

  static auto v6(std::uint16_t last, std::uint16_t port) -> Endpoint {
    std::array<std::uint8_t, 16> bytes{0x20, 0x01, 0x0d, 0xb8};
    bytes[14] = static_cast<std::uint8_t>(last >> 8U);
    bytes[15] = static_cast<std::uint8_t>(last & 0xffU);
    return Endpoint{libchiaking::platform::IpAddress::ipv6(bytes), port};
  }

  static auto decode_ok(std::span<const std::byte> bytes) -> ReceivedMessage {
    auto decoded = ReceivedMessage::decode(bytes);
    EXPECT_TRUE(decoded) << "the message does not decode";
    if (!decoded) return {};
    return std::move(decoded.value());
  }

  auto encode_ok(const Message& message, std::span<const std::byte> integrity,
                 bool fingerprint) -> std::vector<std::byte> {
    auto encoded = message.encode(
        crypto, integrity,
        fingerprint ? Fingerprint::present : Fingerprint::absent);
    EXPECT_TRUE(encoded) << "the message does not encode";
    if (!encoded) return {};
    return std::move(encoded.value());
  }

  static auto raw_message(std::uint16_t type,
                          const std::vector<RawAttribute>& attributes)
      -> std::vector<std::byte> {
    std::vector<std::byte> bytes{static_cast<std::byte>(type >> 8U),
                                 static_cast<std::byte>(type & 0xffU),
                                 std::byte{},
                                 std::byte{},
                                 std::byte{0x21},
                                 std::byte{0x12},
                                 std::byte{0xa4},
                                 std::byte{0x42}};
    const auto id = transaction(3);
    bytes.insert(bytes.end(), id.begin(), id.end());
    for (const auto& item : attributes) {
      bytes.push_back(static_cast<std::byte>(item.type >> 8U));
      bytes.push_back(static_cast<std::byte>(item.type & 0xffU));
      bytes.push_back(static_cast<std::byte>(item.value.size() >> 8U));
      bytes.push_back(static_cast<std::byte>(item.value.size() & 0xffU));
      bytes.insert(bytes.end(), item.value.begin(), item.value.end());
      bytes.insert(bytes.end(), item.padding, std::byte{});
    }
    const auto length = bytes.size() - libchiaking::net::nat::stun::header_size;
    bytes[2] = static_cast<std::byte>(length >> 8U);
    bytes[3] = static_cast<std::byte>(length & 0xffU);
    return bytes;
  }

  static auto transaction_of(std::span<const std::byte> bytes)
      -> TransactionId {
    TransactionId id{};
    std::copy_n(bytes.begin() + 8, id.size(), id.begin());
    return id;
  }

  auto reproduces(std::span<const std::byte> vector,
                  const ReceivedMessage& decoded)
      -> ::testing::AssertionResult {
    if (!decoded.integrity_offset)
      return ::testing::AssertionFailure() << "no integrity offset";
    std::vector<std::byte> rebuilt(
        vector.begin(), vector.begin() + static_cast<std::ptrdiff_t>(
                                             *decoded.integrity_offset));
    const auto k = key();
    if (!Message::append_integrity(crypto, rebuilt, k))
      return ::testing::AssertionFailure() << "integrity was not appended";
    Message::append_fingerprint(rebuilt);
    if (rebuilt.size() != vector.size())
      return ::testing::AssertionFailure()
             << "rebuilt " << rebuilt.size() << " bytes of " << vector.size();
    if (!std::equal(rebuilt.begin(), rebuilt.end(), vector.begin()))
      return ::testing::AssertionFailure() << "rebuilt bytes differ";
    return ::testing::AssertionSuccess();
  }

  auto signed_request() -> std::vector<std::byte> {
    Message message;
    message.kind = libchiaking::net::nat::stun::MessageClass::request;
    message.transaction = transaction(77);
    message.username = libchiaking::SecretString{"abcd:efgh"};
    message.priority = 1234;
    message.ice_controlled = 99;
    const auto k = key();
    return encode_ok(message, k, true);
  }

  static auto refused(std::span<const std::byte> bytes)
      -> ::testing::AssertionResult {
    if (ReceivedMessage::decode(bytes))
      return ::testing::AssertionFailure() << "the message decodes";
    if (Message::recognises(bytes))
      return ::testing::AssertionFailure() << "the message is recognised";
    return ::testing::AssertionSuccess();
  }

  test_support::SoftwareCrypto crypto;
};

}  // namespace nat_test
