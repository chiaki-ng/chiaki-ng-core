#include "tests/unit/core/net/protocol/senkusha/client_adapter_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

namespace senkusha_test {
namespace {

namespace senkusha = libchiaking::net::protocol::senkusha;
namespace takion = libchiaking::net::protocol::takion;
namespace remote_play = libchiaking::net::protocol::remote_play;

class SenkushaClientCompletion : public SenkushaClientRig {
 protected:
  SenkushaHarness harness;
  CandidateRoute route = candidate_route();
};

TEST_F(SenkushaClientCompletion, ReturnsTheRouteAndTheResultOnce) {
  auto subject = single_probe_client(harness);
  ASSERT_NO_FATAL_FAILURE(complete(*subject, harness));

  const auto snapshot = subject->snapshot();
  EXPECT_EQ(snapshot.state, State::completed);
  EXPECT_FALSE(snapshot.attempt_id);
  EXPECT_FALSE(snapshot.deadline_us);
  EXPECT_EQ(snapshot.outbound_mtu, std::optional<std::uint32_t>{1454});
  ASSERT_TRUE(snapshot.result);
  EXPECT_EQ(snapshot.result->takion_version, 20U);
  EXPECT_EQ(snapshot.result->round_trip_time.successful, 1U);
  EXPECT_EQ(snapshot.result->round_trip_time.raw_mean,
            std::chrono::microseconds{100});
  EXPECT_EQ(snapshot.result->inbound_mtu, 1454U);
  EXPECT_EQ(snapshot.result->outbound_mtu, 1454U);
  EXPECT_EQ(snapshot.result->wire_profile.version(), 20U);
  EXPECT_EQ(harness.state().close_count, 0U);

  std::size_t completed_count{};
  for (const auto& value : harness.events.events) {
    const auto* completed = std::get_if<senkusha::event::Completed>(&value);
    if (!completed) continue;
    ++completed_count;
    EXPECT_EQ(completed->result.takion_version, 20U);
    EXPECT_EQ(completed->result.inbound_mtu, 1454U);
    EXPECT_EQ(completed->result.outbound_mtu, 1454U);
  }
  EXPECT_EQ(completed_count, 1U);

  const auto disconnect_datagram = harness.state().sent.back();
  const auto disconnect_message = decode_outbound_control(
      disconnect_datagram, 7,
      outbound_reliable_sequence(disconnect_datagram, 7), 1);
  ASSERT_TRUE(disconnect_message);
  ASSERT_TRUE(std::holds_alternative<takion::Disconnect>(*disconnect_message));
  EXPECT_EQ(std::get<takion::Disconnect>(*disconnect_message).reason,
            "Client Disconnecting");

  auto handed = subject->take_route();
  ASSERT_TRUE(handed);
  const std::array payload{std::byte{0x42}};
  const auto sent = handed->send(payload);
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent.value(), payload.size());
  EXPECT_FALSE(subject->take_route());
  EXPECT_EQ(harness.state().close_count, 0U);
}

TEST_F(SenkushaClientCompletion, ClosesOnceUnderTheClosePolicy) {
  auto subject =
      single_probe_client(harness, TransportHandoffPolicy::close_after_use);
  ASSERT_NO_FATAL_FAILURE(complete(*subject, harness));
  EXPECT_EQ(subject->snapshot().state, State::completed);
  EXPECT_TRUE(subject->snapshot().result);
  EXPECT_EQ(harness.state().close_count, 1U);
  EXPECT_FALSE(subject->take_route());
  EXPECT_EQ(harness.events.failures(), 0U);
}

TEST_F(SenkushaClientCompletion, HandsACandidateRouteOffOnceWithItsMetadata) {
  auto subject = single_probe_candidate_client(harness, route);

  // Unstarted client: no established route yet.
  EXPECT_FALSE(subject->take_established_route());

  ASSERT_NO_FATAL_FAILURE(complete(*subject, harness));
  EXPECT_EQ(subject->snapshot().state, State::completed);
  EXPECT_EQ(harness.events.failures(), 0U);

  EXPECT_FALSE(subject->take_route());
  auto established = subject->take_established_route();
  ASSERT_TRUE(established);
  ASSERT_TRUE(established->transport);
  EXPECT_EQ(established->local_endpoint.address, route.local_endpoint.address);
  EXPECT_EQ(established->local_endpoint.port, route.local_endpoint.port);
  EXPECT_EQ(established->remote_endpoint.address,
            route.remote_endpoint.address);
  EXPECT_EQ(established->remote_endpoint.port, route.remote_endpoint.port);
  EXPECT_EQ(established->kind, route.kind);
  EXPECT_EQ(established->dont_fragment, route.dont_fragment);

  // Calibration compared field-by-field (no Result equality).
  const auto snapshot = subject->snapshot();
  EXPECT_TRUE(snapshot.result);
  EXPECT_EQ(established->calibration.wire_profile.version(), 20U);
  EXPECT_EQ(established->calibration.takion_version, 20U);
  EXPECT_EQ(established->calibration.inbound_mtu, 1454U);
  EXPECT_EQ(established->calibration.outbound_mtu, 1454U);
  EXPECT_EQ(established->calibration.round_trip_time.successful, 1U);
  EXPECT_EQ(established->calibration.round_trip_time.raw_mean,
            std::chrono::microseconds{100});

  // Handed-off transport is usable.
  const std::array payload{std::byte{0x42}};
  const auto sent = established->transport->send(payload);
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent.value(), payload.size());

