#include "tests/support/clock.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/cloud/links/connect_rig.hpp"
#include "tests/support/crypto/refusing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <variant>

namespace cloud_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using ConnectReason = ConnectRig::Reason;
using ConnectStage = ConnectRig::Stage;

TEST(ServerConnectTakionConfig, WrapsWhenAsked) {
  const auto wrapped = play::ServerConnect::takion_config(1);
  EXPECT_TRUE(std::holds_alternative<takion::PsnEnvelope>(wrapped.envelope));
  ASSERT_TRUE(wrapped.receive_envelope);
  EXPECT_TRUE(
      std::holds_alternative<takion::PlainEnvelope>(*wrapped.receive_envelope));
}

TEST(ServerConnectTakionConfig, SendsPlainWithoutAWrapper) {
  const auto plain = play::ServerConnect::takion_config(0);
  EXPECT_TRUE(std::holds_alternative<takion::PlainEnvelope>(plain.envelope));
}

class ServerConnectControlProfile : public PinnedTakionVersions {
 protected:
  using AudioAssemblyPolicy = remote_play::AudioAssemblyPolicy;
};

TEST_F(ServerConnectControlProfile, PlaysTheLegacyServiceLikeAConsole) {
  const auto profile = play::ServerConnect::control_profile(
      CloudService::legacy_cloud_stream, {});
  EXPECT_EQ(profile.controller_type,
            takion::ControllerConnectionType::dualsense);
  EXPECT_FALSE(profile.enable_idr_on_fec_failure);
  EXPECT_FALSE(profile.hevc);
  EXPECT_EQ(profile.audio_assembly, AudioAssemblyPolicy::console_units);
  EXPECT_EQ(profile.peer_silence_timeout_us, 20'000'000);
}

TEST_F(ServerConnectControlProfile, FollowsTheCallerForCloudStream) {
  const auto profile = play::ServerConnect::control_profile(
      CloudService::cloud_stream, {false, true});
  EXPECT_EQ(profile.peer_silence_timeout_us, 20'000'000);
  EXPECT_EQ(profile.controller_type,
            takion::ControllerConnectionType::dualshock4);
  EXPECT_TRUE(profile.enable_idr_on_fec_failure);
  EXPECT_TRUE(profile.hevc);
  EXPECT_EQ(profile.audio_assembly, AudioAssemblyPolicy::cloud_generation);
  EXPECT_EQ(profile.datagram_budget.network_overhead, 28);
  EXPECT_EQ(profile.datagram_budget.reserved_envelope_bytes, 4);
  EXPECT_TRUE(profile.takion_offer.empty());
}

TEST_F(ServerConnectControlProfile, OffersTheCloudVersionsWhenNegotiating) {
  play::ServerConnect::Options negotiating{};
  negotiating.tuning.negotiate_takion_version = true;
  const auto profile = play::ServerConnect::control_profile(
      CloudService::cloud_stream, negotiating);
  EXPECT_EQ(profile.takion_offer,
            takion::takion_negotiation_offer(
                takion::SessionScope::cloud_stream));
  EXPECT_NE(std::ranges::find(profile.takion_offer, 25U),
            profile.takion_offer.end());
}

TEST_F(ServerConnectControlProfile,
       OffersNoCloudOnlyVersionToTheLegacyService) {
  play::ServerConnect::Options negotiating{};
  negotiating.tuning.negotiate_takion_version = true;
  const auto profile = play::ServerConnect::control_profile(
      CloudService::legacy_cloud_stream, negotiating);
  EXPECT_EQ(profile.takion_offer,
            takion::takion_negotiation_offer(
                takion::SessionScope::legacy_cloud_stream));
  ASSERT_FALSE(profile.takion_offer.empty());
  EXPECT_EQ(profile.takion_offer.back(), 20U);
  EXPECT_EQ(std::ranges::find(profile.takion_offer, 21U),
            profile.takion_offer.end());
  EXPECT_EQ(std::ranges::find(profile.takion_offer, 23U),
            profile.takion_offer.end());
  EXPECT_EQ(std::ranges::find(profile.takion_offer, 25U),
            profile.takion_offer.end());
}

