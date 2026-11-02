#pragma once

#include "src/core/net/protocol/remote_play/stream/active_handshake.hpp"
#include "src/core/net/protocol/takion/control_codec.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace remote_play_test {

namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;

class HandshakeKeyPair final : public platform::EcdhKeyPair {
 public:
  explicit HandshakeKeyPair(unsigned* sequence = nullptr)
      : sequence_(sequence) {}

  auto public_coordinates(std::span<std::byte>) const
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  auto derive_secret(std::span<const std::byte> coordinates,
                     std::span<std::byte> secret) const
      -> libchiaking::Expected<void, platform::CryptoError> override {
    ++derive_calls;
    if (sequence_ != nullptr) derive_order = ++*sequence_;
    last_coordinates.assign(coordinates.begin(), coordinates.end());
    if (fail)
      return libchiaking::unexpected(platform::CryptoError::backend_failure);
    for (std::size_t i = 0; i < secret.size(); ++i)
      secret[i] = static_cast<std::byte>(i + 1);
    return {};
  }

  mutable unsigned derive_calls{};
  mutable unsigned derive_order{};
  mutable std::vector<std::byte> last_coordinates;
  bool fail{false};

 private:
  unsigned* sequence_;
};

class HandshakeCrypto final : public platform::CryptoProvider {
 public:
  explicit HandshakeCrypto(unsigned* sequence = nullptr)
      : sequence_(sequence) {}

  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  auto hmac_sha256(std::span<const std::byte> key,
                   std::span<const std::byte> input,
                   std::span<std::byte> digest)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    ++hmac_calls;
    if (sequence_ != nullptr) hmac_order = ++*sequence_;
    last_key.assign(key.begin(), key.end());
    last_input.assign(input.begin(), input.end());
    if (expected_key_size) {
      EXPECT_EQ(key.size(), *expected_key_size);
    }
    if (expected_input_size) {
      EXPECT_EQ(input.size(), *expected_input_size);
    }
    if (fail)
      return libchiaking::unexpected(platform::CryptoError::backend_failure);
    EXPECT_EQ(digest.size(), 32U);
    for (std::size_t i = 0; i < digest.size(); ++i)
      digest[i] = static_cast<std::byte>(0xa0 + i);
    return {};
  }

  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  unsigned hmac_calls{};
  unsigned hmac_order{};
  bool fail{false};
  std::optional<std::size_t> expected_key_size;
  std::optional<std::size_t> expected_input_size;
  std::vector<std::byte> last_key;
  std::vector<std::byte> last_input;

 private:
  unsigned* sequence_;
};

class ActiveHandshakeRig {
 public:
  static constexpr std::string_view sent_key{"session-id"};

  static auto secret(std::string_view text)
      -> std::shared_ptr<const libchiaking::SecretString> {
    return std::make_shared<const libchiaking::SecretString>(text);
  }

  static auto shared_secret(std::size_t size) -> remote_play::EcdhSharedSecret {
    remote_play::EcdhSharedSecret result;
    result.size = size;
    for (std::size_t i = 0; i < std::min(size, result.secret.size()); ++i)
      result.secret[i] = static_cast<std::byte>(i + 1);
    return result;
  }

  static auto curve_for(std::uint32_t version) -> platform::EcdhCurve {
    return version == 20 ? platform::EcdhCurve::secp521r1
                         : platform::EcdhCurve::secp256k1;
  }

  static auto prepared(platform::EcdhCurve curve, unsigned* sequence = nullptr)
      -> remote_play::PreparedEcdh {
    remote_play::PreparedEcdh result{
        curve, std::make_unique<HandshakeKeyPair>(sequence), {}};
    result.public_key.size = platform::ecdh_public_coordinates_size(curve) + 1;
    result.public_key.bytes[0] = std::byte{0x04};
    for (std::size_t i = 1; i < result.public_key.size; ++i)
      result.public_key.bytes[i] = static_cast<std::byte>(i - 1);
    for (std::size_t i = 0; i < result.public_key.signature.size(); ++i)
      result.public_key.signature[i] = static_cast<std::byte>(0xa0 + i);
    return result;
  }

  static auto without_public_key(platform::EcdhCurve curve)
      -> remote_play::PreparedEcdh {
    return remote_play::PreparedEcdh{
        curve, std::make_unique<HandshakeKeyPair>(), {}};
  }

  static auto key_pair_of(const remote_play::PreparedEcdh& prepared)
      -> const HandshakeKeyPair& {
    return static_cast<const HandshakeKeyPair&>(*prepared.key_pair);
  }

  static auto key_pair_of(remote_play::PreparedEcdh& prepared)
      -> HandshakeKeyPair& {
    return static_cast<HandshakeKeyPair&>(*prepared.key_pair);
  }

  static auto bang(std::size_t coordinates_size = 64) -> takion::Bang {
    takion::Bang result;
    result.version_accepted = true;
    result.encrypted_key_accepted = true;
    result.session_key =
        libchiaking::net::protocol::takion::SharedSecretText{sent_key};
    result.ecdh_pub_key = std::vector<std::byte>(coordinates_size + 1);
    result.ecdh_pub_key->at(0) = std::byte{0x04};
    for (std::size_t i = 0; i < coordinates_size; ++i)
      result.ecdh_pub_key->at(i + 1) = static_cast<std::byte>(i);
    result.ecdh_sig = std::vector<std::byte>(32);
    for (std::size_t i = 0; i < 32; ++i)
      result.ecdh_sig->at(i) = static_cast<std::byte>(0xa0 + i);
    return result;
  }

  static auto owner(std::uint32_t version) -> remote_play::ActiveHandshake {
    const auto profile = takion::resolve_takion_wire_profile(version);
    EXPECT_TRUE(profile) << "version " << version;
    return {prepared(curve_for(version)), profile.value(), secret("session-id"),
            secret("launch-spec")};
  }

  static auto committed(unsigned* sequence) -> remote_play::ActiveHandshake {
    const auto profile = takion::resolve_takion_wire_profile(12);
    EXPECT_TRUE(profile);
    remote_play::ActiveHandshake value{
        prepared(platform::EcdhCurve::secp256k1, sequence), profile.value(),
        secret("session-id"), secret("encrypted-launch-spec")};
    value.on_connected();
    const auto big = value.on_profile_committed(12);
    EXPECT_TRUE(big);
    EXPECT_TRUE(big && big.value());
    return value;
  }

  static auto stream_info_with_width(std::uint32_t width)
      -> takion::StreamInfo {
    takion::StreamInfo info;
    info.resolutions.push_back(takion::StreamInfoResolution{width, 720, {}});
    info.audio_header.assign(14, std::byte{0});
    info.audio_header[0] = std::byte{2};
    info.audio_header[1] = std::byte{16};
    return info;
  }

  static auto awaiting_stream_info(takion::CryptoProvider& provider,
                                   std::span<const std::byte> key)
      -> remote_play::ActiveHandshake {
    auto owner_value = owner(12);
    owner_value.on_connected();
    EXPECT_TRUE(owner_value.on_profile_committed(12));
    EXPECT_TRUE(owner_value.on_bang(takion::Bang{}));
    EXPECT_TRUE(owner_value.on_bang_verified(provider, key, shared_secret(32)));
    EXPECT_FALSE(owner_value.on_crypto_installed());
    EXPECT_EQ(owner_value.state(),
              remote_play::ActiveHandshakeState::awaiting_stream_info);
    return owner_value;
  }
};

}  // namespace remote_play_test
