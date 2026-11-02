#include "tests/unit/core/net/nat/stun/message_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace nat_test {
namespace {

using namespace libchiaking::net::nat::stun;

class StunRoundTrip : public StunMessageRig {
 protected:
  void SetUp() override {
    conflict.kind = MessageClass::error;
    conflict.transaction = transaction(33);
    conflict.error = ErrorResponse{
        error_code::role_conflict,
        std::string{ErrorResponse::reason_phrase(error_code::role_conflict)}};
    indication.kind = MessageClass::indication;
    indication.transaction = transaction(35);
  }

  const std::vector<std::byte> k = key();
  Message conflict;
  Message indication;
};

TEST_F(StunRoundTrip, KeepsEveryFieldOfAControllingRequest) {
  Message message;
  message.kind = MessageClass::request;
  message.transaction = transaction(9);
  message.username =
      libchiaking::SecretString{"0123456789abcdef:fedcba9876543210"};
  message.priority = 0x6e7f00ffU;
  message.ice_controlling = 0x0102030405060708ULL;
  message.use_candidate = true;
  const auto bytes = encode_ok(message, k, true);
  EXPECT_TRUE(Message::recognises(bytes));
  const auto decoded = decode_ok(bytes);
  EXPECT_EQ(decoded.message.kind, MessageClass::request);
  EXPECT_EQ(decoded.message.transaction, message.transaction);
  ASSERT_TRUE(decoded.message.username);
  EXPECT_EQ(decoded.message.username->view(), message.username->view());
  EXPECT_EQ(decoded.message.priority, message.priority);
  EXPECT_EQ(decoded.message.ice_controlling, message.ice_controlling);
  EXPECT_FALSE(decoded.message.ice_controlled);
  EXPECT_TRUE(decoded.message.use_candidate);
  EXPECT_TRUE(decoded.fingerprinted);
  EXPECT_TRUE(decoded.verify_integrity(crypto, bytes, k));
}

TEST_F(StunRoundTrip, KeepsEveryFieldOfAControlledRequest) {
  Message message;
  message.kind = MessageClass::request;
  message.transaction = transaction(9);
  message.priority = 0x6e7f00ffU;
  message.use_candidate = false;
  message.ice_controlled = 0xfedcba9876543210ULL;
  message.username = libchiaking::SecretString{"a:b"};
  const auto controlled = decode_ok(encode_ok(message, k, true));
  EXPECT_EQ(controlled.message.ice_controlled, message.ice_controlled);
  EXPECT_FALSE(controlled.message.ice_controlling);
  EXPECT_FALSE(controlled.message.use_candidate);
  ASSERT_TRUE(controlled.message.username);
  EXPECT_EQ(controlled.message.username->view(), "a:b");
}

TEST_F(StunRoundTrip, KeepsTheMappedAddressOfEitherFamily) {
  for (const auto& endpoint : {v4(203, 0, 113, 7, 40001), v4(10, 0, 0, 1, 1),
                               v6(0x1234, 65535), v6(1, 3478)}) {
    SCOPED_TRACE(endpoint.port);
    Message message;
    message.kind = MessageClass::success;
    message.transaction = transaction(21);
    message.xor_mapped = endpoint;
    const auto bytes = encode_ok(message, k, true);
    const auto decoded = decode_ok(bytes);
    EXPECT_EQ(decoded.message.kind, MessageClass::success);
    ASSERT_TRUE(decoded.message.xor_mapped);
    EXPECT_EQ(*decoded.message.xor_mapped, endpoint);
    EXPECT_TRUE(decoded.verify_integrity(crypto, bytes, k));
    const auto plain = decode_ok(encode_ok(message, {}, false));
    EXPECT_FALSE(plain.integrity_offset);
    EXPECT_FALSE(plain.fingerprinted);
    ASSERT_TRUE(plain.message.xor_mapped);
    EXPECT_EQ(*plain.message.xor_mapped, endpoint);
  }
}

TEST_F(StunRoundTrip, KeepsARoleConflict) {
  const auto conflict_bytes = encode_ok(conflict, k, true);
  const auto decoded_conflict = decode_ok(conflict_bytes);
  EXPECT_EQ(decoded_conflict.message.kind, MessageClass::error);
  ASSERT_TRUE(decoded_conflict.message.error);
  EXPECT_EQ(decoded_conflict.message.error->code, 487);
  EXPECT_EQ(decoded_conflict.message.error->reason, "Role Conflict");
  EXPECT_TRUE(decoded_conflict.verify_integrity(crypto, conflict_bytes, k));
}

TEST_F(StunRoundTrip, KeepsTheUnknownAttributesOfARefusal) {
  Message unknown;
  unknown.kind = MessageClass::error;
  unknown.transaction = transaction(34);
  unknown.error =
      ErrorResponse{error_code::unknown_attribute, "Unknown Attribute"};
  unknown.unknown_attributes = {0x7777, 0x0014, 0x0015};
  const auto decoded_unknown = decode_ok(encode_ok(unknown, k, true));
  ASSERT_TRUE(decoded_unknown.message.error);
  EXPECT_EQ(decoded_unknown.message.error->code, 420);
  EXPECT_EQ(decoded_unknown.message.unknown_attributes,
            (std::vector<std::uint16_t>{0x7777, 0x0014, 0x0015}));
}

TEST_F(StunRoundTrip, KeepsAnIndicationWithOnlyAFingerprint) {
  const auto indication_bytes = encode_ok(indication, {}, true);
  EXPECT_EQ(indication_bytes.size(), header_size + 8);
  const auto decoded_indication = decode_ok(indication_bytes);
  EXPECT_EQ(decoded_indication.message.kind, MessageClass::indication);
  EXPECT_TRUE(decoded_indication.fingerprinted);
}

TEST_F(StunRoundTrip, RefusesToEncodeAnErrorCodeOutOfRange) {
  auto& bad_error = conflict;
  bad_error.error->code = 700;
  EXPECT_FALSE(bad_error.encode(crypto, k, Fingerprint::present));
  bad_error.error->code = 299;
  EXPECT_FALSE(bad_error.encode(crypto, k, Fingerprint::present));
}

TEST_F(StunRoundTrip, RefusesToEncodeAnOverlongUsername) {
  auto& long_name = indication;
  long_name.username =
      libchiaking::SecretString{std::string(max_username_size + 1, 'a')};
  EXPECT_FALSE(long_name.encode(crypto, k, Fingerprint::present));
}

}  // namespace
}  // namespace nat_test
