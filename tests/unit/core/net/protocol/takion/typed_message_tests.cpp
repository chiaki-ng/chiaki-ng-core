#include "src/core/net/protocol/takion/detail/crypto_context.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_types.hpp"
#include "src/core/net/protocol/takion/detail/typed_message_codec.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

struct TypedMessageCryptoRecord final {
  std::vector<std::byte> authenticated;
  std::uint64_t authenticate_position{};
  std::uint64_t cipher_position{};
  std::size_t cipher_size{};
};

class TypedMessageRecordingDirection final : public takion::CryptoDirection {
 public:
  explicit TypedMessageRecordingDirection(
      std::shared_ptr<TypedMessageCryptoRecord> record)
      : record_(std::move(record)) {}

  auto apply_keystream(std::uint64_t position, std::span<const std::byte> input,
                       std::span<std::byte> bytes)
      -> takion::Expected<void, takion::CryptoError> override {
    if (input.data() != bytes.data()) std::ranges::copy(input, bytes.begin());
    record_->cipher_position = position;
    record_->cipher_size = bytes.size();
    for (auto& byte : bytes) byte ^= std::byte{0x3c};
    return {};
  }

  auto authenticate(std::uint64_t position, std::span<const std::byte> bytes)
      -> takion::Expected<takion::Mac, takion::CryptoError> override {
    record_->authenticate_position = position;
    record_->authenticated.assign(bytes.begin(), bytes.end());
    return mac(position, bytes);
  }

 private:
  static auto mac(std::uint64_t position, std::span<const std::byte> bytes)
      -> takion::Mac {
    std::uint32_t value = static_cast<std::uint32_t>(position) ^ 0x1234567U;
    for (const auto byte : bytes)
      value =
          (value << 7U) ^ (value >> 3U) ^ std::to_integer<std::uint32_t>(byte);
    return takion::TypedMessageCodec::trace_tag_bytes(value);
  }

  std::shared_ptr<TypedMessageCryptoRecord> record_;
};

class TypedMessageCryptoProvider final : public takion::CryptoProvider {
 public:
  std::shared_ptr<TypedMessageCryptoRecord> local{
      std::make_shared<TypedMessageCryptoRecord>()};
  std::shared_ptr<TypedMessageCryptoRecord> remote{
      std::make_shared<TypedMessageCryptoRecord>()};

  auto create_local(takion::CryptoMaterial)
      -> takion::Expected<std::unique_ptr<takion::CryptoDirection>,
                          takion::CryptoError> override {
    return std::make_unique<TypedMessageRecordingDirection>(local);
  }

  auto create_remote(takion::CryptoMaterial)
      -> takion::Expected<std::unique_ptr<takion::CryptoDirection>,
                          takion::CryptoError> override {
    return std::make_unique<TypedMessageRecordingDirection>(remote);
  }
};

class TypedMessageRig : public ::testing::Test {
 protected:
  static auto hex(std::string_view text) -> std::vector<std::byte> {
    const auto nibble = [](char value) -> std::uint8_t {
      if (value >= '0' && value <= '9')
        return static_cast<std::uint8_t>(value - '0');
      return static_cast<std::uint8_t>(value - 'a' + 10);
    };
    std::vector<std::byte> bytes;
    for (std::size_t index = 0; index + 1 < text.size(); index += 2)
      bytes.push_back(static_cast<std::byte>((nibble(text[index]) << 4U) |
                                             nibble(text[index + 1])));
    return bytes;
  }

  static auto profile(std::uint32_t version) -> takion::WireProfile {
    const auto resolved = takion::resolve_takion_wire_profile(version);
    EXPECT_TRUE(resolved) << "version " << version;
    return resolved.value();
  }

  static auto wire(takion::PacketFamily family, std::vector<std::byte> body,
                   std::uint32_t version) -> std::vector<std::byte> {
    const takion::PacketType type{static_cast<std::uint8_t>(family)};
    takion::PlainPacket packet{
        type,
        profile(version).uses_extended_header(type)
            ? std::optional<
                  takion::ExtendedPacketHeader>{takion::ExtendedPacketHeader{}}
            : std::nullopt,
        std::move(body)};
    auto encoded =
        takion::PacketCodec::encode_plain_packet(packet, profile(version));
    EXPECT_TRUE(encoded);
    return std::vector<std::byte>(encoded.value().begin(),
                                  encoded.value().end());
  }

