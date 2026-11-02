#include "tests/support/bytes.hpp"
#include "tests/unit/core/net/nat/stun/message_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace nat_test {
namespace {

using namespace libchiaking::net::nat::stun;
namespace platform = libchiaking::platform;

class StunRfc5769Vectors : public StunMessageRig {};

TEST_F(StunRfc5769Vectors, DecodeTheSampleRequest) {
  const auto bytes = from_hex(sample_request);
  EXPECT_TRUE(Message::recognises(bytes));
  const auto decoded = decode_ok(bytes);
  const auto& message = decoded.message;
  EXPECT_EQ(message.kind, MessageClass::request);
  EXPECT_EQ(message.transaction, transaction_of(bytes));
  ASSERT_TRUE(message.username);
  EXPECT_EQ(message.username->view(), "evtj:h6vY");
  ASSERT_TRUE(message.priority);
  EXPECT_EQ(*message.priority, 0x6e0001ffU);
  ASSERT_TRUE(message.ice_controlled);
  EXPECT_EQ(*message.ice_controlled, 0x932ff9b151263b36ULL);
  EXPECT_FALSE(message.ice_controlling);
  EXPECT_FALSE(message.use_candidate);
  EXPECT_TRUE(decoded.fingerprinted);
  EXPECT_TRUE(decoded.unknown_required.empty());
  const auto k = key();
  EXPECT_TRUE(decoded.verify_integrity(crypto, bytes, k));
  const auto wrong = test_support::Bytes::of("VOkJxbRl1RmTxUk/WvJxBu");
  EXPECT_FALSE(decoded.verify_integrity(crypto, bytes, wrong));
  EXPECT_TRUE(reproduces(bytes, decoded));
}

TEST_F(StunRfc5769Vectors, DecodeTheSampleIpv4Response) {
  const auto bytes = from_hex(sample_ipv4_response);
  EXPECT_TRUE(Message::recognises(bytes));
  const auto decoded = decode_ok(bytes);
  EXPECT_EQ(decoded.message.kind, MessageClass::success);
  ASSERT_TRUE(decoded.message.xor_mapped);
  EXPECT_EQ(*decoded.message.xor_mapped, v4(192, 0, 2, 1, 32853));
  EXPECT_TRUE(decoded.fingerprinted);
  const auto k = key();
  EXPECT_TRUE(decoded.verify_integrity(crypto, bytes, k));
  EXPECT_TRUE(reproduces(bytes, decoded));
}

TEST_F(StunRfc5769Vectors, DecodeTheSampleIpv6Response) {
  const auto bytes = from_hex(sample_ipv6_response);
  EXPECT_TRUE(Message::recognises(bytes));
  const auto decoded = decode_ok(bytes);
  EXPECT_EQ(decoded.message.kind, MessageClass::success);
  const std::array<std::uint8_t, 16> expected{
      0x20, 0x01, 0x0d, 0xb8, 0x12, 0x34, 0x56, 0x78,
      0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
  ASSERT_TRUE(decoded.message.xor_mapped);
  EXPECT_EQ(
      *decoded.message.xor_mapped,
      (platform::SocketEndpoint{platform::IpAddress::ipv6(expected), 32853}));
  const auto k = key();
  EXPECT_TRUE(decoded.verify_integrity(crypto, bytes, k));
  EXPECT_TRUE(reproduces(bytes, decoded));
}

class StunEncodedBytes : public StunMessageRig {
 protected:
  const TransactionId rfc_transaction =
      transaction_of(from_hex(sample_request));
};

TEST_F(StunEncodedBytes, MatchTheSampleIpv4Mapping) {
  Message ipv4;
  ipv4.kind = MessageClass::success;
  ipv4.transaction = rfc_transaction;
  ipv4.xor_mapped = v4(192, 0, 2, 1, 32853);
  EXPECT_EQ(encode_ok(ipv4, {}, false),
            from_hex("0101000c2112a442b7e7a701bc34d686fa87dfae"
                     "002000080001a147e112a643"));
}

TEST_F(StunEncodedBytes, MatchTheSampleIpv6Mapping) {
  Message ipv6;
  ipv6.kind = MessageClass::success;
  ipv6.transaction = rfc_transaction;
  ipv6.xor_mapped = platform::SocketEndpoint{
      platform::IpAddress::ipv6({0x20, 0x01, 0x0d, 0xb8, 0x12, 0x34, 0x56, 0x78,
                                 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                                 0x77}),
      32853};
  EXPECT_EQ(encode_ok(ipv6, {}, false),
            from_hex("010100182112a442b7e7a701bc34d686fa87dfae"
                     "002000140002a1470113a9faa5d3f179bc25f4b5bed2b9d9"));
}

TEST_F(StunEncodedBytes, PadTheUsernameOfARequestWithZeros) {
  Message request;
  request.kind = MessageClass::request;
  request.transaction = rfc_transaction;
  request.username = libchiaking::SecretString{"evtj:h6vY"};
  request.priority = 0x6e0001ffU;
  request.ice_controlled = 0x932ff9b151263b36ULL;
  request.use_candidate = true;
  EXPECT_EQ(encode_ok(request, {}, false),
            from_hex("000100282112a442b7e7a701bc34d686fa87dfae"
                     "000600096576746a3a68367659000000"
                     "002400046e0001ff"
                     "80290008932ff9b151263b36"
                     "00250000"));
}

TEST_F(StunEncodedBytes, ListUnknownAttributesOfARefusal) {
  Message refusal;
  refusal.kind = MessageClass::error;
  refusal.transaction = rfc_transaction;
  refusal.error =
      ErrorResponse{error_code::unknown_attribute, "Unknown Attribute"};
  refusal.unknown_attributes = {0x7777, 0x0014, 0x0015};
  EXPECT_EQ(encode_ok(refusal, {}, false),
            from_hex("011100282112a442b7e7a701bc34d686fa87dfae"
                     "0009001500000414556e6b6e6f776e204174747269627574"
                     "65000000"
                     "000a0006777700140015"
                     "0000"));
}

}  // namespace
}  // namespace nat_test
