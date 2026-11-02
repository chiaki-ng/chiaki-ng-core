#include "libchiaking/platform/default_factory.hpp"
#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "src/core/play/adapters/links/internet/wire/remote_ctrl.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/remote_ctrl_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test {
namespace {

namespace internet = libchiaking::play::internet;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace rudp = libchiaking::net::transport::rudp;
namespace datagram = libchiaking::net::transport::datagram;
namespace platform = libchiaking::platform;
using remote_ctrl::CtrlMessages;
using remote_ctrl::RejectCrypto;
using remote_ctrl::ZeroCrypto;
using ConnectError = internet::remote_ctrl::ConnectError;
using remote_play::VideoCodec;

class RemoteCtrlConnector : public ::testing::Test {
 protected:
  RemoteCtrlConnector() {
    console_.target = remote_play::RegistrationTarget::ps5_1;
  }

  auto connect(rudp::Connected connected, std::string_view host,
               std::uint16_t port, std::span<const std::byte> device_id,
               std::span<const std::byte> nonce, platform::StopToken stop,
               std::uint64_t attempt) {
    return internet::remote_ctrl::Connector::connect_registered(
        std::move(connected), platform_, crypto_, console_, host, port,
        device_id, VideoCodec::h264, nonce, std::move(stop), attempt);
  }

  auto loopback() -> rudp::Connected {
    return {CtrlMessages::loopback_transport(platform_), {}};
  }

  static constexpr std::array<std::byte, 32> did{};

  platform::DefaultPlatformFactory platform_;

 private:
  RejectCrypto crypto_;
  remote_play::RegisteredConsole console_{};
};

TEST_F(RemoteCtrlConnector, RefusesAMissingTransport) {
  const auto result = connect({}, "127.0.0.1", 9295, did, {}, {}, 1);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConnectError::invalid_transport);
}

TEST_F(RemoteCtrlConnector, RefusesAnEmptyHost) {
  const auto result = connect(loopback(), "", 9295, did, {}, {}, 1);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConnectError::invalid_host);
}

TEST_F(RemoteCtrlConnector, RefusesPortZero) {
  const auto result = connect(loopback(), "127.0.0.1", 0, did, {}, {}, 1);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConnectError::invalid_port);
}

TEST_F(RemoteCtrlConnector, StopsWhenAskedBeforeStarting) {
  const auto result = connect(
      loopback(), "127.0.0.1", 9295, did, {},
      platform::StopToken{std::make_shared<test_support::Stopped>()}, 1);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConnectError::stopped);
}

TEST_F(RemoteCtrlConnector, RefusesAttemptZero) {
  const auto result = connect(loopback(), "127.0.0.1", 9295, did, {}, {}, 0);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConnectError::invalid_attempt);
}

TEST_F(RemoteCtrlConnector, FailsToPrepareWithoutADeviceId) {
  const auto result =
      connect(loopback(), "127.0.0.1", 9295, {}, {did.data(), 16}, {}, 1);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConnectError::prepare_failed);
}

TEST_F(RemoteCtrlConnector, FailsToPrepareWithoutANonce) {
  const auto result = connect(loopback(), "127.0.0.1", 9295, did, {}, {}, 1);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConnectError::prepare_failed);
}