  static auto parse(std::span<const std::byte> bytes, std::uint32_t version)
      -> takion::PlainPacket {
    auto decoded =
        takion::PacketCodec::decode_plain_packet(bytes, profile(version));
    EXPECT_TRUE(decoded);
    return std::move(decoded).value();
  }

  static auto test_secret() -> takion::SecretBytes {
    const std::byte value[]{std::byte{7}};
    return takion::SecretBytes{value};
  }
};

using TypedMessageFamilies = TypedMessageRig;
using LengthPrefixedCodec = TypedMessageRig;
using ClientInfoCodec = TypedMessageRig;
using TraceCodec = TypedMessageRig;
using GenericDataChunkCodec = TypedMessageRig;
using GenericDataControlCodec = TypedMessageRig;
using GenericDataCrypto = TypedMessageRig;
using DetachedAuthentication = TypedMessageRig;

TEST_F(TypedMessageFamilies, HaveFixedCodesAndExtendedHeaderRules) {
  static_assert(static_cast<std::uint8_t>(takion::PacketFamily::trace) == 0x0a);
  static_assert(static_cast<std::uint8_t>(takion::PacketFamily::generic_data) ==
                0x0c);
  EXPECT_EQ(takion::PacketType::family_from_code(0x0a),
            takion::PacketFamily::trace);
  EXPECT_EQ(takion::PacketType::family_from_code(0x0c),
            takion::PacketFamily::generic_data);
  EXPECT_EQ(takion::PacketType{0xfa}.family(), takion::PacketFamily::trace);
  EXPECT_EQ(takion::PacketType{0xfc}.family(),
            takion::PacketFamily::generic_data);
  EXPECT_FALSE(takion::PacketType::has_extended_header(0x04));
  EXPECT_FALSE(takion::PacketType::has_extended_header(0x08));
  EXPECT_TRUE(takion::PacketType::has_extended_header(0x0a));
  EXPECT_TRUE(takion::PacketType::has_extended_header(0x0c));
}

TEST_F(TypedMessageFamilies,
       GainANineByteHeaderFromVersion20OnlyForTraceAndGenericData) {
  for (const auto family :
       {takion::PacketFamily::length_prefixed,
        takion::PacketFamily::client_information, takion::PacketFamily::trace,
        takion::PacketFamily::generic_data}) {
    SCOPED_TRACE(::testing::PrintToString(family));
    const takion::PacketType type{static_cast<std::uint8_t>(family)};
    const bool extended = family == takion::PacketFamily::trace ||
                          family == takion::PacketFamily::generic_data;
    EXPECT_EQ(profile(19).packet_header_size(type), 1U);
    EXPECT_EQ(profile(20).packet_header_size(type), (extended ? 9U : 1U));
  }
}

TEST_F(TypedMessageFamilies, AreGatedByTheNativeFeatureMaskOfEachVersion) {
  EXPECT_EQ(takion::native_takion_feature_mask(9), 0x1fU);
  EXPECT_EQ(takion::native_takion_feature_mask(14), 0xffU);
  EXPECT_EQ(takion::native_takion_feature_mask(15), 0x1fffU);
  EXPECT_EQ(takion::native_takion_feature_mask(20), 0x7ffffU);
  EXPECT_EQ(takion::native_takion_feature_mask(25), 0x3fffffU);
  EXPECT_EQ(takion::native_takion_feature_mask(7), 0U);
  EXPECT_FALSE(takion::TypedMessageCodec::trace_messages_enabled(profile(7)));
  for (const std::uint32_t version : {9U, 10U, 11U, 12U, 14U, 15U, 19U, 20U}) {
    SCOPED_TRACE(::testing::PrintToString(version));
    EXPECT_TRUE(
        takion::TypedMessageCodec::trace_messages_enabled(profile(version)));
  }
  for (const std::uint32_t version : {9U, 12U, 13U, 14U}) {
    SCOPED_TRACE(::testing::PrintToString(version));
    EXPECT_FALSE(
        takion::TypedMessageCodec::generic_data_enabled(profile(version)));
  }
  for (const std::uint32_t version : {15U, 16U, 18U, 19U, 20U}) {
    SCOPED_TRACE(::testing::PrintToString(version));
    EXPECT_TRUE(
        takion::TypedMessageCodec::generic_data_enabled(profile(version)));
  }
  EXPECT_FALSE(profile(10).features().contains(
      takion::Feature::direct_message_encryption));
}

