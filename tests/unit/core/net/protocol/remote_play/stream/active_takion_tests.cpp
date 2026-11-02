#include "tests/unit/core/net/protocol/remote_play/stream/active_session_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;
using remote_play::ActiveTakionError;

class ActiveTakionStart : public ::testing::Test {
 protected:
  static auto route(std::shared_ptr<ActiveSessionTransportState> state,
                    std::uint32_t version) -> remote_play::EstablishedRoute {
    const auto profile = takion::resolve_takion_wire_profile(version);
    EXPECT_TRUE(profile) << version;
    return ActiveSessionWire::route(std::move(state), version, profile.value());
  }

  auto start(remote_play::EstablishedRoute candidate,
             takion::ClientConfig config = {}) {
    return remote_play::ActiveTakion::start(std::move(candidate), clock,
                                            test_support::process_threading(),
                                            random, events, std::move(config));
  }

  void starts_on_a_calibrated_route(std::uint32_t version) {
    takion::ClientConfig config;
    config.bootstrap_version = 7;
    config.profile_selection = takion::FixedTakionProfile{7};
    auto active = start(route(state, version), config);
    ASSERT_TRUE(active);
    auto& client = active.value()->client();
    EXPECT_EQ(client.snapshot().state, takion::ClientState::handshaking);
    EXPECT_EQ(client.snapshot().active_version, version);
    EXPECT_EQ(active.value()->route().calibration.takion_version, version);
    EXPECT_EQ(active.value()->route().kind,
              remote_play::RouteKind::local_direct);
    EXPECT_EQ(active.value()->route().dont_fragment,
              remote_play::DontFragmentPolicy::enabled);
    EXPECT_EQ(active.value()->route().local_endpoint.port, 50000U);
    EXPECT_EQ(active.value()->route().remote_endpoint.port, 9295U);
    ASSERT_EQ(state->sent.size(), 1U);
    const auto profile = takion::resolve_takion_wire_profile(version);
    ASSERT_TRUE(profile);
    EXPECT_TRUE(
        takion::PacketCodec::decode_plain_packet(state->sent[0], *profile));

    for (auto& reply : ActiveSessionWire::handshake_replies(*profile, true))
      state->incoming.push_back(std::move(reply));
    ASSERT_TRUE(client.pump_once());
    EXPECT_EQ(client.snapshot().state, takion::ClientState::connected);
    EXPECT_GE(events.values.size(), 1U);
    EXPECT_FALSE(client.snapshot().crypto_installed);

    takion::Big big;
    big.client_version = version;
    big.session_key =
        libchiaking::net::protocol::takion::SharedSecretText{"session-id"};
    big.launch_spec = libchiaking::net::protocol::takion::SharedSecretText{
        "encrypted-launch-spec"};
    big.encrypted_key = std::vector<std::byte>(4, std::byte{0});
    const auto before_big = state->sent.size();
    EXPECT_TRUE(client.post(takion::SendControl{std::move(big)}));
    ASSERT_TRUE(client.pump_once());
    ASSERT_GT(state->sent.size(), before_big);
    const auto big_packet = takion::PacketCodec::decode_plain_packet(
        state->sent[before_big], *profile);
    ASSERT_TRUE(big_packet);
    const auto big_chunk =
        takion::ControlChunkCodec::decode_packet(big_packet.value().payload);
    ASSERT_TRUE(big_chunk);
    EXPECT_TRUE(std::holds_alternative<takion::DataChunk>(big_chunk.value()));
    EXPECT_FALSE(client.snapshot().crypto_installed);

    EXPECT_TRUE(client.post(takion::SendControl{takion::Heartbeat{}}));
    const auto before_denied = state->sent.size();
    const auto unencrypted = client.pump_once();
    ASSERT_FALSE(unencrypted);
    EXPECT_EQ(unencrypted.error(), takion::ClientError::crypto_not_installed);
    EXPECT_EQ(client.snapshot().state, takion::ClientState::connected);
    EXPECT_EQ(state->sent.size(), before_denied);
    for (const auto& event : events.values)
      EXPECT_FALSE(
          std::holds_alternative<takion::client_event::TerminalFailure>(event));

    static_cast<void>(client.request_stop());
    EXPECT_EQ(state->close_count, 1U);
    active.value().reset();
    EXPECT_EQ(state->close_count, 1U);
  }

