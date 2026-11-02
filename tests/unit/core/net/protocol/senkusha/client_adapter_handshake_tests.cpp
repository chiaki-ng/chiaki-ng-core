#include "tests/unit/core/net/protocol/senkusha/client_adapter_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <variant>
#include <vector>

namespace senkusha_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class SenkushaClientHandshake : public SenkushaClientRig {
 protected:
  SenkushaHarness harness;
};

TEST_F(SenkushaClientHandshake, NegotiatesThenSendsTheLocalBigAndAcceptsABang) {
  auto subject = client(harness);
  ASSERT_TRUE(subject->start());
  queue_handshake(harness.state());
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::negotiating_version);

  const auto request_message =
      decode_outbound_control(harness.state().sent.back(), 7, 0x11223344);
  ASSERT_TRUE(request_message);
  const auto* request = std::get_if<takion::ProtocolRequest>(&*request_message);
  ASSERT_NE(request, nullptr);
  EXPECT_EQ(request->supported_versions,
            (std::vector<std::uint32_t>{12, 15, 18, 20}));

  harness.state().incoming.push_back(
      control_packet(takion::DataAcknowledgementChunk{
          takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 0},
          takion::DataAcknowledgement{0x11223344, 0x19000, {}, {}},
      }));
  auto peer_channel = peer(0xa1b2c3d4);
  queue_reliable_control(harness.state(), peer_channel,
                         takion::ProtocolRequestAck{20}, 7);
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::awaiting_bang);

  ASSERT_TRUE(subject->pump_once());
  const auto big_message =
      decode_outbound_control(harness.state().sent.back(), 7, 0x11223345);
  ASSERT_TRUE(big_message);
  const auto* big = std::get_if<takion::Big>(&*big_message);
  ASSERT_NE(big, nullptr);
  EXPECT_EQ(big->client_version, 9U);
  EXPECT_TRUE(big->session_key.empty());
  EXPECT_TRUE(big->launch_spec.empty());
  EXPECT_TRUE(big->encrypted_key.empty());

  queue_reliable_control(harness.state(), peer_channel, local_bang(20), 7);
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::enabling_echo);
  EXPECT_EQ(harness.events.failures(), 0U);
}

TEST_F(SenkushaClientHandshake, SkipsNegotiationOnAFixedProfile) {
  ClientConfig config;
  config.profile.profile_selection = takion::FixedTakionProfile{12};
  auto subject = client(harness, std::move(config));
  ASSERT_TRUE(subject->start());
  queue_handshake(harness.state());
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::awaiting_bang);
  ASSERT_TRUE(subject->pump_once());

  const auto big_message =
      decode_outbound_control(harness.state().sent.back(), 12, 0x11223344);
  ASSERT_TRUE(big_message);
  const auto* big = std::get_if<takion::Big>(&*big_message);
  ASSERT_NE(big, nullptr);
  EXPECT_EQ(big->client_version, 9U);

  auto peer_channel = peer(0xa1b2c3d4);
  queue_reliable_control(harness.state(), peer_channel, local_bang(12), 12);
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::enabling_echo);
  EXPECT_EQ(harness.events.failures(), 0U);
}

TEST_F(SenkushaClientHandshake, FailsAtOnceWhenTheEchoCommandCannotBeSent) {
  auto subject = client(harness);
  ASSERT_NO_FATAL_FAILURE(reach_local_rtt_measurement(*subject, harness, true));
  harness.state().fail_send = true;
  const auto pumped = subject->pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), Error::takion_failure);
  EXPECT_EQ(subject->snapshot().state, State::failed);
  EXPECT_EQ(harness.events.failures(), 1U);
}

}  // namespace
}  // namespace senkusha_test
