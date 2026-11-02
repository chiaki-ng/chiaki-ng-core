#include "tests/support/stop_states.hpp"
#include "tests/unit/core/net/transport/datagram/loopback_rig.hpp"
#include "tests/unit/core/net/transport/rudp/rudp_rig.hpp"

#include <libchiaking/platform/runtime/threading.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace rudp_test {
namespace {

using libchiaking::net::transport::datagram::ProviderUdpTransport;
using libchiaking::net::transport::datagram::TransportError;
using libchiaking::net::transport::rudp::Connected;
using libchiaking::net::transport::rudp::ConnectError;
using libchiaking::net::transport::rudp::Connector;
using libchiaking::net::transport::rudp::FrameCodec;
using libchiaking::net::transport::rudp::HandshakeCompletion;
using libchiaking::net::transport::rudp::SwitchSendError;
namespace pf = libchiaking::platform;

class RudpConnectorOverChannel : public ::testing::Test, protected RudpPackets {
 protected:
  test_support::FakeClock clock{0};
  ScriptedChannel channel{clock};
  ScriptedRandom random;
};

TEST_F(RudpConnectorOverChannel, DerivesEveryValueFromTheRandomBytes) {
  random.words(0x5DFF, 0x1234);
  const std::array<std::byte, 2> cookie{std::byte{0xC0}, std::byte{0x0E}};
  channel.inbox.push_back(init_response(0x5678, cookie));
  channel.inbox.push_back(cookie_response(0x5679));
  const auto result =
      Connector::connect_over_channel(channel, random, clock, {}, 5);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().local_header, 0x9234);
  EXPECT_EQ(result.value().next_local_counter, 0x6000);
  EXPECT_EQ(result.value().remote_counter, 0x567A);
  ASSERT_EQ(channel.sent.size(), 2U);
  const auto init = FrameCodec::encode_init_request(0x5FFE, 0x9234);
  ASSERT_TRUE(init.has_value());
  EXPECT_EQ(channel.sent[0], init.value());
  const auto cookie_request =
      FrameCodec::encode_cookie_request(0x5FFF, 0x9234, cookie);
  ASSERT_TRUE(cookie_request.has_value());
  EXPECT_EQ(channel.sent[1], cookie_request.value());
}

TEST_F(RudpConnectorOverChannel, ReportsAShortSend) {
  random.words(0x5DFF, 0x1234);
  channel.short_send = true;
  const auto result =
      Connector::connect_over_channel(channel, random, clock, {}, 1);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ConnectError::short_send);
}

TEST_F(RudpConnectorOverChannel, SendsNothingWhenTheRandomSourceFails) {
  random.fail = true;
  const auto result =
      Connector::connect_over_channel(channel, random, clock, {}, 1);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ConnectError::random_failed);
  EXPECT_TRUE(channel.sent.empty());
}

TEST_F(RudpConnectorOverChannel, SendsNothingWhenStoppedFirst) {
  random.words(0, 0);
  auto state = std::make_shared<test_support::StopState>();
  state->stopped = true;
  const auto result = Connector::connect_over_channel(channel, random, clock,
                                                      pf::StopToken{state}, 1);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ConnectError::stopped);
  EXPECT_TRUE(channel.sent.empty());
}

TEST_F(RudpConnectorOverChannel, ReportsASendFailure) {
  random.words(0, 0);
  channel.send_error = TransportError::io_failure;
  const auto result =
      Connector::connect_over_channel(channel, random, clock, {}, 1);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ConnectError::send_failed);
}

TEST_F(RudpConnectorOverChannel, ReportsAReceiveFailure) {
  random.words(0, 0);
  channel.receive_error = TransportError::closed;
  const auto result =
      Connector::connect_over_channel(channel, random, clock, {}, 1);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ConnectError::receive_failed);
}

TEST_F(RudpConnectorOverChannel, TimesOutAfterTheFullDeadline) {
  random.words(0, 0);
  const auto result =
      Connector::connect_over_channel(channel, random, clock, {}, 1);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ConnectError::timeout);
  EXPECT_GE(clock.now_microseconds(),
            libchiaking::net::transport::rudp::rudp_expect_timeout_us);
  EXPECT_GE(channel.sent.size(), 1U);
}