TEST_F(LengthPrefixedCodec, MatchesItsVectorsAndRejectsTruncation) {
  const auto payload = hex("000100");
  for (const std::uint32_t version : {9U, 19U, 20U}) {
    SCOPED_TRACE(::testing::PrintToString(version));
    auto body = takion::TypedMessageCodec::encode_length_prefixed_body(payload);
    ASSERT_TRUE(body);
    const auto encoded =
        wire(takion::PacketFamily::length_prefixed, body.value(), version);
    EXPECT_EQ(encoded, hex("040003000100"));
    const auto decoded = takion::TypedMessageCodec::decode_length_prefixed_body(
        parse(encoded, version).payload);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value(), payload);
  }

  for (const std::size_t size :
       {0U, 1U, 15U, 16U, 255U, 256U, 4095U, 4096U, 65535U}) {
    SCOPED_TRACE(::testing::PrintToString(size));
    std::vector<std::byte> data(size);
    for (std::size_t index = 0; index < size; ++index)
      data[index] = static_cast<std::byte>(index * 7U);
    auto body = takion::TypedMessageCodec::encode_length_prefixed_body(data);
    ASSERT_TRUE(body);
    ASSERT_EQ(body.value().size(), size + 2);
    EXPECT_EQ(std::to_integer<std::size_t>(body.value()[0]), (size >> 8U));
    EXPECT_EQ(std::to_integer<std::size_t>(body.value()[1]), (size & 0xffU));
    auto trailing = body.value();
    trailing.push_back(std::byte{0xee});
    const auto decoded =
        takion::TypedMessageCodec::decode_length_prefixed_body(trailing);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value(), data);
    if (size != 0) {
      auto shortened = body.value();
      shortened.pop_back();
      EXPECT_FALSE(
          takion::TypedMessageCodec::decode_length_prefixed_body(shortened));
    }
  }
  std::vector<std::byte> oversized(65536);
  const auto rejected =
      takion::TypedMessageCodec::encode_length_prefixed_body(oversized);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().reason,
            takion::ParseFailureReason::invalid_length);
  EXPECT_FALSE(takion::TypedMessageCodec::decode_length_prefixed_body(hex("")));
  EXPECT_FALSE(
      takion::TypedMessageCodec::decode_length_prefixed_body(hex("00")));
}

TEST_F(ClientInfoCodec, MatchesItsVectorAndRejectsMalformedMessages) {
  auto key = std::make_shared<libchiaking::SecretString>();
  key->assign("abc");
  const takion::ClientInfoValues values{key, 2U, 3U};
  auto body = takion::TypedMessageCodec::encode_client_info_message(values);
  ASSERT_TRUE(body);
  for (const std::uint32_t version : {9U, 19U, 20U}) {
    SCOPED_TRACE(::testing::PrintToString(version));
    const auto encoded =
        wire(takion::PacketFamily::client_information, body.value(), version);
    EXPECT_EQ(encoded, hex("080816ba01090a0361626310021803"));
    const auto decoded = takion::TypedMessageCodec::decode_client_info_message(
        parse(encoded, version).payload);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().session_key->view(), "abc");
    EXPECT_EQ(decoded.value().tag, 2U);
    EXPECT_EQ(decoded.value().key_position, 3U);
  }
  const auto bare = takion::TypedMessageCodec::decode_client_info_message(
      hex("0816ba01050a03616263"));
  ASSERT_TRUE(bare);
  EXPECT_FALSE(bare.value().tag);
  EXPECT_FALSE(bare.value().key_position);
  EXPECT_FALSE(
      takion::TypedMessageCodec::decode_client_info_message(hex("0803")));
  EXPECT_FALSE(
      takion::TypedMessageCodec::decode_client_info_message(hex("0816ba0100")));
  const auto full = body.value();
  for (std::size_t size = 0; size < full.size(); ++size) {
    SCOPED_TRACE(::testing::PrintToString(size));
    EXPECT_FALSE(takion::TypedMessageCodec::decode_client_info_message(
        std::span{full}.first(size)));
  }
  EXPECT_FALSE(takion::TypedMessageCodec::encode_client_info_message(
      takion::ClientInfoValues{}));
}

