#include "tests/unit/core/net/protocol/senkusha/client_adapter_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace senkusha_test {
namespace {

namespace datagram = libchiaking::net::transport::datagram;
namespace takion = libchiaking::net::protocol::takion;
using libchiaking::play::PingService;

class SenkushaClientCloud : public SenkushaClientRig {
 protected:
  static auto keyed_big_echo_config() -> ClientConfig {
    auto config = fixed_v9_big_echo_config();
    config.profile.big.session_key =
        libchiaking::net::protocol::takion::SharedSecretText{
            "synthetic-cloud-key"};
    return config;
  }

  static auto cloud_big(std::string key) -> takion::Big {
    return takion::Big{9,
                       takion::SharedSecretText{key},
                       takion::SharedSecretText{},
                       {},
                       std::nullopt,
                       std::nullopt};
  }

  SenkushaHarness harness;
};

TEST_F(SenkushaClientCloud, MovesOnWhenTheBigEchoWaitRunsOut) {
  auto subject = client(harness, keyed_big_echo_config());
  ASSERT_TRUE(subject->start());
  queue_handshake(harness.state(), 9);
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::awaiting_big_echo);
  EXPECT_EQ(subject->snapshot().deadline_us,
            std::optional<std::uint64_t>{harness.clock.now_us + 2'000'000});
  harness.clock.now_us += 1'999'999;
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::awaiting_big_echo);
  ++harness.clock.now_us;
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::enabling_echo);
  EXPECT_EQ(harness.events.failures(), 0U);
}

TEST_F(SenkushaClientCloud, StartsRttEarlyWhenTheBigEchoArrives) {
  auto subject = client(harness, keyed_big_echo_config());
  ASSERT_TRUE(subject->start());
  queue_handshake(harness.state(), 9);
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::awaiting_big_echo);
  auto peer_channel = peer(0xa1b2c3d4);
  queue_reliable_control(harness.state(), peer_channel,
                         cloud_big("synthetic-cloud-key"), 9);
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::enabling_echo);
  EXPECT_EQ(harness.events.failures(), 0U);
}