TEST(RemoteCtrlConnectorSession, ContinuesTheSessionRudpWithAnInitRequest) {
  platform::DefaultPlatformFactory platform;
  ZeroCrypto crypto;
  remote_play::RegisteredConsole console{};
  console.target = remote_play::RegistrationTarget::ps5_1;
  const std::array<std::byte, 32> did{};
  const std::array<std::byte, 16> nonce{};
  auto peer = CtrlMessages::loopback_transport(platform);
  ASSERT_NE(peer, nullptr);
  const auto peer_endpoint = peer->local_endpoint();
  ASSERT_TRUE(peer_endpoint);
  auto ours = datagram::ProviderUdpTransport::create(
      platform.network(),
      datagram::UdpTransportConfig{
          platform::SocketEndpoint{platform::IpAddress::loopback_ipv4(), 0},
          peer_endpoint.value()});
  ASSERT_TRUE(ours);
  auto stop_state = std::make_shared<test_support::AtomicStop>();
  std::vector<std::byte> first_init;
  auto console_peer = platform::Thread::start(
      platform.threading(), "console peer", platform::ThreadRole::unspecified,
      [&] {
        const auto ready =
            peer->wait_readable(std::chrono::seconds{5}, platform::StopToken{});
        if (ready && ready.value()) {
          auto received = peer->receive();
          if (received && received.value())
            first_init.assign(received.value()->begin(),
                              received.value()->end());
        }
        stop_state->stopped.store(true);
      });
  ASSERT_TRUE(console_peer);
  const auto connected = internet::remote_ctrl::Connector::connect_registered(
      {std::move(ours.value()), {0xA1B2C3D4U, 0x1234, 0x5678}}, platform,
      crypto, console, "127.0.0.1", 9295, did, VideoCodec::h264, nonce,
      platform::StopToken{stop_state}, 1);
  static_cast<void>(console_peer.value().join());
  ASSERT_FALSE(connected);
  EXPECT_EQ(connected.error().kind, ConnectError::stopped);
  const auto expected =
      rudp::FrameCodec::encode_init_request(0x1234, 0xA1B2C3D4U);
  ASSERT_TRUE(expected);
  EXPECT_EQ(first_init, expected.value());
}

class RemoteCtrlConnectorDrain : public ::testing::Test {
 protected:
  void SetUp() override {
    internet::remote_ctrl::StartupResult startup{};
    startup.final.response.next_remote_counter = 10;
    startup.final.remote_counter = 20;
    auto created =
        internet::remote_ctrl::Session::create(std::move(startup), 7);
    ASSERT_TRUE(created);
    owner_ = std::move(created.value());
  }

  auto drain(remote_play::WipingCtrlBytes& trailing) {
    return internet::remote_ctrl::Connector::drain_initial_frames(
        *owner_, trailing, crypto_, remote_play::RegistrationTarget::ps5_1);
  }

  auto owner() -> internet::remote_ctrl::Session& { return *owner_; }

 private:
  ZeroCrypto crypto_;
  std::unique_ptr<internet::remote_ctrl::Session> owner_;
};

TEST_F(RemoteCtrlConnectorDrain, TakesTheSessionIdAndFinishesAPartialFrame) {
  const auto id_header = remote_play::CtrlCodec::encode_frame_header(0x33, 25);
  ASSERT_TRUE(id_header);
  remote_play::WipingCtrlBytes trailing{id_header.value().begin(),
                                        id_header.value().end()};
  trailing.push_back(std::byte{'J'});
  for (int index = 0; index < 24; ++index) trailing.push_back(std::byte{'A'});
  const auto heartbeat = remote_play::CtrlCodec::encode_frame_header(0xfe, 0);
  ASSERT_TRUE(heartbeat);
  trailing.insert(trailing.end(), heartbeat.value().begin(),
                  heartbeat.value().begin() + 3);
  EXPECT_TRUE(drain(trailing));
  EXPECT_EQ(owner().pump_state().remote_crypt_counter, 11U);
  const auto ticket = owner().take_ready_ticket();
  ASSERT_TRUE(ticket);
  EXPECT_EQ(ticket->session_id.view(), "AAAAAAAAAAAAAAAAAAAAAAAA");
  EXPECT_EQ(trailing.size(), 3U);
  trailing.insert(trailing.end(), heartbeat.value().begin() + 3,
                  heartbeat.value().end());
  EXPECT_TRUE(drain(trailing));
  EXPECT_TRUE(trailing.empty());
  EXPECT_EQ(owner().pump_state().remote_crypt_counter, 11U);
  EXPECT_TRUE(owner().inbox().front_effect());
}

TEST_F(RemoteCtrlConnectorDrain, RefusesTrailingBytesLargerThanAFrame) {
  remote_play::WipingCtrlBytes oversized(513, std::byte{});
  const auto too_large = drain(oversized);
  ASSERT_FALSE(too_large);
  EXPECT_EQ(too_large.error(), ConnectError::trailing_too_large);
}

}  // namespace
}  // namespace play_test