TEST_F(TraceCodec, MatchesItsVectorsAndClearsTheAuthenticatedForm) {
  const takion::TraceValues values{127, 128, 16384};
  auto body = takion::TypedMessageCodec::encode_trace_message(values);
  ASSERT_TRUE(body);
  EXPECT_EQ(body.value(), hex("081ad20109087f10800118808001"));
  const auto version19 = wire(takion::PacketFamily::trace, body.value(), 19);
  const auto version20 = wire(takion::PacketFamily::trace, body.value(), 20);
  EXPECT_EQ(version19, hex("0a081ad20109087f10800118808001"));
  EXPECT_EQ(version20, hex("0a0000000000000000081ad20109087f10800118808001"));
  EXPECT_EQ(version19.size(), body.value().size() + 1);
  EXPECT_EQ(version20.size(), body.value().size() + 9);
  for (const auto& [encoded, version] :
       {std::pair{version19, 19U}, std::pair{version20, 20U}}) {
    SCOPED_TRACE(::testing::PrintToString(version));
    const auto decoded = takion::TypedMessageCodec::decode_trace_message(
        parse(encoded, version).payload);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value(), values);
  }

  const takion::TraceValues maximum{0xffffffffU, 0x80000000U, 0xffffffffU};
  auto maximum_body = takion::TypedMessageCodec::encode_trace_message(maximum);
  ASSERT_TRUE(maximum_body);
  EXPECT_EQ(maximum_body.value(),
            hex("081ad2011208ffffffff0f10808080800818ffffffff0f"));
  EXPECT_EQ(
      takion::TypedMessageCodec::decode_trace_message(maximum_body.value())
          .value(),
      maximum);
  const auto zero =
      takion::TypedMessageCodec::encode_trace_message(takion::TraceValues{});
  ASSERT_TRUE(zero);
  EXPECT_EQ(zero.value(), hex("081ad20106080010001800"));

  const auto cleared = takion::TypedMessageCodec::trace_authenticated_message(
      maximum_body.value());
  ASSERT_TRUE(cleared);
  EXPECT_EQ(cleared.value(), takion::TypedMessageCodec::encode_trace_message(
                                 takion::TraceValues{0xffffffffU, 0, 0})
                                 .value());

  const auto& full = body.value();
  for (std::size_t size = 0; size < full.size(); ++size) {
    SCOPED_TRACE(::testing::PrintToString(size));
    EXPECT_FALSE(takion::TypedMessageCodec::decode_trace_message(
        std::span{full}.first(size)));
    EXPECT_FALSE(takion::TypedMessageCodec::trace_authenticated_message(
        std::span{full}.first(size)));
  }
  EXPECT_FALSE(takion::TypedMessageCodec::decode_trace_message(hex("0803")));
  EXPECT_FALSE(takion::TypedMessageCodec::decode_trace_message(
      hex("081ad20104087f1080")));

  const takion::Mac mac{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  EXPECT_EQ(takion::TypedMessageCodec::trace_tag_value(mac), 0x04030201U);
  EXPECT_EQ(takion::TypedMessageCodec::trace_tag_bytes(0x04030201U), mac);
}

