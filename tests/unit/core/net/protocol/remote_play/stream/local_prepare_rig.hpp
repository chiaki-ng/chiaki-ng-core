#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/protocol/remote_play/stream/local_prepare.hpp"
#include "src/core/net/protocol/remote_play/stream/local_start.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/utils/text/base64.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace remote_play_test {

class LocalPrepareKeyPair final : public libchiaking::platform::EcdhKeyPair {
 public:
  explicit LocalPrepareKeyPair(std::size_t size) : size_(size) {}

  auto public_coordinates(std::span<std::byte> output) const
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    EXPECT_EQ(output.size(), size_);
    for (std::size_t i = 0; i < output.size(); ++i)
      output[i] = static_cast<std::byte>(i);
    return {};
  }

  auto derive_secret(std::span<const std::byte>, std::span<std::byte>) const
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }

 private:
  std::size_t size_;
};

class LocalPrepareCrypto final : public libchiaking::platform::CryptoProvider {
 public:
  auto generate_ecdh(libchiaking::platform::EcdhCurve curve)
      -> libchiaking::Expected<
          std::unique_ptr<libchiaking::platform::EcdhKeyPair>,
          libchiaking::platform::CryptoError> override {
    ++generate_calls;
    generated_curve = curve;
    if (fail_generate)
      return libchiaking::unexpected(
          libchiaking::platform::CryptoError::backend_failure);
    return std::unique_ptr<libchiaking::platform::EcdhKeyPair>{
        new LocalPrepareKeyPair(
            libchiaking::platform::ecdh_public_coordinates_size(curve))};
  }

  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }

  auto hmac_sha256(std::span<const std::byte> key,
                   std::span<const std::byte> input,
                   std::span<std::byte> digest)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    ++hmac_calls;
    if (input.size() == 65 || input.size() == 133)
      ecdh_hmac_key.assign(key.begin(), key.end());
    if (input.size() == 24)
      launch_hmac_input.assign(input.begin(), input.end());
    for (auto& byte : digest) byte = std::byte{0};
    return {};
  }

  auto aes128_encrypt_block(std::span<const std::byte> key,
                            std::span<const std::byte>,
                            std::span<std::byte> output)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    ++aes_calls;
    if (launch_aes_key.empty()) launch_aes_key.assign(key.begin(), key.end());
    if (fail_aes)
      return libchiaking::unexpected(
          libchiaking::platform::CryptoError::backend_failure);
    for (auto& byte : output) byte = std::byte{0};
    return {};
  }

  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }

  unsigned generate_calls{};
  unsigned hmac_calls{};
  unsigned aes_calls{};
  libchiaking::platform::EcdhCurve generated_curve{
      libchiaking::platform::EcdhCurve::secp256k1};
  std::vector<std::byte> ecdh_hmac_key;
  std::vector<std::byte> launch_hmac_input;
  std::vector<std::byte> launch_aes_key;
  bool fail_generate{};
  bool fail_aes{};
};

class LocalPrepareRandom final
    : public libchiaking::platform::SecureRandomProvider {
 public:
  auto fill_bytes(std::span<std::byte> output)
      -> libchiaking::Expected<void,
                               libchiaking::platform::PlatformError> override {
    ++calls;
    if (fail || (fail_on_call && calls == *fail_on_call)) {
      output[0] = std::byte{0xff};
      return libchiaking::unexpected(
          libchiaking::platform::PlatformError::io_failure);
    }
    for (std::size_t i = 0; i < output.size(); ++i)
      output[i] = static_cast<std::byte>(0x10 + i);
    return {};
  }

  unsigned calls{};
  bool fail{};
  std::optional<unsigned> fail_on_call{};
};

struct LocalPrepareRoute {
  std::vector<libchiaking::net::transport::datagram::OwnedDatagram> sent;
  unsigned close_count{};
};

class LocalPrepareTransport final
    : public libchiaking::net::transport::datagram::DatagramTransport {
 public:
  explicit LocalPrepareTransport(std::shared_ptr<LocalPrepareRoute> state)
      : state_(std::move(state)) {}

  auto send(std::span<const std::byte> datagram)
      -> libchiaking::net::protocol::takion::Expected<
          std::size_t,
          libchiaking::net::transport::datagram::TransportError> override {
    state_->sent.emplace_back(datagram.begin(), datagram.end());
    return datagram.size();
  }

  auto receive() -> libchiaking::net::protocol::takion::Expected<
      std::optional<libchiaking::net::transport::datagram::Datagram>,
      libchiaking::net::transport::datagram::TransportError> override {
    return std::optional<libchiaking::net::transport::datagram::Datagram>{};
  }

  auto close() -> libchiaking::net::protocol::takion::Expected<
      void, libchiaking::net::transport::datagram::TransportError> override {
    ++state_->close_count;
    return {};
  }

 private:
  std::shared_ptr<LocalPrepareRoute> state_;
};

