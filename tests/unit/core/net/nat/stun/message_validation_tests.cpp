#include "tests/support/bytes.hpp"
#include "tests/unit/core/net/nat/stun/message_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nat_test {
namespace {

using namespace libchiaking::net::nat::stun;

class StunMessageLength : public StunMessageRig {};

TEST_F(StunMessageLength, RefusesEveryTruncatedPrefix) {
  const auto bytes = signed_request();
  for (std::size_t size = 0; size < bytes.size(); ++size)
    EXPECT_TRUE(refused(std::span<const std::byte>{bytes.data(), size}))
        << size;
}

TEST_F(StunMessageLength, RefusesTrailingBytesBeyondTheLength) {
  auto longer = signed_request();
  longer.insert(longer.end(), 4, std::byte{});
  EXPECT_TRUE(refused(longer));
}

TEST_F(StunMessageLength, RefusesALengthShorterThanTheBody) {
  auto shorter_header = signed_request();
  shorter_header[3] = static_cast<std::byte>(
      std::to_integer<std::uint8_t>(shorter_header[3]) - 4);
  EXPECT_TRUE(refused(shorter_header));
}

TEST_F(StunMessageLength, RefusesALengthThatIsNotFourByteAligned) {
  auto unaligned = signed_request();
  unaligned[3] =
      static_cast<std::byte>(std::to_integer<std::uint8_t>(unaligned[3]) - 1);
  unaligned.pop_back();
  EXPECT_TRUE(refused(unaligned));
}

class StunMessageHeader : public StunMessageRig {};

TEST_F(StunMessageHeader, RefusesAWrongMagicCookie) {
  auto bad_cookie = signed_request();
  bad_cookie[5] ^= std::byte{0x01};
  EXPECT_TRUE(refused(bad_cookie));
}

TEST_F(StunMessageHeader, RefusesTheTopBitsSet) {
  auto top_bits = signed_request();
  top_bits[0] |= std::byte{0x40};
  EXPECT_TRUE(refused(top_bits));
}

TEST_F(StunMessageHeader, DecodesOnlyBindingMessages) {
  EXPECT_FALSE(ReceivedMessage::decode(raw_message(0x0003, {})));
  EXPECT_TRUE(ReceivedMessage::decode(raw_message(0x0001, {})));
}

TEST_F(StunMessageHeader, RefusesAMessageBeyondTheMaximumSize) {
  std::vector<std::byte> oversize(max_message_size + 4);
  const auto username =
      std::vector<std::byte>(oversize.size() - header_size - 4, std::byte{'a'});
  const auto big =
      raw_message(0x0001, {RawAttribute{attribute::username, username, 0}});
  EXPECT_EQ(big.size(), max_message_size + 4);
  EXPECT_TRUE(refused(big));
}

class StunMessagePadding : public StunMessageRig {};

TEST_F(StunMessagePadding, RefusesAnUnpaddedAttribute) {
  const auto unpadded = raw_message(
      0x0001, {RawAttribute{attribute::username,
                            test_support::Bytes::of("abcdefghi"), 0}});
  EXPECT_TRUE(refused(unpadded));
}

TEST_F(StunMessagePadding, RefusesAnAttributeThatOverrunsTheMessage) {
  auto overrun = raw_message(
      0x0001, {RawAttribute{attribute::username,
                            test_support::Bytes::of("abcdefgh"), 0}});
  overrun[header_size + 3] = std::byte{9};
  EXPECT_FALSE(ReceivedMessage::decode(overrun));
}

TEST_F(StunMessagePadding, IgnoresWhatThePaddingHolds) {
  auto spaces = raw_message(
      0x0001, {RawAttribute{attribute::username,
                            test_support::Bytes::of("abcdefghi"), 3}});
  spaces[spaces.size() - 1] = std::byte{0x20};
  spaces[spaces.size() - 2] = std::byte{0x20};
  spaces[spaces.size() - 3] = std::byte{0x20};
  const auto decoded = decode_ok(spaces);
  ASSERT_TRUE(decoded.message.username);
  EXPECT_EQ(decoded.message.username->view(), "abcdefghi");
}

class StunMessageFingerprint : public StunMessageRig {};

TEST_F(StunMessageFingerprint, RefusesAnAttributeAfterTheFingerprint) {
  auto after = signed_request();
  const std::array<std::byte, 8> trailing{
      std::byte{0x80}, std::byte{0x22}, std::byte{0x00}, std::byte{0x04},
      std::byte{'a'},  std::byte{'b'},  std::byte{'c'},  std::byte{'d'}};
  after.insert(after.end(), trailing.begin(), trailing.end());
  after[3] =
      static_cast<std::byte>(std::to_integer<std::uint8_t>(after[3]) + 8);
  EXPECT_FALSE(ReceivedMessage::decode(after));
}

TEST_F(StunMessageFingerprint, RefusesAWrongChecksum) {
  auto wrong_crc = signed_request();
  wrong_crc.back() ^= std::byte{0x01};
  EXPECT_FALSE(ReceivedMessage::decode(wrong_crc));
}

TEST_F(StunMessageFingerprint, RefusesASecondFingerprint) {
  auto two = signed_request();
  Message::append_fingerprint(two);
  EXPECT_FALSE(ReceivedMessage::decode(two));
}

TEST_F(StunMessageFingerprint, RefusesAShortFingerprint) {
  const auto short_fingerprint =
      raw_message(0x0001, {RawAttribute{attribute::fingerprint,
                                        test_support::Bytes::of("abc"), 1}});
  EXPECT_FALSE(ReceivedMessage::decode(short_fingerprint));
}

class StunMessageIntegrity : public StunMessageRig {
 protected:
  void SetUp() override {
    message.kind = MessageClass::request;
    message.transaction = transaction(5);
    message.username = libchiaking::SecretString{"abcd:efgh"};
    message.priority = 7;
    message.ice_controlling = 1;
  }