  void splits_big_within_the_measured_mtu(bool cloud, bool wrapped,
                                          std::uint32_t measured) {
    const auto version = cloud ? 12U : 15U;
    const auto resolved = takion::resolve_takion_wire_profile(version);
    ASSERT_TRUE(resolved);
    const auto profile = *resolved;
    takion::ClientConfig config;
    if (wrapped) config.envelope = takion::PsnEnvelope{1};
    auto candidate = route(state, version);
    candidate.calibration.inbound_mtu = measured;
    candidate.calibration.outbound_mtu = measured + 73;
    const remote_play::ActiveDatagramBudget budget =
        cloud ? remote_play::ActiveDatagramBudget{28, 4}
              : remote_play::ActiveDatagramBudget{};
    auto active = remote_play::ActiveTakion::start(
        std::move(candidate), clock, test_support::process_threading(), random,
        events, config, budget);
    ASSERT_TRUE(active);
    auto& client = active.value()->client();
    for (const auto& reply : ActiveSessionWire::handshake_replies(profile)) {
      const auto wire = takion::EnvelopeCodec::encode(reply, config.envelope);
      ASSERT_TRUE(wire);
      state->incoming.push_back(
          std::vector<std::byte>(wire.value().begin(), wire.value().end()));
    }
    ASSERT_TRUE(client.pump_once());
    EXPECT_EQ(client.snapshot().state, takion::ClientState::connected);
    takion::Big big;
    big.client_version = version;
    big.session_key = libchiaking::net::protocol::takion::SharedSecretText{
        "synthetic-session"};
    big.launch_spec = libchiaking::net::protocol::takion::SharedSecretText{
        std::string(4000, 'A')};
    big.encrypted_key = std::vector<std::byte>(4);
    const auto encoded =
        takion::ControlCodec{}.encode(takion::ControlMessage{big.clone()});
    ASSERT_TRUE(encoded);
    const auto before = state->sent.size();
    EXPECT_TRUE(client.post(takion::SendControl{std::move(big)}));
    ASSERT_TRUE(client.pump_once());
    std::size_t position{};
    for (std::size_t index = before; index < state->sent.size(); ++index) {
      const auto plain =
          takion::EnvelopeCodec::decode(state->sent[index], config.envelope);
      ASSERT_TRUE(plain) << index;
      const auto packet =
          takion::PacketCodec::decode_plain_packet(plain.value(), profile);
      ASSERT_TRUE(packet) << index;
      const auto chunk =
          takion::ControlChunkCodec::decode_packet(packet.value().payload);
      ASSERT_TRUE(chunk) << index;
      ASSERT_TRUE(std::holds_alternative<takion::DataChunk>(chunk.value()))
          << index;
      const auto& data = std::get<takion::DataChunk>(chunk.value());
      const bool first = index == before;
      const std::size_t reference_payload_budget =
          measured - (cloud ? 28U : 50U) -
          (cloud ? (first ? 30U : 29U) : (first ? 26U : 25U));
      const auto size =
          std::min(reference_payload_budget, encoded.value().size() - position);
      const std::size_t prefix = first ? 9 : 8;
      ASSERT_EQ(data.payload.size(), prefix + size) << index;
      EXPECT_TRUE(std::equal(
          encoded.value().begin() + static_cast<std::ptrdiff_t>(position),
          encoded.value().begin() +
              static_cast<std::ptrdiff_t>(position + size),
          data.payload.begin() + static_cast<std::ptrdiff_t>(prefix)))
          << index;
      position += size;
      EXPECT_EQ(data.header.flags, position == encoded.value().size() ? 1U : 0U)
          << index;
      EXPECT_LE(state->sent[index].size(), measured - (cloud ? 28U : 50U))
          << index;
    }
    EXPECT_EQ(position, encoded.value().size());
    EXPECT_TRUE(client.request_stop());
    EXPECT_EQ(state->close_count, 1U);
  }

