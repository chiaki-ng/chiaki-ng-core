#include "tests/unit/core/net/protocol/remote_play/stream/active_session_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace remote_play_test {
namespace {

namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;
using remote_play::ActiveHandshakeState;

class ActiveSessionNegotiation : public ::testing::Test {
 protected:
  enum class Reply : std::uint8_t {
    none,
    accepted,
    silent,
    unoffered,
    absent,
    disallowed
  };

  static auto offer(takion::SessionScope scope) -> std::vector<std::uint32_t> {
    const auto offered = takion::takion_negotiation_offer(scope);
    return {offered.begin(), offered.end()};
  }

  void negotiate(std::uint32_t pinned, std::vector<std::uint32_t> offered,
                 Reply reply, std::uint32_t selected) {
    rig.emplace(pinned);
    const auto& profile = rig->profile;
    const auto pinned_curve =
        profile.ecdh_curve() == platform::EcdhCurve::secp521r1
            ? platform::EcdhCurve::secp521r1
            : platform::EcdhCurve::secp256k1;
    remote_play::ActiveControlProfile control;
    control.takion_offer = offered;
    libchiaking::SecretArray<16> handshake_key;
    auto started = remote_play::ActiveSession::start(
        ActiveSessionWire::route(rig->state, pinned, profile,
                                 remote_play::RouteKind::cloud_allocated,
                                 remote_play::DontFragmentPolicy::disabled),
        rig->clock, test_support::process_threading(), rig->random, crypto,
        rig->takion_crypto, std::move(handshake_key),
        ActiveSessionWire::prepared(pinned_curve),
        ActiveSessionWire::secret("session-id"),
        ActiveSessionWire::secret("launch-spec"), {}, control);
    ASSERT_TRUE(started);
    auto& owner = *started.value();
    rig->answer_handshake();
    ASSERT_TRUE(owner.pump_once());
    ASSERT_EQ(rig->state->sent.size(), 3U);
    const auto first =
        ActiveSessionWire::sent_message(rig->state->sent[2], profile);
    ASSERT_TRUE(first);
    if (reply == Reply::none) {
      ASSERT_TRUE(std::holds_alternative<takion::Big>(first->second));
      EXPECT_EQ(std::get<takion::Big>(first->second).client_version, pinned);
      EXPECT_EQ(owner.committed_takion_version(), pinned);
      EXPECT_TRUE(crypto.generated.empty());
      return;
    }
    const auto* request = std::get_if<takion::ProtocolRequest>(&first->second);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->supported_versions, offered);
    EXPECT_EQ(owner.state(), ActiveHandshakeState::awaiting_profile);
    EXPECT_FALSE(owner.committed_takion_version());
    rig->state->incoming.push_back(
        ActiveSessionWire::acknowledgement(first->first, profile));
    if (reply != Reply::silent) {
      const auto ack = reply == Reply::accepted || reply == Reply::disallowed
                           ? takion::ProtocolRequestAck{selected}
                       : reply == Reply::unoffered
                           ? takion::ProtocolRequestAck{12}
                           : takion::ProtocolRequestAck{};
      rig->state->incoming.push_back(
          ActiveSessionWire::inbound_message(ack, 0xa1b2c3d4, profile));
    }
    ASSERT_TRUE(owner.pump_once());
    if (reply == Reply::silent) {
      EXPECT_FALSE(owner.committed_takion_version());
      rig->clock.now_us += remote_play::takion_negotiation_timeout_us + 1;
      ASSERT_TRUE(owner.pump_once());
    }
    const auto expected = reply == Reply::accepted ? selected : pinned;
    EXPECT_EQ(owner.committed_takion_version(), expected);
    EXPECT_EQ(owner.snapshot().active_version, expected);
    EXPECT_EQ(owner.state(), ActiveHandshakeState::awaiting_bang);
    const auto committed = takion::resolve_takion_wire_profile(expected);
    ASSERT_TRUE(committed);
    std::optional<takion::Big> big;
    for (std::size_t index = 3; index < rig->state->sent.size() && !big;
         ++index) {
      if (const auto message = ActiveSessionWire::sent_message(
              rig->state->sent[index], *committed))
        if (const auto* value = std::get_if<takion::Big>(&message->second))
          big = value->clone();
    }
    ASSERT_TRUE(big);
    EXPECT_EQ(big->client_version, expected);
    const bool p521 = committed->ecdh_curve() == platform::EcdhCurve::secp521r1;
    ASSERT_TRUE(big->ecdh_pub_key);
    EXPECT_EQ(big->ecdh_pub_key->size(), p521 ? 133U : 65U);
    EXPECT_EQ(crypto.generated.size(),
              committed->ecdh_curve() == profile.ecdh_curve() ? 0U : 1U);
    EXPECT_TRUE(owner.request_stop());
  }

  std::optional<ActiveSessionRig> rig;
  ActiveSessionNegotiationCrypto crypto;
  const std::vector<std::uint32_t> cloud_offer =
      offer(takion::SessionScope::cloud_stream);
  const std::vector<std::uint32_t> console_offer =
      offer(takion::SessionScope::console);
  const std::vector<std::uint32_t> legacy_cloud_stream_offer =
      offer(takion::SessionScope::legacy_cloud_stream);
};

