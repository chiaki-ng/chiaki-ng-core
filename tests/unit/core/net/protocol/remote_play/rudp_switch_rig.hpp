#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/protocol/remote_play/rudp_ctrl.hpp"
#include "src/core/net/protocol/remote_play/rudp_switch.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/random.hpp"
#include "tests/support/stop_states.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace remote_play_test {

class RudpSwitchRouteState {
 public:
  unsigned close_count{};
  bool fail_close{};
};

class RudpSwitchRouteTransport final
    : public libchiaking::net::transport::datagram::DatagramTransport {
 public:
  explicit RudpSwitchRouteTransport(std::shared_ptr<RudpSwitchRouteState> state)
      : state_(std::move(state)) {}

  auto send(std::span<const std::byte> datagram) -> libchiaking::Expected<
      std::size_t,
      libchiaking::net::transport::datagram::TransportError> override {
    return datagram.size();
  }

  auto receive() -> libchiaking::Expected<
      std::optional<libchiaking::net::transport::datagram::Datagram>,
      libchiaking::net::transport::datagram::TransportError> override {
    return std::optional<libchiaking::net::transport::datagram::Datagram>{};
  }

  auto close() -> libchiaking::Expected<
      void, libchiaking::net::transport::datagram::TransportError> override {
    ++state_->close_count;
    if (state_->fail_close)
      return libchiaking::unexpected(
          libchiaking::net::transport::datagram::TransportError::io_failure);
    return {};
  }

 private:
  std::shared_ptr<RudpSwitchRouteState> state_;
};

class RudpSwitchNoCrypto final : public libchiaking::platform::CryptoProvider {
 public:
  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }

  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<std::byte>)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }

  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }

  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }
};

class RudpSwitchChannel final
    : public libchiaking::net::transport::rudp::HandshakeChannel {
 public:
  auto send(std::span<const std::byte> bytes) -> libchiaking::Expected<
      std::size_t,
      libchiaking::net::transport::datagram::TransportError> override {
    sent.emplace_back(bytes.begin(), bytes.end());
    if (fail_send)
      return libchiaking::unexpected(
          libchiaking::net::transport::datagram::TransportError::io_failure);
    if (short_send) return std::size_t{0};
    return bytes.size();
  }

  auto receive_for(std::chrono::microseconds timeout,
                   const libchiaking::platform::StopToken&)
      -> libchiaking::Expected<
          std::optional<libchiaking::net::transport::datagram::OwnedDatagram>,
          libchiaking::net::transport::datagram::TransportError> override {
    if (on_receive) on_receive();
    if (std::exchange(early_empty, false))
      return std::optional<
          libchiaking::net::transport::datagram::OwnedDatagram>{};
    if (fail_receive)
      return libchiaking::unexpected(
          libchiaking::net::transport::datagram::TransportError::io_failure);
    if (inbox) return std::exchange(inbox, std::nullopt);
    clock->now_us += static_cast<std::uint64_t>(timeout.count());
    return std::optional<
        libchiaking::net::transport::datagram::OwnedDatagram>{};
  }

  test_support::FakeClock* clock{};
  std::optional<libchiaking::net::transport::datagram::OwnedDatagram> inbox;
  std::vector<std::vector<std::byte>> sent;
  std::function<void()> on_receive;
  bool fail_send{};
  bool short_send{};
  bool fail_receive{};
  bool early_empty{};
};

class RudpSwitchRig : public ::testing::Test {
 protected:
  void SetUp() override {
    profile =
        libchiaking::net::protocol::takion::resolve_takion_wire_profile(7);
    ASSERT_TRUE(profile);
    auto ctrl =
        libchiaking::net::protocol::remote_play::CtrlCodec::encode_frame_header(
            0x34, 0);
    ASSERT_TRUE(ctrl);
    std::vector<std::byte> payload{std::byte{0x56}, std::byte{0x77}};
    payload.insert(payload.end(), ctrl.value().begin(), ctrl.value().end());
    const libchiaking::net::transport::rudp::Frame frame{0x0230,
                                                         std::move(payload)};
    auto encoded =
        libchiaking::net::transport::rudp::FrameCodec::encode({&frame, 1});
    ASSERT_TRUE(encoded);
    switch_packet = encoded.value();
  }

  auto route(
      std::shared_ptr<RudpSwitchRouteState> state,
      libchiaking::net::protocol::remote_play::RouteKind kind =
          libchiaking::net::protocol::remote_play::RouteKind::remote_rudp) const
      -> libchiaking::net::protocol::remote_play::EstablishedRoute {
    return libchiaking::net::protocol::remote_play::EstablishedRoute{
        {std::make_unique<RudpSwitchRouteTransport>(std::move(state)),
         {libchiaking::platform::IpAddress::loopback_ipv4(), 50000},
         {libchiaking::platform::IpAddress::loopback_ipv4(), 9295},
         kind,
         libchiaking::net::protocol::remote_play::DontFragmentPolicy::disabled},
        {7, {}, 1454, 1454, *profile}};
  }

  auto heartbeat_packet() const
      -> libchiaking::net::transport::datagram::OwnedDatagram {
    auto heartbeat = switch_packet;
    heartbeat[15] = std::byte{0xfe};
    return heartbeat;
  }

  std::optional<libchiaking::net::protocol::takion::WireProfile> profile;
  libchiaking::net::transport::datagram::OwnedDatagram switch_packet;
};

}  // namespace remote_play_test
