#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/net/default.hpp"
#include "libchiaking/platform/net/poller.hpp"
#include "src/core/net/protocol/takion/control_chunk_codec.hpp"
#include "src/core/net/protocol/takion/detail/envelope_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/cloud/loopback_rig.hpp"
#include "tests/support/live_providers.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <variant>

namespace play_test {

using test_support::LiveProviders;
namespace {

namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;
using cloud_loopback::GkScript;
using cloud_loopback::HandshakeCrypto;
using cloud_loopback::LoopbackSink;
using libchiaking::cloud::CloudPlatform;

class AllocatedCloudLink : public cloud_loopback::LoopbackTest {
 protected:
  void stream(CloudService service, std::uint32_t takion_version_override) {
    auto peer = network_.sockets().create_udp_socket(
        platform::IpAddress::loopback_ipv4());
    ASSERT_TRUE(peer);
    ASSERT_TRUE(peer.value()->bind({platform::IpAddress::loopback_ipv4(), 0}));
    const auto endpoint = peer.value()->local_endpoint();
    ASSERT_TRUE(endpoint);
    play::CloudLinkConfiguration config;
    config.tuning.takion_version_override = takion_version_override;
    play::CloudAllocation allocation;
    allocation.service = service;
    allocation.platform = service == CloudService::legacy_cloud_stream
                              ? CloudPlatform::ps4
                              : CloudPlatform::ps5;
    allocation.server_ip = "127.0.0.1";
    allocation.server_port = endpoint.value().port;
    allocation.handshake_key.assign("AAECAwQFBgcICQoLDA0ODw==");
    allocation.launch_spec.assign("synthetic-launch");
    allocation.session_id.assign("synthetic-session");
    allocation.psn_wrapper_type = 3;
    allocation.mtu_in = 1400;
    allocation.mtu_out = 1200;
    config.allocation = std::move(allocation);
    auto session =
        play::CloudLinkConfiguration::link(std::move(config), factory_);
    ASSERT_TRUE(session);
    LoopbackSink sink;
    session.value()->set_event_sink(sink);
    EXPECT_TRUE(session.value()->start());
    EXPECT_EQ(http_.calls, 0U);
    EXPECT_TRUE(sink.progress_texts.empty());
    EXPECT_EQ(sink.datacenter_events, 0U);
    EXPECT_EQ(session.value()->state(),
              remote_play::StreamState::negotiating_stream);
    const auto expected = takion_version_override ? takion_version_override
                          : service == CloudService::cloud_stream
                              ? takion::takion_version_cloud_stream
                              : takion::takion_version_legacy_cloud_stream;
    EXPECT_EQ(session.value()->takion_version(), expected);
    EXPECT_TRUE(receive_init(*peer.value(), 3, expected));
    EXPECT_TRUE(session.value()->stop());
    EXPECT_TRUE(peer.value()->close());
  }

  void refuse_without_a_server(CloudService service) {
    play::CloudLinkConfiguration broken;
    broken.allocation = play::CloudAllocation{};
    broken.allocation->service = service;
    broken.allocation->launch_spec.assign("synthetic-launch");
    broken.allocation->handshake_key.assign("AAECAwQFBgcICQoLDA0ODw==");
    auto failing =
        play::CloudLinkConfiguration::link(std::move(broken), factory_);
    ASSERT_TRUE(failing);
    LoopbackSink failing_sink;
    failing.value()->set_event_sink(failing_sink);
    const auto started = failing.value()->start();
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error(), remote_play::StreamError::invalid_configuration);
    EXPECT_EQ(http_.calls, 0U);
  }

  auto receive_init(platform::UdpSocket& peer, std::uint8_t wrapper,
                    std::uint32_t version) -> ::testing::AssertionResult {
    const platform::StopToken running;
    auto poller = network_.pollers().create_poller();
    if (!poller) return ::testing::AssertionFailure() << "no poller";
    const std::array<platform::PollRequest, 1> requests{
        {{&peer, 0, platform::PollEvent::readable}}};
    const auto waited =
        poller.value()->wait(requests, std::chrono::seconds{1}, running);
    if (!waited) return ::testing::AssertionFailure() << "the poll failed";
    if (waited.value().ready.empty())
      return ::testing::AssertionFailure() << "nothing arrived";
    std::array<std::byte, 2048> buffer{};
    const auto datagram = peer.receive_from(buffer);
    if (!datagram)
      return ::testing::AssertionFailure() << "the datagram was unreadable";
    const std::span<const std::byte> bytes(buffer.data(),
                                           datagram.value().size);
    const auto unwrapped =
        wrapper
            ? takion::EnvelopeCodec::decode(bytes, takion::PsnEnvelope{wrapper})
            : takion::EnvelopeCodec::decode(bytes, takion::PlainEnvelope{});
    if (!unwrapped)
      return ::testing::AssertionFailure() << "the envelope did not decode";
    const auto profile = takion::resolve_takion_wire_profile(version);
    if (!profile)
      return ::testing::AssertionFailure() << "no profile for " << version;
    const auto plain = takion::PacketCodec::decode_plain_packet(
        unwrapped.value(), profile.value());
    if (!plain)
      return ::testing::AssertionFailure() << "the packet did not decode";
    const auto packet =
        takion::ControlChunkCodec::decode_packet(plain.value().payload);
    if (!packet || !std::holds_alternative<takion::InitChunk>(packet.value()))
      return ::testing::AssertionFailure() << "the packet is not an init";
    return ::testing::AssertionSuccess();
  }

  platform::DefaultNetworkProvider network_{LiveProviders::threading(), LiveProviders::steady()};
  HandshakeCrypto crypto_;
  GkScript http_;
  platform::DefaultPlatformFactory factory_{platform::PlatformProviders{
      .network = &network_, .crypto = &crypto_, .http = &http_}};
};

TEST_F(AllocatedCloudLink, StreamsWithoutProvisioningOnCloudStream) {
  stream(CloudService::cloud_stream, 0);
}

TEST_F(AllocatedCloudLink, StreamsWithoutProvisioningOnLegacyCloudStream) {
  stream(CloudService::legacy_cloud_stream, 0);
}

TEST_F(AllocatedCloudLink, StreamsWithAnOverriddenTakionVersion) {
  stream(CloudService::cloud_stream, 11);
}

TEST_F(AllocatedCloudLink, RefusesAnAllocationWithoutAServerOnCloudStream) {
  refuse_without_a_server(CloudService::cloud_stream);
}

TEST_F(AllocatedCloudLink,
       RefusesAnAllocationWithoutAServerOnLegacyCloudStream) {
  refuse_without_a_server(CloudService::legacy_cloud_stream);
}

}  // namespace
}  // namespace play_test