TEST_F(ActiveSessionNegotiation, WithoutAnOfferCloudStreamSendsBigAtVersion23) {
  negotiate(23, {}, Reply::none, 0);
}

TEST_F(ActiveSessionNegotiation, WithoutAnOfferVersion12SendsBigDirectly) {
  negotiate(12, {}, Reply::none, 0);
}

TEST_F(ActiveSessionNegotiation, CommitsVersion20AcceptedFromVersion12) {
  negotiate(12, cloud_offer, Reply::accepted, 20);
}

TEST_F(ActiveSessionNegotiation, CommitsVersion25AcceptedFromVersion23) {
  negotiate(23, cloud_offer, Reply::accepted, 25);
}

TEST_F(ActiveSessionNegotiation, CommitsVersion11AcceptedFromVersion23) {
  negotiate(23, cloud_offer, Reply::accepted, 11);
}

TEST_F(ActiveSessionNegotiation, KeepsThePinnedVersionWhenTheConsoleIsSilent) {
  negotiate(23, cloud_offer, Reply::silent, 0);
}

TEST_F(ActiveSessionNegotiation,
       KeepsThePinnedVersionWhenAnUnofferedOneIsSelected) {
  negotiate(23, cloud_offer, Reply::unoffered, 0);
}

TEST_F(ActiveSessionNegotiation,
       KeepsThePinnedVersionWhenTheSelectionIsAbsent) {
  negotiate(23, cloud_offer, Reply::absent, 0);
}

TEST_F(ActiveSessionNegotiation, ConsoleCommitsVersion15AcceptedFromVersion20) {
  negotiate(20, console_offer, Reply::accepted, 15);
}

TEST_F(ActiveSessionNegotiation, ConsoleKeepsVersion9WhenThePeerIsSilent) {
  negotiate(9, console_offer, Reply::silent, 0);
}

TEST_F(ActiveSessionNegotiation, WithoutAnOfferVersion20SendsBigDirectly) {
  negotiate(20, {}, Reply::none, 0);
}

TEST_F(ActiveSessionNegotiation, WithoutAnOfferVersion9SendsBigDirectly) {
  negotiate(9, {}, Reply::none, 0);
}

TEST_F(ActiveSessionNegotiation, LegacyCloudStreamCommitsVersion20Accepted) {
  negotiate(12, legacy_cloud_stream_offer, Reply::accepted, 20);
}

TEST_F(ActiveSessionNegotiation,
       LegacyCloudStreamKeepsVersion12OverADisallowedOne) {
  negotiate(12, legacy_cloud_stream_offer, Reply::disallowed, 21);
}

TEST_F(ActiveSessionNegotiation, ConsoleKeepsVersion20OverADisallowedOne) {
  negotiate(20, console_offer, Reply::disallowed, 25);
}

class ActiveSessionProfileCommit : public ::testing::Test {
 protected:
  void post_big_after_commit(std::uint32_t version) {
    ActiveSessionRig rig{version};
    ASSERT_NO_FATAL_FAILURE(rig.start({}, {},
                                      version == 20
                                          ? platform::EcdhCurve::secp521r1
                                          : platform::EcdhCurve::secp256k1));
    ASSERT_EQ(rig.state->sent.size(), 1U);
    EXPECT_EQ(rig.owner->state(), ActiveHandshakeState::awaiting_connection);
    rig.answer_handshake(true);
    ASSERT_TRUE(rig.owner->pump_once());
    EXPECT_EQ(rig.owner->snapshot().state, takion::ClientState::connected);
    EXPECT_EQ(rig.owner->state(), ActiveHandshakeState::awaiting_bang);
    EXPECT_EQ(rig.owner->snapshot().queued_commands, 0U);
    ASSERT_GT(rig.state->sent.size(), 2U);
    const auto big_packet = takion::PacketCodec::decode_plain_packet(
        rig.state->sent[2], rig.profile);
    ASSERT_TRUE(big_packet);
    const auto big_chunk =
        takion::ControlChunkCodec::decode_packet(big_packet.value().payload);
    ASSERT_TRUE(big_chunk);
    EXPECT_TRUE(std::holds_alternative<takion::DataChunk>(big_chunk.value()));
    const auto big_datagram = rig.state->sent[2];
    EXPECT_TRUE(rig.owner->request_stop());
    EXPECT_EQ(rig.state->close_count, 1U);
    rig.owner.reset();
    EXPECT_EQ(rig.state->close_count, 1U);
    EXPECT_FALSE(big_datagram.empty());
  }
};

TEST_F(ActiveSessionProfileCommit, PostsBigAfterTheBootstrapVersionConnects) {
  post_big_after_commit(7);
}

TEST_F(ActiveSessionProfileCommit, PostsBigAfterVersion20Connects) {
  post_big_after_commit(20);
}

}  // namespace
}  // namespace remote_play_test
