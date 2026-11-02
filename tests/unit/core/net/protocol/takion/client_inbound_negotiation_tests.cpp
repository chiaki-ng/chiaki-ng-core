#include "tests/unit/core/net/protocol/takion/client_inbound_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace takion_test {
namespace {

class TakionInboundNegotiation : public ::testing::Test {};

TEST_F(TakionInboundNegotiation,
       RejectsAMissingProfileWithoutFailingTheClient) {
  InboundHarness harness{InboundWire::negotiated_config()};
  harness.host.connected = true;

  takion::client_detail::InboundSession session{nullptr, &harness.handshake,
                                                &harness.reliable, true};
  harness.receive(InboundWire::data_datagram(1, harness.config.control_channel,
                                             InboundWire::encoded_ack(20),
                                             harness.config.bootstrap_version),
                  session);

  const auto rejection = harness.events.single_rejection();
  ASSERT_TRUE(rejection);
  EXPECT_EQ(rejection, takion::ClientError::invalid_configuration);
  EXPECT_TRUE(harness.host.failures.empty());
}

TEST_F(TakionInboundNegotiation, CommitsTheSelectedVersionOnce) {
  InboundHarness harness{InboundWire::negotiated_config()};

  harness.deliver(1, harness.config.bootstrap_version,
                  InboundWire::encoded_ack(20));
  EXPECT_TRUE(harness.host.failures.empty());

  const auto ready = harness.inbound.take_ready_commit(&harness.reliable);
  ASSERT_TRUE(ready);
  EXPECT_EQ(ready, 20U);
  EXPECT_FALSE(harness.inbound.take_ready_commit(&harness.reliable));

  harness.deliver(2, harness.config.bootstrap_version,
                  InboundWire::encoded_ack(15));
  EXPECT_FALSE(harness.inbound.take_ready_commit(&harness.reliable));
}

TEST_F(TakionInboundNegotiation, CommitsOnlyWhenTheChannelIsQuiet) {
  InboundHarness quiet{InboundWire::negotiated_config()};
  quiet.deliver(1, quiet.config.bootstrap_version,
                InboundWire::encoded_ack(20));
  const auto sent = quiet.reliable.send(
      quiet.config.control_channel, takion::MessageDataType{0x00},
      std::vector<std::byte>{std::byte{0x01}}, quiet.clock.now_us);
  ASSERT_TRUE(sent.has_value());
  EXPECT_NE(quiet.reliable.outstanding_fragment_count(), 0U);
  EXPECT_FALSE(quiet.inbound.take_ready_commit(&quiet.reliable));

  const auto acknowledged = quiet.reliable.acknowledge(
      takion::DataAcknowledgement{
          100 + static_cast<std::uint32_t>(sent->fragments.size()) - 1,
          takion::default_advertised_receive_window,
          {},
          {},
      },
      quiet.clock.now_us);
  EXPECT_TRUE(acknowledged.has_value());
  EXPECT_EQ(quiet.reliable.outstanding_fragment_count(), 0U);
  const auto unblocked = quiet.inbound.take_ready_commit(&quiet.reliable);
  ASSERT_TRUE(unblocked);
  EXPECT_EQ(unblocked, 20U);
}

TEST_F(TakionInboundNegotiation,
       AnOutstandingRequestBlocksTheCommitUntilAcknowledged) {
  InboundHarness harness{InboundWire::negotiated_config()};
  harness.inbound.note_version_request_sent(0);

  harness.deliver(1, harness.config.bootstrap_version,
                  InboundWire::encoded_ack(20));
  EXPECT_FALSE(harness.inbound.take_ready_commit(&harness.reliable));

  harness.acknowledge_request();

  const auto ready = harness.inbound.take_ready_commit(&harness.reliable);
  ASSERT_TRUE(ready);
  EXPECT_EQ(ready, 20U);
}

TEST_F(TakionInboundNegotiation,
       AnUnsupportedSelectionKeepsTheBootstrapVersion) {
  InboundHarness harness{InboundWire::negotiated_config()};

  harness.deliver(1, harness.config.bootstrap_version,
                  InboundWire::encoded_ack(9));

  EXPECT_TRUE(harness.host.failures.empty());
  const auto ready = harness.inbound.take_ready_commit(&harness.reliable);
  ASSERT_TRUE(ready);
  EXPECT_EQ(ready, harness.config.bootstrap_version);
}

TEST_F(TakionInboundNegotiation,
       AnAnswerWithoutAVersionKeepsTheBootstrapVersion) {
  InboundHarness versionless{InboundWire::negotiated_config()};
  versionless.deliver(1, versionless.config.bootstrap_version,
                      InboundWire::encoded_control(takion::ControlMessage{
                          takion::ProtocolRequestAck{}}));
  EXPECT_TRUE(versionless.host.failures.empty());
  const auto fallback =
      versionless.inbound.take_ready_commit(&versionless.reliable);
  ASSERT_TRUE(fallback);
  EXPECT_EQ(fallback, versionless.config.bootstrap_version);
}

TEST_F(TakionInboundNegotiation,
       ARequestIsAnsweredWithTheHighestNegotiableVersion) {
  InboundHarness harness{InboundWire::negotiated_config()};

  harness.deliver(1, harness.config.bootstrap_version,
                  InboundWire::encoded_request({9, 15, 12, 20}));

  ASSERT_EQ(harness.host.controls.size(), 1U);
  const auto* reply =
      std::get_if<takion::ProtocolRequestAck>(&harness.host.controls.front());
  ASSERT_NE(reply, nullptr);
  ASSERT_TRUE(reply->selected_version);
  EXPECT_EQ(*reply->selected_version, 20U);
  EXPECT_TRUE(harness.host.failures.empty());
}

TEST_F(TakionInboundNegotiation, FallsBackStrictlyAfterTheTimeout) {
  auto config = InboundWire::negotiated_config();
  config.negotiation_timeout_us = 1'000'000;
  InboundHarness harness{config};
  harness.inbound.note_version_request_sent(5'000);
  harness.acknowledge_request();
  harness.inbound.expire_negotiation(4'000);
  harness.inbound.expire_negotiation(1'005'000);
  EXPECT_FALSE(harness.inbound.take_ready_commit(&harness.reliable));
  harness.inbound.expire_negotiation(1'005'001);
  const auto fallback = harness.inbound.take_ready_commit(&harness.reliable);
  ASSERT_TRUE(fallback);
  EXPECT_EQ(fallback, config.bootstrap_version);
  harness.deliver(1, config.bootstrap_version, InboundWire::encoded_ack(20));
  EXPECT_FALSE(harness.inbound.take_ready_commit(&harness.reliable));
}

TEST_F(TakionInboundNegotiation, AnAnswerBeforeTheTimeoutIsKept) {
  auto config = InboundWire::negotiated_config();
  config.negotiation_timeout_us = 1'000'000;
  InboundHarness answered{config};
  answered.inbound.note_version_request_sent(0);
  answered.acknowledge_request();
  answered.deliver(1, config.bootstrap_version, InboundWire::encoded_ack(20));
  answered.inbound.expire_negotiation(2'000'000);
  const auto accepted = answered.inbound.take_ready_commit(&answered.reliable);
  ASSERT_TRUE(accepted);
  EXPECT_EQ(accepted, 20U);
}

TEST_F(TakionInboundNegotiation, WaitsForeverWithoutATimeout) {
  InboundHarness unbounded{InboundWire::negotiated_config()};
  unbounded.inbound.note_version_request_sent(0);
  unbounded.acknowledge_request();
  unbounded.inbound.expire_negotiation(60'000'000);
  EXPECT_FALSE(unbounded.inbound.take_ready_commit(&unbounded.reliable));
}

TEST_F(TakionInboundNegotiation,
       AnswersOutsideTheOfferKeepTheBootstrapVersion) {
  auto config = InboundWire::negotiated_config();
  config.negotiation_timeout_us = 1'000'000;
  for (const std::uint32_t rejected : {9U, 22U, 25U}) {
    InboundHarness offered{config};
    offered.inbound.note_version_request_sent(0);
    offered.acknowledge_request();
    offered.deliver(1, config.bootstrap_version,
                    InboundWire::encoded_ack(rejected));
    const auto ready = offered.inbound.take_ready_commit(&offered.reliable);
    ASSERT_TRUE(ready) << "rejected " << rejected;
    EXPECT_EQ(ready, config.bootstrap_version) << "rejected " << rejected;
    EXPECT_TRUE(offered.host.failures.empty()) << "rejected " << rejected;
  }
}

TEST_F(TakionInboundNegotiation,
       AnUndeliveredOfferThatTimesOutCommitsTheBootstrapOnceDelivered) {
  auto config = InboundWire::negotiated_config();
  config.negotiation_timeout_us = 1'000;
  InboundHarness harness{config};
  harness.inbound.note_version_request_sent(100);
  harness.inbound.expire_negotiation(1'101);
  EXPECT_FALSE(harness.inbound.take_ready_commit(&harness.reliable));
  harness.deliver(1, config.bootstrap_version, InboundWire::encoded_ack(20));
  EXPECT_FALSE(harness.inbound.take_ready_commit(&harness.reliable));
  harness.acknowledge_request();
  const auto fallback = harness.inbound.take_ready_commit(&harness.reliable);
  ASSERT_TRUE(fallback);
  EXPECT_EQ(fallback, config.bootstrap_version);
  harness.inbound.note_version_request_sent(2'000);
  harness.acknowledge_request();
  EXPECT_FALSE(harness.inbound.take_ready_commit(&harness.reliable));
}

TEST_F(TakionInboundNegotiation, AFixedProfileIgnoresVersionAnswers) {
  const auto config = InboundWire::fixed_config(15);
  InboundHarness harness{config};
  harness.deliver(1, config.bootstrap_version, InboundWire::encoded_ack(20));
  EXPECT_TRUE(harness.host.failures.empty());
  EXPECT_FALSE(harness.inbound.take_ready_commit(&harness.reliable));
}

}  // namespace
}  // namespace takion_test