class RudpConnectorOverUdp : public datagram_test::UdpLoopbackRig,
                             protected RudpPackets {
 protected:
  using Bytes = RudpPackets::Bytes;

  auto unbound_transport() -> std::unique_ptr<ProviderUdpTransport> {
    auto transport =
        ProviderUdpTransport::create(factory.network(), loopback_config());
    EXPECT_TRUE(transport.has_value());
    return transport ? std::move(transport.value()) : nullptr;
  }

  static auto switch_connected(std::unique_ptr<ProviderUdpTransport> transport,
                               std::uint16_t next_local_counter = 0xFFFF)
      -> Connected {
    return Connected{std::move(transport),
                     HandshakeCompletion{0x9234, next_local_counter, 0x567A}};
  }

  static auto counting_nonce() -> Bytes {
    Bytes out;
    for (std::uint8_t value = 1; value <= 16; ++value)
      out.push_back(static_cast<std::byte>(value));
    return out;
  }

  auto clock() -> pf::ClockProvider& { return factory.clock(); }

  ScriptedRandom random;
};

TEST_F(RudpConnectorOverUdp, RefusesAMissingTransport) {
  random.words(0, 0);
  const auto result = Connector::connect(nullptr, random, clock(), {}, 1);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ConnectError::invalid_transport);
}

TEST_F(RudpConnectorOverUdp, StopsBeforeConnectingWhenStoppedFirst) {
  random.words(0, 0);
  auto transport = unbound_transport();
  ASSERT_NE(transport, nullptr);
  auto state = std::make_shared<test_support::StopState>();
  state->stopped = true;
  const auto result = Connector::connect(std::move(transport), random, clock(),
                                         pf::StopToken{state}, 1);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ConnectError::stopped);
}

TEST_F(RudpConnectorOverUdp, ReportsARandomFailure) {
  random.fail = true;
  auto transport = unbound_transport();
  ASSERT_NE(transport, nullptr);
  const auto result =
      Connector::connect(std::move(transport), random, clock(), {}, 1);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ConnectError::random_failed);
}

TEST_F(RudpConnectorOverUdp, ConnectsToALoopbackPeer) {
  random.words(0x5DFF, 0x1234);
  ASSERT_NO_FATAL_FAILURE(open_pair());
  auto& peer = *second;
  bool peer_ok = false;
  const auto answering = [&peer, &peer_ok]() {
    const pf::StopToken idle{};
    auto readable = peer.wait_readable(std::chrono::seconds{2}, idle);
    if (!readable.has_value() || !readable.value()) return;
    auto datagram = peer.receive();
    if (!datagram.has_value() || !datagram.value().has_value()) return;
    auto frames = FrameCodec::decode(*datagram.value());
    if (!frames.has_value() || frames.value().size() != 1 ||
        frames.value()[0].type != 0x8030)
      return;
    const std::array<std::byte, 2> cookie{std::byte{0xC0}, std::byte{0x0E}};
    if (!peer.send(init_response(0x5678, cookie)).has_value()) return;
    readable = peer.wait_readable(std::chrono::seconds{2}, idle);
    if (!readable.has_value() || !readable.value()) return;
    datagram = peer.receive();
    if (!datagram.has_value() || !datagram.value().has_value()) return;
    frames = FrameCodec::decode(*datagram.value());
    if (!frames.has_value() || frames.value().size() != 1 ||
        frames.value()[0].type != 0x9030)
      return;
    if (!peer.send(cookie_response(0x5679)).has_value()) return;
    readable = peer.wait_readable(std::chrono::seconds{2}, idle);
    if (!readable.has_value() || !readable.value()) return;
    datagram = peer.receive();
    if (!datagram.has_value() || !datagram.value().has_value()) return;
    peer_ok = true;
  };
  auto peer_thread = pf::Thread::start(factory.threading(), "rudp-peer",
                                       pf::ThreadRole::unspecified, answering);
  ASSERT_TRUE(peer_thread);
  const auto result =
      Connector::connect(std::move(first), random, clock(), {}, 5);
  if (result.has_value()) {
    const std::array<std::byte, 1> payload{std::byte{0x42}};
    (void)result.value().transport->send(payload);
  }
  static_cast<void>(peer_thread.value().join());
  ASSERT_TRUE(result.has_value());
  EXPECT_NE(result.value().transport, nullptr);
  EXPECT_EQ(result.value().completion.local_header, 0x9234);
  EXPECT_EQ(result.value().completion.next_local_counter, 0x6000);
  EXPECT_EQ(result.value().completion.remote_counter, 0x567A);
  EXPECT_TRUE(peer_ok);
}