  // Duplicate take is empty; handoff policy leaves transport open.
  EXPECT_FALSE(subject->take_established_route());
  EXPECT_FALSE(subject->take_route());
  subject.reset();
  EXPECT_EQ(harness.state().close_count, 0U);
  EXPECT_TRUE(established->transport.close());
  EXPECT_EQ(harness.state().close_count, 1U);
}

TEST_F(SenkushaClientCompletion,
       ClosesACandidateOnceAndHandsNothingOffUnderTheClosePolicy) {
  // close_after_use success: closed exactly once, no route handed off.
  auto subject = single_probe_candidate_client(
      harness, route, TransportHandoffPolicy::close_after_use);
  ASSERT_NO_FATAL_FAILURE(complete(*subject, harness));
  EXPECT_EQ(subject->snapshot().state, State::completed);
  EXPECT_FALSE(subject->take_established_route());
  EXPECT_EQ(harness.state().close_count, 1U);
  EXPECT_EQ(harness.events.failures(), 0U);
}

TEST_F(SenkushaClientCompletion, ClosesACancelledCandidateOnceWithoutARoute) {
  // Cancel: never completed, no established route, closed exactly once.
  auto subject = single_probe_candidate_client(harness, route);
  ASSERT_NO_FATAL_FAILURE(reach_final_outbound_mtu_command(*subject, harness));
  EXPECT_TRUE(subject->cancel());
  EXPECT_NE(subject->snapshot().state, State::completed);
  EXPECT_FALSE(subject->take_established_route());
  EXPECT_EQ(harness.state().close_count, 1U);
}

TEST_F(SenkushaClientCompletion, ClosesATimedOutCandidateOnceWithoutARoute) {
  // Timeout: never completed, no established route, closed exactly once.
  auto subject = single_probe_candidate_client(harness, route);
  ASSERT_NO_FATAL_FAILURE(reach_final_outbound_mtu_command(*subject, harness));
  const auto deadline = subject->snapshot().deadline_us;
  ASSERT_TRUE(deadline);
  harness.clock.now_us = *deadline + 1;
  const auto pumped = subject->pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), Error::timeout);
  EXPECT_NE(subject->snapshot().state, State::completed);
  EXPECT_FALSE(subject->take_established_route());
  EXPECT_EQ(harness.state().close_count, 1U);
  EXPECT_EQ(harness.events.failures(), 1U);
}

class SenkushaClientFailedHandoff : public SenkushaClientRig {
 protected:
  void SetUp() override {
    SenkushaClientRig::SetUp();
    if (IsSkipped()) return;
    ClientConfig config;
    config.failure_handoff = TransportHandoffPolicy::return_route;
    subject = std::make_unique<Client>(
        remote_play::RouteCandidate{
            harness.transport(), {}, {}, remote_play::RouteKind::remote_rudp},
        harness.clock, threading(), harness.random, harness.events,
        std::move(config));
    ASSERT_TRUE(subject->start());
    EXPECT_FALSE(subject->take_failed_route());
  }

  void fail_by_deadline() {
    harness.clock.now_us = 31'000'001;
    EXPECT_FALSE(subject->pump_once());
    EXPECT_EQ(harness.state().close_count, 0U);
  }

  SenkushaHarness harness;
  std::unique_ptr<Client> subject;
};

TEST_F(SenkushaClientFailedHandoff, HandsTheFailedRouteOffOnce) {
  fail_by_deadline();
  auto failed = subject->take_failed_route();
  ASSERT_TRUE(failed);
  EXPECT_TRUE(failed->transport);
  EXPECT_FALSE(subject->take_failed_route());
  subject.reset();
  EXPECT_EQ(harness.state().close_count, 0U);
  EXPECT_TRUE(failed->transport.close());
  EXPECT_EQ(harness.state().close_count, 1U);
}

TEST_F(SenkushaClientFailedHandoff, ClosesAnUnclaimedFailedRouteOnDestruction) {
  fail_by_deadline();
  subject.reset();
  EXPECT_EQ(harness.state().close_count, 1U);
}

TEST_F(SenkushaClientFailedHandoff, OffersNoRouteAfterACancel) {
  EXPECT_TRUE(subject->cancel());
  EXPECT_FALSE(subject->take_failed_route());
  subject.reset();
  EXPECT_EQ(harness.state().close_count, 1U);
}

}  // namespace
}  // namespace senkusha_test