TEST_F(GenericDataChunkCodec,
       MatchesItsVectorsForEachVersionAndRejectsTruncation) {
  takion::GenericDataChunk chunk{
      {std::byte{0xde}, std::byte{0xad}, std::byte{0xbe}, std::byte{0xef}},
      0x01020304U,
      0x0a0b0c0dU,
      7,
      0x208U,
      libchiaking::SecretBytes{hex("aabbcc")}};
  const auto expected19 =
      hex("0cdeadbeef010203040a0b0c0d070000000300000208aabbcc");
  const auto expected20 =
      hex("0c0000000000000000deadbeef010203040a0b0c0d070000000300000208aabbcc");
  for (const auto& [version, expected] :
       {std::pair{15U, expected19}, std::pair{19U, expected19},
        std::pair{20U, expected20}}) {
    SCOPED_TRACE(::testing::PrintToString(version));
    auto body = takion::TypedMessageCodec::encode_generic_data_chunk_body(
        chunk, profile(version));
    ASSERT_TRUE(body);
    const auto encoded =
        wire(takion::PacketFamily::generic_data,
             std::vector<std::byte>(body.value().begin(), body.value().end()),
             version);
    EXPECT_EQ(encoded, expected);
    const auto decoded = takion::TypedMessageCodec::decode_generic_data_chunk(
        parse(encoded, version), profile(version));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value(), chunk);
    EXPECT_EQ(encoded.size() - chunk.payload.size(),
              (version >= 20 ? 30U : takion::generic_data_header_size));

    const auto header = profile(version).packet_header_size(takion::PacketType{
        static_cast<std::uint8_t>(takion::PacketFamily::generic_data)});
    for (std::size_t size = header; size < encoded.size(); ++size) {
      SCOPED_TRACE(::testing::PrintToString(size));
      EXPECT_FALSE(takion::TypedMessageCodec::decode_generic_data_chunk(
          parse(std::span{encoded}.first(size), version), profile(version)));
    }
    auto trailing = encoded;
    trailing.push_back(std::byte{0});
    const auto mismatch = takion::TypedMessageCodec::decode_generic_data_chunk(
        parse(trailing, version), profile(version));
    ASSERT_FALSE(mismatch);
    EXPECT_EQ(mismatch.error().reason,
              takion::ParseFailureReason::invalid_length);
  }

  const auto body = takion::TypedMessageCodec::encode_generic_data_chunk_body(
      chunk, profile(15));
  const auto encoded = wire(
      takion::PacketFamily::generic_data,
      std::vector<std::byte>(body.value().begin(), body.value().end()), 14);
  const auto old = takion::TypedMessageCodec::decode_generic_data_chunk(
      parse(encoded, 14), profile(14));
  ASSERT_FALSE(old);
  EXPECT_EQ(old.error().reason, takion::ParseFailureReason::unsupported_layout);
  EXPECT_FALSE(takion::TypedMessageCodec::encode_generic_data_chunk_body(
      chunk, profile(14)));

  auto wrong = parse(expected19, 19);
  wrong.type = takion::PacketType{0x05};
  EXPECT_FALSE(
      takion::TypedMessageCodec::decode_generic_data_chunk(wrong, profile(19)));
  auto flagged = parse(expected19, 19);
  flagged.type = takion::PacketType{0xfc};
  EXPECT_TRUE(takion::TypedMessageCodec::decode_generic_data_chunk(
      flagged, profile(19)));
}

TEST_F(GenericDataControlCodec, MatchesItsVectors) {
  const takion::GenericDataControl announce{5,  0,  1000U, std::string{"m"},
                                            2U, {}, {}};
  auto payload =
      takion::TypedMessageCodec::encode_generic_data_payload(announce);
  ASSERT_TRUE(payload);
  EXPECT_EQ(payload.value(), hex("0805100018e80722016d28023200"));
  auto heard = announce;
  heard.error = std::string{};
  EXPECT_EQ(
      takion::TypedMessageCodec::decode_generic_data_payload(payload.value())
          .value(),
      heard);

  auto direct =
      takion::TypedMessageCodec::encode_generic_data_direct_message(announce);
  ASSERT_TRUE(direct);
  EXPECT_EQ(direct.value(),
            hex("081dea0115080210e9071a0e0805100018e80722016d28023200"));
  const auto routed =
      takion::TypedMessageCodec::decode_generic_data_direct_message(
          direct.value());
  ASSERT_TRUE(routed);
  ASSERT_TRUE(routed.value());
  EXPECT_EQ(*routed.value(), heard);
  EXPECT_EQ(takion::TypedMessageCodec::direct_message_data_size(direct.value()),
            14U);
  EXPECT_EQ(takion::TypedMessageCodec::direct_message_data_size(hex("0803")),
            0U);
  EXPECT_EQ(takion::TypedMessageCodec::direct_message_data_size(
                hex("081dea0105080210e907")),
            0U);
  EXPECT_EQ(takion::TypedMessageCodec::direct_message_data_size(
                hex("081dea0108080010e8071a01ff")),
            1U);

  const takion::GenericDataControl missing{5, 5, {}, {}, {}, {}, {1, 3}};
  EXPECT_EQ(
      takion::TypedMessageCodec::encode_generic_data_payload(missing).value(),
      hex("08051005320038013803"));
  EXPECT_EQ(takion::TypedMessageCodec::decode_generic_data_payload(
                hex("0805100538013803"))
                .value(),
            missing);
  const takion::GenericDataControl rejected{9, 2, {}, {}, {}, std::string{"no"},
                                            {}};
  EXPECT_EQ(takion::TypedMessageCodec::decode_generic_data_payload(
                takion::TypedMessageCodec::encode_generic_data_payload(rejected)
                    .value())
                .value(),
            rejected);

  const auto heartbeat =
      takion::TypedMessageCodec::decode_generic_data_direct_message(
          hex("0803"));
  ASSERT_TRUE(heartbeat);
  EXPECT_FALSE(heartbeat.value());
  const auto microphone =
      takion::TypedMessageCodec::decode_generic_data_direct_message(
          hex("081dea0105080110e907"));
  ASSERT_TRUE(microphone);
  EXPECT_FALSE(microphone.value());
  EXPECT_FALSE(takion::TypedMessageCodec::decode_generic_data_direct_message(
      hex("081dea0105080210e907")));
  EXPECT_FALSE(takion::TypedMessageCodec::decode_generic_data_direct_message(
      hex("081dea0108080210e9071a01ff")));
  EXPECT_FALSE(
      takion::TypedMessageCodec::decode_generic_data_payload(hex("1000")));
  EXPECT_FALSE(
      takion::TypedMessageCodec::decode_generic_data_payload(hex("0805")));
}