TEST_F(SenkushaClientCloud, PingsWithV7ProbesAndAnswersInV9) {
  for (const bool wrapped : {false, true}) {
    SCOPED_TRACE(wrapped ? "legacy cloud stream" : "cloud stream");
    SenkushaHarness ping;
    ping.state().wrap_incoming = false;
    auto config = cloud_config(wrapped ? PingService::legacy_cloud_stream
                                       : PingService::cloud_stream);
    config.policy.probe.count = 2;
    config.profile.big.session_key =
        libchiaking::net::protocol::takion::SharedSecretText{
            "synthetic-cloud-key"};
    auto subject = client(ping, std::move(config));
    const auto unwrap = [wrapped](const datagram::OwnedDatagram& value) {
      if (!wrapped) return value;
      EXPECT_GE(value.size(), 4U);
      if (value.size() < 4) return datagram::OwnedDatagram{};
      EXPECT_EQ(value[3], std::byte{1});
      return datagram::OwnedDatagram(value.begin() + 4, value.end());
    };
    ASSERT_TRUE(subject->start());
    queue_handshake(ping.state(), 9);
    ASSERT_TRUE(subject->pump_once());
    auto peer_channel = peer(0xa1b2c3d4);
    queue_reliable_control(ping.state(), peer_channel,
                           cloud_big("synthetic-cloud-key"), 9);
    ASSERT_TRUE(subject->pump_once());
    ASSERT_TRUE(subject->pump_once());
    const auto echo = unwrap(ping.state().sent.back());
    queue_reliable_ack(ping.state(), outbound_reliable_sequence(echo, 9), 9);
    ASSERT_TRUE(subject->pump_once());
    ASSERT_EQ(subject->snapshot().state, State::measuring_rtt);
    for (unsigned index = 0; index < 2; ++index) {
      ASSERT_TRUE(subject->pump_once());
      const auto probe = unwrap(ping.state().sent.back());
      std::vector<std::byte> expected(0x224);
      expected[0] = std::byte{3};
      expected[6] = static_cast<std::byte>(0x1fU | (index << 5U));
      expected[7] = std::byte{0xfc};
      expected[9] = std::byte{0xff};
      expected[22] = std::byte{0x11};
      expected[23] = std::byte{0x22};
      expected[24] = std::byte{0x33};
      expected[25] = std::byte{0x44};
      ASSERT_EQ(probe, expected) << "probe " << index;
      auto wrong = probe;
      wrong[22] ^= std::byte{1};
      ping.state().incoming.push_back(wrong);
      ASSERT_TRUE(subject->pump_once());
      EXPECT_EQ(subject->snapshot().state, State::measuring_rtt);
      ping.clock.now_us += 30'000;
      ping.state().incoming.push_back(probe);
      ASSERT_TRUE(subject->pump_once());
    }
    ASSERT_EQ(subject->snapshot().state, State::disabling_echo);
    ASSERT_TRUE(subject->pump_once());
    const auto disable = unwrap(ping.state().sent.back());
    queue_reliable_ack(ping.state(), outbound_reliable_sequence(disable, 9), 9);
    ASSERT_TRUE(subject->pump_once());
    ASSERT_EQ(subject->snapshot().state, State::measuring_inbound_mtu);
    ASSERT_TRUE(subject->snapshot().rtt_statistics);
    EXPECT_EQ(subject->snapshot().rtt_statistics->successful, 2U);
    EXPECT_EQ(subject->snapshot().rtt_statistics->adjusted_mean,
              std::chrono::microseconds{10'000});
    ASSERT_TRUE(subject->pump_once());
    auto wrong_mtu = inbound_video_response(2);
    wrong_mtu.resize(1426);
    ping.state().incoming.push_back(wrong_mtu);
    ASSERT_TRUE(subject->pump_once());
    EXPECT_EQ(subject->snapshot().state, State::measuring_inbound_mtu);
    auto mtu_reply = inbound_video_response(1);
    mtu_reply.resize(1426);
    ping.state().incoming.push_back(mtu_reply);
    ASSERT_TRUE(subject->pump_once());
    EXPECT_EQ(subject->snapshot().inbound_mtu,
              std::optional<std::uint32_t>{1454});
    ASSERT_EQ(subject->snapshot().state, State::starting_outbound_mtu);
    ASSERT_TRUE(subject->pump_once());
    queue_reliable_control(
        ping.state(), peer_channel,
        takion::Senkusha{takion::SenkushaClientMtuCommand{1, 1454, true, 1454}},
        9, 8);
    ASSERT_TRUE(subject->pump_once());
    ASSERT_EQ(subject->snapshot().state, State::measuring_outbound_mtu);
    ASSERT_TRUE(subject->pump_once());
    const auto mtu_probe = unwrap(ping.state().sent.back());
    ASSERT_EQ(mtu_probe.size(), 1426U);
    EXPECT_EQ(mtu_probe[22], std::byte{0x11});
    ping.state().incoming.push_back(mtu_probe);
    ASSERT_TRUE(subject->pump_once());
    ASSERT_EQ(subject->snapshot().state, State::finishing_outbound_mtu);
    ASSERT_TRUE(subject->pump_once());
    const auto finish = unwrap(ping.state().sent.back());
    queue_reliable_ack(ping.state(), outbound_reliable_sequence(finish, 9), 9);
    ASSERT_TRUE(subject->pump_once());
    EXPECT_EQ(subject->snapshot().state, State::completed);
    EXPECT_EQ(subject->snapshot().outbound_mtu,
              std::optional<std::uint32_t>{1454});
    EXPECT_EQ(ping.state().close_count, 1U);
    EXPECT_EQ(ping.events.failures(), 0U);
  }
}

TEST_F(SenkushaClientCloud,
       CountsBigEchoFragmentsInOrderWithReferencePrefixes) {
  for (const bool reordered : {false, true}) {
    SCOPED_TRACE(reordered ? "reordered" : "in order");
    SenkushaHarness echo;
    auto config = cloud_config(PingService::cloud_stream);
    config.profile.big.session_key =
        libchiaking::net::protocol::takion::SharedSecretText{
            "synthetic-fragmented-key"};
    auto subject = client(echo, std::move(config));
    ASSERT_TRUE(subject->start());
    queue_handshake(echo.state(), 9);
    ASSERT_TRUE(subject->pump_once());
    ASSERT_TRUE(subject->pump_once());
    const takion::ControlCodec codec;
    const auto encoded = codec.encode(cloud_big("synthetic-fragmented-key"));
    ASSERT_TRUE(encoded);
    const auto split = std::find(encoded.value().begin(), encoded.value().end(),
                                 std::byte{'s'});
    ASSERT_NE(split, encoded.value().end());
    const auto first_size =
        static_cast<std::size_t>(split - encoded.value().begin());
    const auto fragment = [&](bool first) {
      std::vector<std::byte> bytes{
          std::byte{0xa1}, std::byte{0xb2},
          std::byte{0xc3}, first ? std::byte{0xd4} : std::byte{0xd5},
          std::byte{0},    std::byte{1},
          std::byte{0},    std::byte{0}};
      if (first) {
        bytes.push_back(std::byte{0});
        bytes.insert(
            bytes.end(), encoded.value().begin(),
            encoded.value().begin() + static_cast<std::ptrdiff_t>(first_size));
      } else {
        bytes.insert(bytes.end(), split, encoded.value().end());
      }
      return control_packet(
          takion::DataChunk{
              takion::ControlHeader{takion::VerificationTag{0x11223344},
                                    {},
                                    0,
                                    first ? std::uint8_t{0} : std::uint8_t{1}},
              bytes},
          9);
    };
    echo.state().incoming.push_back(fragment(!reordered));
    ASSERT_TRUE(subject->pump_once());
    EXPECT_EQ(subject->snapshot().state, State::awaiting_big_echo);
    echo.state().incoming.push_back(fragment(!reordered));
    ASSERT_TRUE(subject->pump_once());
    EXPECT_EQ(subject->snapshot().state, State::awaiting_big_echo);
    echo.state().incoming.push_back(fragment(reordered));
    ASSERT_TRUE(subject->pump_once());
    EXPECT_EQ(subject->snapshot().state, State::enabling_echo);
    EXPECT_EQ(echo.clock.now_us, 1000U);
    EXPECT_EQ(echo.events.failures(), 0U);
    EXPECT_TRUE(subject->cancel());
  }
}

TEST_F(SenkushaClientCloud, FitsTheBigInReference576ByteIpPackets) {
  for (const auto service :
       {PingService::cloud_stream, PingService::legacy_cloud_stream}) {
    SCOPED_TRACE(testing::Message() << "service " << static_cast<int>(service));
    SenkushaHarness big;
    auto config = cloud_config(service);
    config.profile.big.session_key =
        libchiaking::net::protocol::takion::SharedSecretText{
            std::string(1300, 'k')};
    auto subject = client(big, std::move(config));
    ASSERT_TRUE(subject->start());
    queue_handshake(big.state(), 9);
    const auto before = big.state().sent.size();
    ASSERT_TRUE(subject->pump_once());
    ASSERT_TRUE(subject->pump_once());
    const auto& sent = big.state().sent;
    EXPECT_GE(sent.size(), before + 3);
    for (std::size_t index = before; index < sent.size(); ++index)
      EXPECT_LE(sent[index].size(), 576U - 28U) << "datagram " << index;
    EXPECT_TRUE(subject->cancel());
  }
}

TEST_F(SenkushaClientCloud, SendsASecureSessionKeyLikeAPlainOne) {
  auto config = fixed_v9_big_echo_config();
  libchiaking::SecretString key;
  key.assign("synthetic-cloud-key");
  config.profile.big.session_key = takion::SharedSecretText{
      std::make_shared<const libchiaking::SecretString>(std::move(key))};
  auto subject = client(harness, std::move(config));
  ASSERT_TRUE(subject->start());
  queue_handshake(harness.state(), 9);
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::awaiting_big_echo);
  ASSERT_TRUE(subject->pump_once());

  const auto message =
      decode_outbound_control(harness.state().sent.back(), 9, 0x11223344);
  ASSERT_TRUE(message);
  const auto* big = std::get_if<takion::Big>(&*message);
  ASSERT_NE(big, nullptr);
  EXPECT_EQ(big->session_key.view(), "synthetic-cloud-key");

  SenkushaHarness legacy;
  auto legacy_client = client(legacy, keyed_big_echo_config());
  ASSERT_TRUE(legacy_client->start());
  queue_handshake(legacy.state(), 9);
  ASSERT_TRUE(legacy_client->pump_once());
  ASSERT_TRUE(legacy_client->pump_once());
  EXPECT_EQ(harness.state().sent.back(), legacy.state().sent.back());
}

TEST_F(SenkushaClientCloud, FailsAtOnceWhenTheBigCannotBeSent) {
  auto config = cloud_config(PingService::cloud_stream);
  config.profile.big.session_key =
      libchiaking::net::protocol::takion::SharedSecretText{
          "synthetic-cloud-key"};
  auto subject = client(harness, std::move(config));
  ASSERT_TRUE(subject->start());
  queue_handshake(harness.state(), 9);
  ASSERT_TRUE(subject->pump_once());
  harness.state().fail_send = true;
  const auto pumped = subject->pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), Error::takion_failure);
  EXPECT_EQ(subject->snapshot().state, State::failed);
  EXPECT_EQ(harness.clock.now_us, 1000U);
  EXPECT_EQ(harness.events.failures(), 1U);
}

}  // namespace
}  // namespace senkusha_test