  std::shared_ptr<ActiveSessionTransportState> state{
      std::make_shared<ActiveSessionTransportState>()};
  test_support::FakeClock clock{1'000};
  ActiveSessionRandom random;
  ActiveTakionEvents events;
};

TEST_F(ActiveTakionStart, StartsOnACalibratedBootstrapRoute) {
  starts_on_a_calibrated_route(7);
}

TEST_F(ActiveTakionStart, StartsOnACalibratedVersion20Route) {
  starts_on_a_calibrated_route(20);
}

TEST_F(ActiveTakionStart, ConsoleBigFitsA600ByteMeasuredMtu) {
  splits_big_within_the_measured_mtu(false, false, 600);
}

TEST_F(ActiveTakionStart, ConsoleBigFitsA1454ByteMeasuredMtu) {
  splits_big_within_the_measured_mtu(false, false, 1454);
}

TEST_F(ActiveTakionStart, CloudBigFitsA600ByteMeasuredMtu) {
  splits_big_within_the_measured_mtu(true, false, 600);
}

TEST_F(ActiveTakionStart, CloudBigFitsA1454ByteMeasuredMtu) {
  splits_big_within_the_measured_mtu(true, false, 1454);
}

TEST_F(ActiveTakionStart, WrappedCloudBigFitsA600ByteMeasuredMtu) {
  splits_big_within_the_measured_mtu(true, true, 600);
}

TEST_F(ActiveTakionStart, WrappedCloudBigFitsA1454ByteMeasuredMtu) {
  splits_big_within_the_measured_mtu(true, true, 1454);
}

TEST_F(ActiveTakionStart, RefusesAnMtuTooSmallForTheOverheads) {
  for (const std::uint32_t mtu : {1U, 49U, 50U, 98U}) {
    auto small_state = std::make_shared<ActiveSessionTransportState>();
    auto candidate = route(small_state, 15);
    candidate.calibration.inbound_mtu = mtu;
    const auto active = start(std::move(candidate));
    ASSERT_FALSE(active) << mtu;
    EXPECT_EQ(active.error(), ActiveTakionError::invalid_mtu) << mtu;
    EXPECT_TRUE(small_state->sent.empty()) << mtu;
    EXPECT_EQ(small_state->close_count, 1U) << mtu;
  }
}

TEST_F(ActiveTakionStart, InvalidRoutesCloseOnceWithoutSending) {
  auto mismatch_state = std::make_shared<ActiveSessionTransportState>();
  auto mismatched = route(mismatch_state, 20);
  mismatched.calibration.takion_version = 7;
  const auto mismatch = start(std::move(mismatched));
  ASSERT_FALSE(mismatch);
  EXPECT_EQ(mismatch.error(), ActiveTakionError::version_mismatch);
  EXPECT_TRUE(mismatch_state->sent.empty());
  EXPECT_EQ(mismatch_state->close_count, 1U);

  auto mtu_state = std::make_shared<ActiveSessionTransportState>();
  auto no_mtu = route(mtu_state, 7);
  no_mtu.calibration.outbound_mtu = 0;
  const auto invalid_mtu = start(std::move(no_mtu));
  ASSERT_FALSE(invalid_mtu);
  EXPECT_EQ(invalid_mtu.error(), ActiveTakionError::invalid_mtu);
  EXPECT_TRUE(mtu_state->sent.empty());
  EXPECT_EQ(mtu_state->close_count, 1U);

  auto missing = route(std::make_shared<ActiveSessionTransportState>(), 7);
  missing.transport.reset();
  const auto invalid = start(std::move(missing));
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), ActiveTakionError::invalid_route);
}

TEST_F(ActiveTakionStart, StartupFailureClosesOnce) {
  random.fail = true;
  const auto failed = start(route(state, 7));
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), ActiveTakionError::start_failed);
  EXPECT_TRUE(state->sent.empty());
  EXPECT_EQ(state->close_count, 1U);

  auto bad_config_state = std::make_shared<ActiveSessionTransportState>();
  takion::ClientConfig config;
  config.limits.mailbox_capacity = 0;
  const auto bad_config = start(route(bad_config_state, 7), config);
  ASSERT_FALSE(bad_config);
  EXPECT_EQ(bad_config.error(), ActiveTakionError::start_failed);
  EXPECT_TRUE(bad_config_state->sent.empty());
  EXPECT_EQ(bad_config_state->close_count, 1U);
}

}  // namespace
}  // namespace remote_play_test