class LocalPrepareUnusedTakionCrypto final
    : public libchiaking::net::protocol::takion::CryptoProvider {
 public:
  auto create_local(libchiaking::net::protocol::takion::CryptoMaterial)
      -> libchiaking::net::protocol::takion::Expected<
          std::unique_ptr<libchiaking::net::protocol::takion::CryptoDirection>,
          libchiaking::net::protocol::takion::CryptoError> override {
    return libchiaking::net::protocol::takion::unexpected(
        libchiaking::net::protocol::takion::CryptoError::provider_failure);
  }

  auto create_remote(libchiaking::net::protocol::takion::CryptoMaterial)
      -> libchiaking::net::protocol::takion::Expected<
          std::unique_ptr<libchiaking::net::protocol::takion::CryptoDirection>,
          libchiaking::net::protocol::takion::CryptoError> override {
    return libchiaking::net::protocol::takion::unexpected(
        libchiaking::net::protocol::takion::CryptoError::provider_failure);
  }
};

class LocalPrepareRig : public ::testing::Test {
 protected:
  using SenkushaResult = libchiaking::net::protocol::senkusha::Result;

  static auto threading() -> libchiaking::platform::DefaultThreadingProvider& {
    return test_support::process_threading();
  }

  static auto registered_console()
      -> libchiaking::net::protocol::remote_play::RegisteredConsole {
    libchiaking::net::protocol::remote_play::RegisteredConsole console;
    console.target =
        libchiaking::net::protocol::remote_play::RegistrationTarget::ps4_9;
    for (std::size_t i = 0; i < console.key.size(); ++i)
      console.key[i] = static_cast<std::byte>(0x50 + i);
    return console;
  }

  static auto ctrl_nonce() -> std::array<std::byte, 16> {
    std::array<std::byte, 16> nonce{};
    for (std::size_t i = 0; i < nonce.size(); ++i)
      nonce[i] = static_cast<std::byte>(0x30 + i);
    return nonce;
  }

  static auto calibration(std::uint32_t version,
                          std::uint32_t inbound_mtu = 1300,
                          std::int64_t rtt_us = 2005)
      -> std::optional<SenkushaResult> {
    const auto profile =
        libchiaking::net::protocol::takion::resolve_takion_wire_profile(
            version);
    if (!profile) return std::nullopt;
    libchiaking::net::protocol::senkusha::RttStatistics rtt{};
    rtt.adjusted_mean = std::chrono::microseconds{rtt_us};
    return SenkushaResult{version, rtt, inbound_mtu, 1400, *profile};
  }

  static auto session_id()
      -> std::optional<libchiaking::net::protocol::remote_play::SessionId> {
    std::vector<std::byte> payload{std::byte{0x4a}};
    constexpr std::string_view id = "ABCdef0123456789ABCdef01";
    for (const char c : id) payload.push_back(static_cast<std::byte>(c));
    auto parsed =
        libchiaking::net::protocol::remote_play::SessionId::parse(payload);
    if (!parsed) return std::nullopt;
    return std::move(parsed.value());
  }

  static auto launch_spec_json(std::string_view encoded)
      -> std::optional<std::string> {
    std::string output(encoded.size(), '\0');
    const auto written = libchiaking::text::base64::decode_into(
        encoded, std::as_writable_bytes(std::span{output}),
        libchiaking::text::base64::Alphabet::standard);
    if (!written) return std::nullopt;
    output.resize(written.value());
    return output;
  }

  static auto established_route(std::shared_ptr<LocalPrepareRoute> state,
                                const SenkushaResult& calibrated)
      -> libchiaking::net::protocol::remote_play::EstablishedRoute {
    return {
        {std::make_unique<LocalPrepareTransport>(std::move(state)),
         {libchiaking::platform::IpAddress::loopback_ipv4(), 50000},
         {libchiaking::platform::IpAddress::loopback_ipv4(), 9295},
         libchiaking::net::protocol::remote_play::RouteKind::local_direct,
         libchiaking::net::protocol::remote_play::DontFragmentPolicy::enabled},
        calibrated};
  }

  static auto console_version(
      libchiaking::net::protocol::remote_play::RegistrationTarget target)
      -> std::uint32_t {
    return libchiaking::net::protocol::takion::select_takion_version(
        target == libchiaking::net::protocol::remote_play::RegistrationTarget::
                      ps5_1
            ? libchiaking::net::protocol::takion::takion_version_ps5
            : libchiaking::net::protocol::takion::takion_version_ps4,
        0);
  }
};

}  // namespace remote_play_test
