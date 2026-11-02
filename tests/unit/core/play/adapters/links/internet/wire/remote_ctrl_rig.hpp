#pragma once

#include "libchiaking/platform/default_factory.hpp"
#include "src/core/net/transport/datagram/udp_transport.hpp"
#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "src/core/play/adapters/links/internet/wire/remote_ctrl.hpp"
#include "tests/support/clock.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace play_test::remote_ctrl {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace rudp = libchiaking::net::transport::rudp;
namespace datagram = libchiaking::net::transport::datagram;
namespace platform = libchiaking::platform;

using CryptoResult = libchiaking::Expected<void, platform::CryptoError>;

class ZeroCrypto final : public platform::CryptoProvider {
 public:
  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> CryptoResult override {
    return refused();
  }
  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<std::byte> output) -> CryptoResult override {
    std::ranges::fill(output, std::byte{0});
    return {};
  }
  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>,
                            std::span<std::byte> output)
      -> CryptoResult override {
    std::ranges::fill(output, std::byte{0});
    return {};
  }
  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> CryptoResult override {
    return refused();
  }

 private:
  static auto refused() -> CryptoResult {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }
};

class RejectCrypto final : public platform::CryptoProvider {
 public:
  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> CryptoResult override {
    return refused();
  }
  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<std::byte>) -> CryptoResult override {
    return refused();
  }
  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>, std::span<std::byte>)
      -> CryptoResult override {
    return refused();
  }
  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> CryptoResult override {
    return refused();
  }

 private:
  static auto refused() -> CryptoResult {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }
};

class StartupChannel final : public rudp::HandshakeChannel {
 public:
  explicit StartupChannel(test_support::FakeClock& clock) noexcept
      : clock_(clock) {}

  auto send(std::span<const std::byte> bytes)
      -> libchiaking::Expected<std::size_t, datagram::TransportError> override {
    sent_.emplace_back(bytes.begin(), bytes.end());
    return bytes.size();
  }
  auto receive_for(std::chrono::microseconds timeout,
                   const platform::StopToken&)
      -> libchiaking::Expected<std::optional<datagram::OwnedDatagram>,
                               datagram::TransportError> override {
    if (response_) return std::exchange(response_, std::nullopt);
    clock_.now_us += static_cast<std::uint64_t>(timeout.count());
    return std::optional<datagram::OwnedDatagram>{};
  }

  void respond(datagram::OwnedDatagram response) {
    response_ = std::move(response);
  }
  auto sent() const noexcept -> const std::vector<std::vector<std::byte>>& {
    return sent_;
  }

  static auto http_reply(std::string_view http) -> datagram::OwnedDatagram {
    std::vector<std::byte> payload{std::byte{0x56}, std::byte{0x78}};
    for (const char c : http) payload.push_back(static_cast<std::byte>(c));
    const rudp::Frame frame{0x0230, payload};
    auto encoded = rudp::FrameCodec::encode({&frame, 1});
    EXPECT_TRUE(encoded);
    if (!encoded) return {};
    return std::move(encoded.value());
  }

 private:
  test_support::FakeClock& clock_;
  std::optional<datagram::OwnedDatagram> response_;
  std::vector<std::vector<std::byte>> sent_;
};

class CtrlMessages {
 public:
  static auto parsed_id() -> remote_play::SessionId {
    std::array<std::byte, 25> bytes{};
    bytes[0] = std::byte{'J'};
    std::ranges::fill(bytes | std::views::drop(1), std::byte{'A'});
    auto id = remote_play::SessionId::parse(bytes);
    EXPECT_TRUE(id);
    return std::move(id.value());
  }

  static auto step(remote_play::CtrlMessage message, std::uint64_t attempt)
      -> remote_play::RudpCtrlSwitchStepResult {
    remote_play::RudpCtrlMessageResult decoded{};
    decoded.message = std::move(message);
    const bool switch_notification =
        std::holds_alternative<remote_play::CtrlStreamSwitchAck>(
            decoded.message);
    return remote_play::RudpCtrlSwitchStepResult{std::move(decoded), attempt,
                                                 switch_notification};
  }

  static auto prepared(platform::CryptoProvider& crypto)
      -> remote_play::PreparedLocalCtrl {
    remote_play::RegisteredConsole console;
    console.target = remote_play::RegistrationTarget::ps5_1;
    const std::array<std::byte, 32> did{};
    std::array<std::byte, 16> nonce{};
    nonce.fill(std::byte{1});
    auto result = remote_play::LocalCtrlPreparation::prepare_from_nonce(
        crypto, console, "127.0.0.1", 9295, did, remote_play::VideoCodec::h264,
        nonce);
    EXPECT_TRUE(result);
    if (!result) return {};
    return std::move(result.value());
  }

  static auto loopback_transport(platform::DefaultPlatformFactory& platform)
      -> std::unique_ptr<datagram::ProviderUdpTransport> {
    auto created = datagram::ProviderUdpTransport::create(
        platform.network(),
        datagram::UdpTransportConfig{
            platform::SocketEndpoint{platform::IpAddress::loopback_ipv4(), 0}});
    EXPECT_TRUE(created);
    if (!created) return nullptr;
    return std::move(created.value());
  }
};

}  // namespace play_test::remote_ctrl