  const std::vector<std::byte> k = key();
  Message message;
};

TEST_F(StunMessageIntegrity, IgnoresAttributesAfterIt) {
  auto bytes = encode_ok(message, k, false);
  const std::array<std::byte, 4> use_candidate{
      std::byte{0x00}, std::byte{0x25}, std::byte{0x00}, std::byte{0x00}};
  bytes.insert(bytes.end(), use_candidate.begin(), use_candidate.end());
  bytes[3] =
      static_cast<std::byte>(std::to_integer<std::uint8_t>(bytes[3]) + 4);
  Message::append_fingerprint(bytes);
  const auto decoded = decode_ok(bytes);
  EXPECT_FALSE(decoded.message.use_candidate);
  EXPECT_TRUE(decoded.fingerprinted);
  EXPECT_TRUE(decoded.verify_integrity(crypto, bytes, k));
}

TEST_F(StunMessageIntegrity, FailsWhenTheMessageIsUnsigned) {
  const auto plain = decode_ok(encode_ok(message, {}, true));
  EXPECT_FALSE(plain.verify_integrity(crypto, encode_ok(message, {}, true), k));
}

TEST_F(StunMessageIntegrity, FailsWithAnEmptyOrDifferentKey) {
  const auto signed_bytes = encode_ok(message, k, true);
  const auto signed_decoded = decode_ok(signed_bytes);
  EXPECT_FALSE(signed_decoded.verify_integrity(crypto, signed_bytes, {}));
  EXPECT_FALSE(signed_decoded.verify_integrity(
      crypto, signed_bytes, test_support::Bytes::of("other")));
}

TEST_F(StunMessageIntegrity, RefusesAShortIntegrityAttribute) {
  const auto short_integrity =
      raw_message(0x0001, {RawAttribute{attribute::message_integrity,
                                        std::vector<std::byte>(19), 1}});
  EXPECT_FALSE(ReceivedMessage::decode(short_integrity));
}

TEST_F(StunMessageIntegrity, CatchesEveryFlippedBit) {
  for (const bool fingerprint : {true, false}) {
    Message flipped_message;
    flipped_message.kind = MessageClass::request;
    flipped_message.transaction = transaction(11);
    flipped_message.username = libchiaking::SecretString{"abcd:efghi"};
    flipped_message.priority = 0x7e00ffffU;
    flipped_message.ice_controlled = 0x1111222233334444ULL;
    const auto bytes = encode_ok(flipped_message, k, fingerprint);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      for (unsigned bit = 0; bit < 8; ++bit) {
        auto flipped = bytes;
        flipped[index] ^= static_cast<std::byte>(1U << bit);
        const auto decoded = ReceivedMessage::decode(flipped);
        if (!decoded) continue;
        EXPECT_TRUE(!decoded.value().verify_integrity(crypto, flipped, k) ||
                    (fingerprint && !decoded.value().fingerprinted))
            << "byte " << index << " bit " << bit << " fingerprint "
            << fingerprint;
      }
    }
  }
}

class StunMessageAttributes : public StunMessageRig {};