class ServerConnectStart : public PinnedTakionVersions,
                           public ::testing::WithParamInterface<CloudService> {
 protected:
  void SetUp() override {
    PinnedTakionVersions::SetUp();
    if (IsSkipped()) return;
    const auto resolved = takion::resolve_takion_wire_profile(version());
    ASSERT_TRUE(resolved);
    profile.emplace(*resolved);
    credentials.psn_wrapper_type = 1;
  }

  auto version() const -> std::uint32_t {
    return GetParam() == CloudService::cloud_stream
               ? takion::takion_version_cloud_stream
               : takion::takion_version_legacy_cloud_stream;
  }

  auto route(
      remote_play::RouteKind kind = remote_play::RouteKind::cloud_allocated)
      -> remote_play::EstablishedRoute {
    return remote_play::EstablishedRoute{
        {std::make_unique<FakeTransport>(state),
         {platform::IpAddress::loopback_ipv4(), 50000},
         {platform::IpAddress::loopback_ipv4(), 9295},
         kind,
         remote_play::DontFragmentPolicy::disabled},
        {version(), {}, 1454, 1254, *profile}};
  }

  auto start(play::ServerConnect::Options options = {}) {
    return connector.start(route(), credentials, GetParam(), options);
  }

  std::optional<takion::WireProfile> profile;
  platform::SteadyClock steady;
  platform::DefaultNetworkProvider network{test_support::process_threading(),
                                           steady};
  test_support::FakeClock clock{1'000};
  test_support::FixedRandom random;
  FakeCrypto crypto;
  test_support::RefusingTakionCrypto takion_crypto;
  play::ServerConnect connector{{network, clock,
                                 test_support::process_threading(), random,
                                 crypto, takion_crypto}};
  libchiaking::psn::gk::Allocation credentials = ConnectRig::allocation();
  std::shared_ptr<TransportState> state = std::make_shared<TransportState>();
};

TEST_P(ServerConnectStart, ConnectsTheChosenControllerType) {
  const auto started = start({false, true});
  ASSERT_TRUE(started);
  EXPECT_EQ(started.value().session->pad_controller_type(0),
            takion::ControllerConnectionTypeValue{
                takion::ControllerConnectionType::dualshock4});
  EXPECT_TRUE(started.value().session->request_stop());
}

TEST_P(ServerConnectStart, SendsTheWrappedHandshakeAndClosesOnStop) {
  auto started = start();
  ASSERT_TRUE(started);
  EXPECT_EQ(started.value().session->pad_controller_type(3),
            takion::ControllerConnectionTypeValue{
                takion::ControllerConnectionType::dualsense});
  ASSERT_EQ(state->sent.size(), std::size_t{1});
  EXPECT_EQ(state->closes, 0U);
  EXPECT_EQ(started.value().session->snapshot().active_version, version());
  EXPECT_TRUE(takion::EnvelopeCodec::decode(state->sent.front(),
                                            takion::PsnEnvelope{1}));
  EXPECT_TRUE(started.value().session->request_stop());
  EXPECT_EQ(state->closes, 1U);
  started.value().session.reset();
  EXPECT_EQ(state->closes, 1U);
}

TEST_P(ServerConnectStart, SendsAPlainHandshakeWithoutAWrapper) {
  credentials.psn_wrapper_type = 0;
  const auto started = start();
  ASSERT_TRUE(started);
  ASSERT_EQ(state->sent.size(), std::size_t{1});
  EXPECT_TRUE(takion::EnvelopeCodec::decode(state->sent.front(),
                                            takion::PlainEnvelope{}));
  EXPECT_TRUE(started.value().session->request_stop());
  EXPECT_EQ(state->closes, 1U);
}

TEST_P(ServerConnectStart, ClosesTheRouteForAnUndecodableKey) {
  credentials.psn_wrapper_type = 0;
  credentials.handshake_key.assign("invalid");
  EXPECT_TRUE(ConnectRig::fails_with(start(), ConnectStage::active,
                                     ConnectReason::handshake_key_length));
  EXPECT_TRUE(state->sent.empty());
  EXPECT_EQ(state->closes, 1U);
}

TEST_P(ServerConnectStart, ClosesALocalRoute) {
  credentials.psn_wrapper_type = 0;
  credentials.handshake_key.assign("invalid");
  EXPECT_TRUE(ConnectRig::fails_with(
      connector.start(route(remote_play::RouteKind::local_direct), credentials,
                      GetParam()),
      ConnectStage::active, ConnectReason::invalid_route));
  EXPECT_TRUE(state->sent.empty());
  EXPECT_EQ(state->closes, 1U);
}

TEST_P(ServerConnectStart, StartsDespiteAFailedFirstSend) {
  credentials = ConnectRig::allocation();
  state->fail_send = true;
  const auto started = start();
  ASSERT_TRUE(started);
  EXPECT_FALSE(started.value().session->failure());
  EXPECT_TRUE(state->sent.empty());
  EXPECT_EQ(state->closes, 1U);
}

INSTANTIATE_TEST_SUITE_P(CloudServices, ServerConnectStart,
                         ::testing::Values(CloudService::legacy_cloud_stream,
                                           CloudService::cloud_stream));

class ServerConnectConnection : public PinnedTakionVersions {
 protected:
  void SetUp() override {
    PinnedTakionVersions::SetUp();
    if (IsSkipped()) return;
    datagram::UdpTransportConfig peer_config;
    peer_config.local = {platform::IpAddress::loopback_ipv4(), 0};
    auto created = datagram::ProviderUdpTransport::create(network, peer_config);
    ASSERT_TRUE(created);
    peer = std::move(created.value());
    const auto peer_endpoint = peer->local_endpoint();
    ASSERT_TRUE(peer_endpoint);
    selected.measured = true;
    selected.rtt_ms = 20;
    selected.mtu_in = 1454;
    selected.mtu_out = 1254;
    provisioned.server_ip = "127.0.0.1";
    provisioned.server_port = peer_endpoint.value().port;
  }

  auto calibration() const {
    return play::Calibration::from_ping(selected,
                                        CloudService::legacy_cloud_stream);
  }

  platform::SteadyClock steady;
  platform::DefaultNetworkProvider network{test_support::process_threading(),
                                           steady};
  std::unique_ptr<datagram::ProviderUdpTransport> peer;
  libchiaking::psn::gk::Ping selected;
  libchiaking::psn::gk::Allocation provisioned = ConnectRig::allocation();
  test_support::FakeClock clock{1'000};
  test_support::FixedRandom random;
  FakeCrypto crypto;
  test_support::RefusingTakionCrypto takion_crypto;
  play::ServerConnect connector{{network, clock,
                                 test_support::process_threading(), random,
                                 crypto, takion_crypto}};
};

TEST_F(ServerConnectConnection, SendsTheWrappedHandshakeToTheAllocatedServer) {
  const auto measured = calibration();
  ASSERT_TRUE(measured);
  EXPECT_EQ(measured.value().service(), CloudService::legacy_cloud_stream);
  const auto active = connector.connect(provisioned, measured.value());
  ASSERT_TRUE(active);
  EXPECT_TRUE(active.value().session);
  EXPECT_TRUE(active.value().pollable);
  const platform::StopToken running;
  const auto ready = peer->wait_readable(std::chrono::seconds{1}, running);
  ASSERT_TRUE(ready);
  EXPECT_TRUE(ready.value());
  const auto received = peer->receive();
  ASSERT_TRUE(received);
  ASSERT_TRUE(received.value());
  EXPECT_TRUE(
      takion::EnvelopeCodec::decode(*received.value(), takion::PsnEnvelope{1}));
  EXPECT_TRUE(active.value().session->request_stop());
}

TEST_F(ServerConnectConnection, RefusesAnAllocationWithoutAHost) {
  const auto measured = calibration();
  ASSERT_TRUE(measured);
  provisioned.server_ip.clear();
  const auto active = connector.connect(provisioned, measured.value());
  ASSERT_FALSE(active);
  EXPECT_EQ(active.error().stage(), ConnectStage::route);
  EXPECT_TRUE(ConnectRig::failed_with(active.error(), ConnectStage::route,
                                      ConnectReason::invalid_host));
}

TEST_F(ServerConnectConnection, RefusesAnUndecodableKey) {
  const auto measured = calibration();
  ASSERT_TRUE(measured);
  provisioned.handshake_key.assign("invalid");
  const auto active = connector.connect(provisioned, measured.value());
  ASSERT_FALSE(active);
  EXPECT_EQ(active.error().stage(), ConnectStage::active);
  EXPECT_TRUE(ConnectRig::failed_with(active.error(), ConnectStage::active,
                                      ConnectReason::handshake_key_length));
}

TEST_F(ServerConnectConnection, CalibratesNothingUnmeasured) {
  selected.measured = false;
  EXPECT_TRUE(ConnectRig::fails_with(calibration(), ConnectStage::route,
                                     ConnectReason::not_measured));
}

}  // namespace
}  // namespace cloud_test