TEST_F(GenericDataCrypto, KeepsTheKeyPositionInTheAuthenticatedBytes) {
  for (const std::uint32_t version : {15U, 20U}) {
    SCOPED_TRACE(::testing::PrintToString(version));
    TypedMessageCryptoProvider provider;
    takion::CryptoContext sender{takion::CryptoPolicy::required};
    takion::CryptoContext receiver{takion::CryptoPolicy::required};
    EXPECT_TRUE(
        sender.install_local(provider, takion::CryptoMaterial{test_secret()}));
    EXPECT_TRUE(receiver.install_remote(provider,
                                        takion::CryptoMaterial{test_secret()}));
    EXPECT_TRUE(sender.set_local_key_position(0x2000));
    receiver.set_remote_key_position(0x1f00);

    const auto offsets = takion::CryptoContext::packet_offsets(
        takion::PacketFamily::generic_data, profile(version));
    const std::size_t shift = version >= 20 ? 8 : 0;
    ASSERT_TRUE(offsets);
    EXPECT_EQ(offsets.value().mac, 1 + shift);
    EXPECT_EQ(offsets.value().key_position_low, 5 + shift);
    EXPECT_FALSE(offsets.value().zero_key_position_for_authentication);
    EXPECT_EQ(offsets.value().cipher_key_position_addition, 16U);

    const takion::GenericDataChunk chunk{
        {}, 0, 0x11, 1, 519, libchiaking::SecretBytes{hex("0102030405")}};
    const auto body = takion::TypedMessageCodec::encode_generic_data_chunk_body(
        chunk, profile(version));
    auto packet =
        wire(takion::PacketFamily::generic_data,
             std::vector<std::byte>(body.value().begin(), body.value().end()),
             version);
    const auto header = takion::generic_data_header_size + shift;
    auto reservation = sender.protect_local(
        packet, takion::PacketFamily::generic_data, profile(version),
        takion::PacketCryptoCoverage{0, packet.size(), header,
                                     chunk.payload.size()},
        chunk.payload.size() + 16);
    ASSERT_TRUE(reservation);
    EXPECT_TRUE(reservation.value().commit());
    EXPECT_EQ(provider.local->cipher_position, 0x2010U);
    EXPECT_EQ(provider.local->cipher_size, chunk.payload.size());
    EXPECT_EQ(provider.local->authenticate_position, 0x2000U);
    const auto& aad = provider.local->authenticated;
    ASSERT_EQ(aad.size(), packet.size());
    for (std::size_t index = 0; index < 4; ++index) {
      SCOPED_TRACE(::testing::PrintToString(index));
      EXPECT_EQ(aad[1 + shift + index], std::byte{});
    }
    EXPECT_EQ((std::vector<std::byte>{
                  aad.begin() + static_cast<std::ptrdiff_t>(5 + shift),
                  aad.begin() + static_cast<std::ptrdiff_t>(9 + shift)}),
              hex("00002000"));
    EXPECT_EQ(packet[header], (std::byte{1} ^ std::byte{0x3c}));

    auto tampered = packet;
    tampered[8 + shift] ^= std::byte{0x10};
    EXPECT_EQ(
        receiver
            .authenticate_and_decrypt_remote(
                tampered, takion::PacketFamily::generic_data, profile(version),
                takion::PacketCryptoCoverage{0, tampered.size(),
                                             tampered.size(), 0})
            .error(),
        takion::CryptoError::authentication_failed);

    const auto position = receiver.authenticate_and_decrypt_remote(
        packet, takion::PacketFamily::generic_data, profile(version),
        takion::PacketCryptoCoverage{0, packet.size(), packet.size(), 0});
    ASSERT_TRUE(position);
    EXPECT_EQ(position.value(), 0x2000U);
    auto decoded = takion::TypedMessageCodec::decode_generic_data_chunk(
        parse(packet, version), profile(version));
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(receiver.decrypt_remote_payload(
        decoded.value().payload.span(), decoded.value().payload.span(),
        takion::PacketFamily::generic_data, profile(version),
        position.value()));
    EXPECT_TRUE(std::ranges::equal(decoded.value().payload.bytes(),
                                   chunk.payload.bytes()));
    EXPECT_EQ(decoded.value().key_position, 0x2000U);
    EXPECT_EQ(provider.remote->cipher_position, 0x2010U);
  }
  EXPECT_FALSE(takion::CryptoContext::packet_offsets(
      takion::PacketFamily::generic_data, profile(14)));
  for (const auto family : {takion::PacketFamily::length_prefixed,
                            takion::PacketFamily::client_information,
                            takion::PacketFamily::trace}) {
    SCOPED_TRACE(::testing::PrintToString(family));
    EXPECT_FALSE(takion::CryptoContext::packet_offsets(family, profile(20)));
  }
}