TEST_F(StunMessageAttributes, RefusesValuesOfTheWrongSize) {
  EXPECT_FALSE(ReceivedMessage::decode(raw_message(
      0x0001,
      {RawAttribute{attribute::priority, std::vector<std::byte>(3), 1}})));
  EXPECT_FALSE(ReceivedMessage::decode(raw_message(
      0x0101,
      {RawAttribute{attribute::xor_mapped_address,
                    {std::byte{0}, std::byte{2}, std::byte{0}, std::byte{1},
                     std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}},
                    0}})));
  EXPECT_FALSE(ReceivedMessage::decode(raw_message(
      0x0001,
      {RawAttribute{attribute::use_candidate, std::vector<std::byte>(4), 0}})));
  EXPECT_FALSE(ReceivedMessage::decode(
      raw_message(0x0001, {RawAttribute{attribute::ice_controlling,
                                        std::vector<std::byte>(4), 0}})));
}

TEST_F(StunMessageAttributes, RefusesAnUnknownAddressFamily) {
  EXPECT_FALSE(ReceivedMessage::decode(raw_message(
      0x0101,
      {RawAttribute{attribute::xor_mapped_address,
                    {std::byte{0}, std::byte{3}, std::byte{0}, std::byte{1},
                     std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}},
                    0}})));
}

TEST_F(StunMessageAttributes, RefusesAnErrorCodeOutOfRange) {
  EXPECT_FALSE(ReceivedMessage::decode(raw_message(
      0x0111,
      {RawAttribute{attribute::error_code,
                    {std::byte{0}, std::byte{0}, std::byte{7}, std::byte{0}},
                    0}})));
  EXPECT_FALSE(ReceivedMessage::decode(raw_message(
      0x0111,
      {RawAttribute{attribute::error_code,
                    {std::byte{0}, std::byte{0}, std::byte{4}, std::byte{100}},
                    0}})));
}

TEST_F(StunMessageAttributes, ReadsAPlainMappedAddressApartFromTheXorOne) {
  const auto mapped = raw_message(
      0x0101, {RawAttribute{
                  attribute::mapped_address,
                  {std::byte{0}, std::byte{1}, std::byte{0x0d}, std::byte{0x96},
                   std::byte{198}, std::byte{51}, std::byte{100}, std::byte{9}},
                  0}});
  const auto decoded_mapped = decode_ok(mapped);
  ASSERT_TRUE(decoded_mapped.message.mapped);
  EXPECT_EQ(*decoded_mapped.message.mapped, v4(198, 51, 100, 9, 3478));
  EXPECT_FALSE(decoded_mapped.message.xor_mapped);
}

TEST_F(StunMessageAttributes, ListsOnlyUnknownRequiredAttributes) {
  const auto unknown = raw_message(
      0x0001,
      {RawAttribute{0x0014, test_support::Bytes::of("realm"), 3},
       RawAttribute{0xc001, test_support::Bytes::of("opt"), 1},
       RawAttribute{attribute::software, test_support::Bytes::of("sw"), 2}});
  const auto decoded_unknown = decode_ok(unknown);
  EXPECT_EQ(decoded_unknown.unknown_required,
            std::vector<std::uint16_t>{0x0014});
}

TEST_F(StunMessageAttributes, KeepsTheFirstOfARepeatedAttribute) {
  const auto duplicate = raw_message(
      0x0001,
      {RawAttribute{attribute::priority,
                    {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}},
                    0},
       RawAttribute{attribute::priority,
                    {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{2}},
                    0}});
  const auto decoded_duplicate = decode_ok(duplicate);
  ASSERT_TRUE(decoded_duplicate.message.priority);
  EXPECT_EQ(*decoded_duplicate.message.priority, 1U);
}

class StunMessageClassifier : public StunMessageRig {};

TEST_F(StunMessageClassifier, RecognisesBindingMessages) {
  EXPECT_TRUE(Message::recognises(signed_request()));
  EXPECT_TRUE(Message::recognises(raw_message(0x0001, {})));
}

TEST_F(StunMessageClassifier, RejectsTheFirstBytesOfOtherProtocols) {
  for (unsigned first = 20; first <= 63; ++first) {
    auto record = raw_message(0x0001, {});
    record[0] = static_cast<std::byte>(first);
    EXPECT_TRUE(refused(record)) << first;
  }
}

TEST_F(StunMessageClassifier, RejectsKakusanRecordsAndEmptyDatagrams) {
  auto rcpm = test_support::Bytes::of("RCPM");
  rcpm.resize(64, std::byte{0x5a});
  EXPECT_TRUE(refused(rcpm));
  auto disguised = raw_message(0x0001, {});
  disguised[0] = std::byte{'R'};
  disguised[1] = std::byte{'C'};
  EXPECT_FALSE(Message::recognises(disguised));
  EXPECT_FALSE(Message::recognises(std::span<const std::byte>{}));
}

}  // namespace
}  // namespace nat_test
