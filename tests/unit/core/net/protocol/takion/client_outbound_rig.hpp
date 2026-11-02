#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/net/protocol/takion/client/outbound.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "tests/support/clock.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace takion_test {

class OutboundTransport final
    : public libchiaking::net::transport::datagram::DatagramTransport {
 public:
  std::vector<libchiaking::net::transport::datagram::OwnedDatagram> sent;
  bool short_send{};

  auto send(std::span<const std::byte> datagram)
      -> libchiaking::net::protocol::takion::Expected<
          std::size_t,
          libchiaking::net::transport::datagram::TransportError> override {
    sent.emplace_back(datagram.begin(), datagram.end());
    return short_send ? sent.back().size() - 1 : sent.back().size();
  }

  auto receive() -> libchiaking::net::protocol::takion::Expected<
      std::optional<libchiaking::net::transport::datagram::Datagram>,
      libchiaking::net::transport::datagram::TransportError> override {
    return std::optional<libchiaking::net::transport::datagram::Datagram>{};
  }

  auto close() -> libchiaking::net::protocol::takion::Expected<
      void, libchiaking::net::transport::datagram::TransportError> override {
    return {};
  }
};

class OutboundCountingLocal final
    : public libchiaking::net::protocol::takion::CryptoDirection {
 public:
  auto apply_keystream(std::uint64_t position, std::span<const std::byte> input,
                       std::span<std::byte> bytes)
      -> libchiaking::net::protocol::takion::Expected<
          void, libchiaking::net::protocol::takion::CryptoError> override {
    if (input.data() != bytes.data()) std::ranges::copy(input, bytes.begin());
    for (auto& byte : bytes) byte ^= static_cast<std::byte>(position);
    return {};
  }

  auto authenticate(std::uint64_t position, std::span<const std::byte>)
      -> libchiaking::net::protocol::takion::Expected<
          libchiaking::net::protocol::takion::Mac,
          libchiaking::net::protocol::takion::CryptoError> override {
    return libchiaking::net::protocol::takion::Mac{
        std::byte{0xa0}, std::byte{0xb0}, std::byte{0xc0},
        static_cast<std::byte>(position)};
  }
};

class OutboundCountingProvider final
    : public libchiaking::net::protocol::takion::CryptoProvider {
 public:
  auto create_local(libchiaking::net::protocol::takion::CryptoMaterial)
      -> libchiaking::net::protocol::takion::Expected<
          std::unique_ptr<libchiaking::net::protocol::takion::CryptoDirection>,
          libchiaking::net::protocol::takion::CryptoError> override {
    return std::unique_ptr<libchiaking::net::protocol::takion::CryptoDirection>{
        new OutboundCountingLocal};
  }

  auto create_remote(libchiaking::net::protocol::takion::CryptoMaterial)
      -> libchiaking::net::protocol::takion::Expected<
          std::unique_ptr<libchiaking::net::protocol::takion::CryptoDirection>,
          libchiaking::net::protocol::takion::CryptoError> override {
    return libchiaking::net::protocol::takion::unexpected(
        libchiaking::net::protocol::takion::CryptoError::key_material_missing);
  }
};

class PlainOutbound {
 public:
  PlainOutbound(std::uint32_t version,
                libchiaking::net::protocol::takion::CryptoPolicy policy,
                bool allow_plain_control_until_crypto = false)
      : crypto(policy),
        config(config_for(policy, allow_plain_control_until_crypto)),
        profile(
            *libchiaking::net::protocol::takion::resolve_takion_wire_profile(
                version)) {
    outbound.set_profile(&profile);
  }

  static auto config_for(
      libchiaking::net::protocol::takion::CryptoPolicy policy,
      bool allow_plain_control_until_crypto)
      -> libchiaking::net::protocol::takion::ClientConfig {
    libchiaking::net::protocol::takion::ClientConfig result;
    result.bootstrap_version = 20;
    result.profile_selection =
        libchiaking::net::protocol::takion::FixedTakionProfile{20};
    result.crypto_policy = policy;
    result.allow_plain_control_until_crypto = allow_plain_control_until_crypto;
    return result;
  }

  OutboundTransport transport;
  test_support::FakeClock clock{0};
  libchiaking::net::protocol::takion::CryptoContext crypto;
  libchiaking::net::protocol::takion::ClientConfig config;
  libchiaking::net::protocol::takion::WireProfile profile;
  libchiaking::net::protocol::takion::client_detail::ClientOutbound outbound{
      &transport, clock, crypto, config};
};

class KeyedOutbound {
 public:
  KeyedOutbound(std::uint32_t version,
                libchiaking::net::protocol::takion::PacketEnvelope envelope)
      : profile(
            *libchiaking::net::protocol::takion::resolve_takion_wire_profile(
                version)) {
    config.bootstrap_version = version;
    config.profile_selection =
        libchiaking::net::protocol::takion::FixedTakionProfile{version};
    config.crypto_policy =
        libchiaking::net::protocol::takion::CryptoPolicy::required;
    config.envelope = envelope;
    const std::byte secret[]{std::byte{1}};
    EXPECT_TRUE(crypto.install_local(
        provider,
        libchiaking::net::protocol::takion::CryptoMaterial{
            libchiaking::net::protocol::takion::SecretBytes{secret}}));
    EXPECT_TRUE(crypto.set_local_key_position(1000));
    outbound = std::make_unique<
        libchiaking::net::protocol::takion::client_detail::ClientOutbound>(
        &transport, clock, crypto, config);
    outbound->set_profile(&profile);
    outbound->set_crypto_installed(true);
  }

  auto advanced() const -> std::uint64_t {
    return crypto.local_key_position() - 1000;
  }

  OutboundTransport transport;
  test_support::FakeClock clock{0};
  OutboundCountingProvider provider;
  libchiaking::net::protocol::takion::CryptoContext crypto{
      libchiaking::net::protocol::takion::CryptoPolicy::required};
  libchiaking::net::protocol::takion::ClientConfig config;
  libchiaking::net::protocol::takion::WireProfile profile;
  std::unique_ptr<
      libchiaking::net::protocol::takion::client_detail::ClientOutbound>
      outbound;
};

}  // namespace takion_test