TEST_F(DetachedAuthentication, RoundTripsAndGuardsItsReservation) {
  TypedMessageCryptoProvider provider;
  takion::CryptoContext sender{takion::CryptoPolicy::required};
  takion::CryptoContext receiver{takion::CryptoPolicy::required};
  EXPECT_FALSE(sender.authenticate_local_detached(hex("0a"), 16));
  EXPECT_TRUE(
      sender.install_local(provider, takion::CryptoMaterial{test_secret()}));
  EXPECT_TRUE(
      receiver.install_remote(provider, takion::CryptoMaterial{test_secret()}));
  EXPECT_TRUE(sender.set_local_key_position(0x30));

  const auto aad = wire(takion::PacketFamily::trace,
                        takion::TypedMessageCodec::encode_trace_message(
                            takion::TraceValues{9, 0, 0})
                            .value(),
                        20);
  auto authenticated =
      sender.authenticate_local_detached(aad, takion::trace_key_advance);
  ASSERT_TRUE(authenticated);
  EXPECT_EQ(authenticated.value().reservation.key_position(), 0x30U);
  EXPECT_EQ(provider.local->authenticated, aad);
  EXPECT_EQ(provider.local->authenticate_position, 0x30U);
  EXPECT_EQ(sender.authenticate_local_detached(aad, 16).error(),
            takion::CryptoError::reservation_in_progress);
  EXPECT_TRUE(authenticated.value().reservation.commit());
  EXPECT_EQ(sender.local_key_position(), 0x40U);

  const auto mac = authenticated.value().mac;
  const auto verified = receiver.verify_remote_detached(aad, 0x30, mac);
  ASSERT_TRUE(verified);
  EXPECT_EQ(verified.value(), 0x30U);
  EXPECT_EQ(receiver.remote_key_position(), 0x30U);
  auto damaged = mac;
  damaged[0] ^= std::byte{1};
  EXPECT_EQ(receiver.verify_remote_detached(aad, 0x40, damaged).error(),
            takion::CryptoError::authentication_failed);
  EXPECT_EQ(receiver.remote_key_position(), 0x30U);
  EXPECT_EQ(receiver.verify_remote_detached(aad, 0x31, mac).error(),
            takion::CryptoError::authentication_failed);

  takion::CryptoContext disabled{takion::CryptoPolicy::disabled};
  auto skipped = disabled.authenticate_local_detached(aad, 16);
  ASSERT_TRUE(skipped);
  EXPECT_TRUE(skipped.value().reservation.commit());
  EXPECT_EQ(disabled.local_key_position(), 0U);
}

}  // namespace
}  // namespace takion_test