class RudpSwitchRequestSend : public RudpConnectorOverUdp {};

TEST_F(RudpSwitchRequestSend, RefusesAMissingTransport) {
  const auto result =
      Connector::send_switch_request(switch_connected(nullptr), random);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SwitchSendError::invalid_transport);
}

TEST_F(RudpSwitchRequestSend, ReportsARandomFailureAsAPacketFailure) {
  auto transport = unbound_transport();
  ASSERT_NE(transport, nullptr);
  random.fail = true;
  const auto result = Connector::send_switch_request(
      switch_connected(std::move(transport)), random);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SwitchSendError::packet_failed);
}

TEST_F(RudpSwitchRequestSend, SendsTheRequestAndKeepsTheTransport) {
  random.counting_nonce();
  ASSERT_NO_FATAL_FAILURE(open_pair());
  const void* raw_before = first.get();
  auto result = Connector::send_switch_request(
      switch_connected(std::move(first)), random);
  ASSERT_TRUE(result.has_value());
  ASSERT_NE(result.value().transport, nullptr);
  EXPECT_EQ(result.value().transport.get(), raw_before);
  EXPECT_EQ(result.value().completion.next_local_counter, 0x0000);

  const std::array<std::byte, 3> followup{std::byte{0xAA}, std::byte{0xBB},
                                          std::byte{0xCC}};
  const auto sent = result.value().transport->send(followup);
  ASSERT_TRUE(sent.has_value());
  EXPECT_EQ(sent.value(), followup.size());

  const auto expected =
      FrameCodec::encode_switch_request_random(0xFFFF, random);
  ASSERT_TRUE(expected.has_value());
  ASSERT_TRUE(ready(*second, stop));
  auto datagram = second->receive();
  ASSERT_TRUE(datagram.has_value());
  ASSERT_TRUE(datagram.value().has_value());
  EXPECT_TRUE(std::ranges::equal(*datagram.value(), expected.value()));
  const auto frames = FrameCodec::decode(*datagram.value());
  ASSERT_TRUE(frames.has_value());
  ASSERT_EQ(frames.value().size(), 1U);
  const auto& frame = frames.value()[0];
  EXPECT_EQ(frame.type, 0x0230);
  ASSERT_EQ(frame.payload.size(), 26U);
  EXPECT_EQ(frame.payload[0], std::byte{0xFF});
  EXPECT_EQ(frame.payload[1], std::byte{0xFF});
  const std::array<std::byte, 8> fixed{
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x10},
      std::byte{0x00}, std::byte{0x0D}, std::byte{0x00}, std::byte{0x00}};
  EXPECT_TRUE(
      std::equal(fixed.begin(), fixed.end(), frame.payload.begin() + 2));
  const auto nonce = counting_nonce();
  EXPECT_TRUE(
      std::equal(nonce.begin(), nonce.end(), frame.payload.begin() + 10));

  ASSERT_TRUE(ready(*second, stop));
  datagram = second->receive();
  ASSERT_TRUE(datagram.has_value());
  ASSERT_TRUE(datagram.value().has_value());
  EXPECT_TRUE(std::ranges::equal(*datagram.value(), followup));
}

TEST_F(RudpSwitchRequestSend, ReportsASendFailureOnAClosedTransport) {
  auto transport = unbound_transport();
  ASSERT_NE(transport, nullptr);
  ASSERT_TRUE(transport->close().has_value());
  random.counting_nonce();
  const auto result = Connector::send_switch_request(
      switch_connected(std::move(transport)), random);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SwitchSendError::send_failed);
}

}  // namespace
}  // namespace rudp_test
