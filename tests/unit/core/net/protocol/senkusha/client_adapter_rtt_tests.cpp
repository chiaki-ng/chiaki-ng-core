#include "tests/unit/core/net/protocol/senkusha/client_adapter_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace senkusha_test {
namespace {

namespace datagram = libchiaking::net::transport::datagram;
namespace takion = libchiaking::net::protocol::takion;

class SenkushaClientRtt : public SenkushaClientRig {
 protected:
  static auto counted_client(SenkushaHarness& harness, std::uint32_t probes)
      -> std::unique_ptr<Client> {
    ClientConfig config;
    config.policy.probe.count = probes;
    return client(harness, std::move(config));
  }

  SenkushaHarness harness;
};

TEST_F(SenkushaClientRtt, RecordsTenTaggedSamplesAndDisablesEcho) {
  auto subject = client(harness);
  ASSERT_NO_FATAL_FAILURE(reach_local_rtt_measurement(*subject, harness));

  for (std::uint32_t index = 0; index < 10; ++index) {
    ASSERT_TRUE(subject->pump_once()) << "probe " << index;
    const auto probe = harness.state().sent.back();
    ASSERT_EQ(probe.size(), 0x224U) << "probe " << index;

    if (index == 0) {
      auto wrong_tag = probe;
      ASSERT_GT(wrong_tag.size(), 22U);
      wrong_tag[22] ^= std::byte{0x01};
      harness.clock.now_us += 50;
      harness.state().incoming.push_back(std::move(wrong_tag));
      ASSERT_TRUE(subject->pump_once());
      EXPECT_EQ(subject->snapshot().state, State::measuring_rtt);
      EXPECT_EQ(harness.state().sent.back(), probe);
      harness.clock.now_us += 50;
    } else {
      harness.clock.now_us += 100;
    }
    harness.state().incoming.push_back(probe);
    ASSERT_TRUE(subject->pump_once()) << "probe " << index;
  }

  EXPECT_EQ(subject->snapshot().state, State::disabling_echo);
  ASSERT_TRUE(subject->pump_once());
  const auto disable_datagram = harness.state().sent.back();
  const auto echo =
      senkusha_command<takion::SenkushaEchoCommand>(disable_datagram);
  ASSERT_TRUE(echo);
  EXPECT_FALSE(echo->state);

  queue_reliable_ack(harness.state(),
                     outbound_reliable_sequence(disable_datagram, 7));
  ASSERT_TRUE(subject->pump_once());
  const auto snapshot = subject->snapshot();
  EXPECT_EQ(snapshot.state, State::measuring_inbound_mtu);
  ASSERT_TRUE(snapshot.rtt_statistics);
  EXPECT_EQ(snapshot.rtt_statistics->attempted, 10U);
  EXPECT_EQ(snapshot.rtt_statistics->successful, 10U);
  EXPECT_EQ(snapshot.rtt_statistics->lost, 0U);
  EXPECT_EQ(snapshot.rtt_statistics->raw_mean, std::chrono::microseconds{100});
  EXPECT_EQ(snapshot.rtt_statistics->minimum, std::chrono::microseconds{100});
  EXPECT_EQ(snapshot.rtt_statistics->maximum, std::chrono::microseconds{100});
  EXPECT_EQ(snapshot.rtt_statistics->variance_us_squared, 0.0L);
  EXPECT_EQ(snapshot.rtt_statistics->adjusted_mean,
            std::chrono::microseconds{100});
  EXPECT_EQ(harness.events.failures(), 0U);
}

TEST_F(SenkushaClientRtt, DisablesEchoThenFailsOnceWithoutPongs) {
  auto subject = client(harness);
  ASSERT_NO_FATAL_FAILURE(reach_zero_pong_echo_disable(*subject, harness));
  queue_reliable_ack(harness.state(), last_sequence(harness));
  const auto pumped = subject->pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), Error::no_rtt_samples);
  EXPECT_EQ(subject->snapshot().state, State::failed);
  EXPECT_EQ(subject->snapshot().error, Error::no_rtt_samples);
  EXPECT_EQ(harness.events.failures(), 1U);
  EXPECT_EQ(harness.state().close_count, 1U);
}

TEST_F(SenkushaClientRtt, ReportsTheEchoDisableTimeoutWithoutPongs) {
  auto subject = client(harness);
  ASSERT_NO_FATAL_FAILURE(reach_zero_pong_echo_disable(*subject, harness));
  harness.clock.now_us += 4'999'999;
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::disabling_echo);
  harness.clock.now_us += 1;
  const auto pumped = subject->pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), Error::timeout);
  EXPECT_EQ(subject->snapshot().error, Error::timeout);
  EXPECT_EQ(harness.events.failures(), 1U);
  EXPECT_EQ(harness.state().close_count, 1U);
}

TEST_F(SenkushaClientRtt, ReportsPartialLoss) {
  auto subject = counted_client(harness, 3);
  ASSERT_NO_FATAL_FAILURE(reach_local_rtt_measurement(*subject, harness));

  ASSERT_TRUE(subject->pump_once());
  harness.clock.now_us += 1'000'000;
  ASSERT_TRUE(subject->pump_once());

  ASSERT_TRUE(subject->pump_once());
  const auto successful_probe = harness.state().sent.back();
  harness.clock.now_us += 200;
  harness.state().incoming.push_back(successful_probe);
  ASSERT_TRUE(subject->pump_once());

  ASSERT_TRUE(subject->pump_once());
  harness.clock.now_us += 1'000'000;
  ASSERT_TRUE(subject->pump_once());

  const auto snapshot = subject->snapshot();
  EXPECT_EQ(snapshot.state, State::disabling_echo);
  ASSERT_TRUE(snapshot.rtt_statistics);
  EXPECT_EQ(snapshot.rtt_statistics->attempted, 3U);
  EXPECT_EQ(snapshot.rtt_statistics->successful, 1U);
  EXPECT_EQ(snapshot.rtt_statistics->lost, 2U);
  EXPECT_EQ(snapshot.rtt_statistics->raw_mean, std::chrono::microseconds{200});
  EXPECT_EQ(snapshot.rtt_statistics->adjusted_mean,
            std::chrono::microseconds{200});
}

TEST_F(SenkushaClientRtt, TimesAPongReadATickLaterFromWhenTheRouteWasReady) {
  auto subject = client(harness);
  ASSERT_NO_FATAL_FAILURE(reach_local_rtt_measurement(*subject, harness));

  for (std::uint32_t index = 0; index < 10; ++index) {
    ASSERT_TRUE(subject->pump_once()) << "probe " << index;
    const auto probe = harness.state().sent.back();
    const auto sent_at = harness.clock.now_us;
    harness.state().incoming.push_back(probe);
    subject->mark_readable(sent_at + 300);
    subject->mark_readable(sent_at + 900);
    harness.clock.now_us = sent_at + 300 + 4'000 + 1'000;
    ASSERT_TRUE(subject->pump_once()) << "probe " << index;
  }
  EXPECT_EQ(subject->snapshot().state, State::disabling_echo);
  const auto statistics = subject->snapshot().rtt_statistics;
  ASSERT_TRUE(statistics);
  EXPECT_EQ(statistics->successful, 10U);
  EXPECT_EQ(statistics->raw_mean, std::chrono::microseconds{300});
  EXPECT_EQ(statistics->adjusted_mean, std::chrono::microseconds{300});
  EXPECT_EQ(statistics->adjusted_mean.count() / 1000, 0);
}

TEST_F(SenkushaClientRtt, IgnoresRouteReadinessFromBeforeTheProbeWasSent) {
  auto subject = counted_client(harness, 1);
  ASSERT_NO_FATAL_FAILURE(reach_local_rtt_measurement(*subject, harness));

  ASSERT_TRUE(subject->pump_once());
  const auto probe = harness.state().sent.back();
  const auto sent_at = harness.clock.now_us;
  subject->mark_readable(sent_at - 1);
  harness.clock.now_us = sent_at + 1'500;
  harness.state().incoming.push_back(probe);
  ASSERT_TRUE(subject->pump_once());

  const auto statistics = subject->snapshot().rtt_statistics;
  ASSERT_TRUE(statistics);
  EXPECT_EQ(statistics->successful, 1U);
  EXPECT_EQ(statistics->raw_mean, std::chrono::microseconds{1'500});
  EXPECT_EQ(statistics->adjusted_mean.count() / 1000, 1);
}

TEST_F(SenkushaClientRtt, FailsCalibrationWhenAProbeCannotBeSent) {
  auto subject = counted_client(harness, 3);
  ASSERT_NO_FATAL_FAILURE(reach_local_rtt_measurement(*subject, harness));
  ASSERT_TRUE(subject->pump_once());
  const auto probe = harness.state().sent.back();
  EXPECT_EQ(probe.size(), 0x224U);
  harness.clock.now_us += 100;
  harness.state().incoming.push_back(probe);
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::measuring_rtt);
  harness.state().fail_send = true;
  const auto pumped = subject->pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), Error::takion_failure);
  EXPECT_EQ(subject->snapshot().state, State::failed);
  EXPECT_EQ(harness.events.failures(), 1U);
  EXPECT_EQ(harness.state().close_count, 1U);
}

TEST_F(SenkushaClientRtt, RefusesABlockedProbeWithoutWaitingForWritability) {
  auto subject = counted_client(harness, 2);
  ASSERT_NO_FATAL_FAILURE(reach_local_rtt_measurement(*subject, harness));
  const auto before = harness.clock.now_us;
  const auto sent_before = harness.state().sent.size();
  harness.state().blocked_sends = 1;
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(harness.state().blocked_sends, 0U);
  EXPECT_TRUE(harness.state().writable_waits.empty());
  EXPECT_EQ(harness.clock.now_us, before);
  EXPECT_EQ(subject->snapshot().state, State::measuring_rtt);
  EXPECT_EQ(harness.state().sent.size(), sent_before);
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(harness.state().sent.size(), sent_before + 1);
  EXPECT_TRUE(harness.state().writable_waits.empty());
  EXPECT_EQ(harness.events.failures(), 0U);
}

TEST_F(SenkushaClientRtt, CountsABlockedProbeAsLostAndMeasuresTheNext) {
  auto subject = counted_client(harness, 2);
  ASSERT_NO_FATAL_FAILURE(reach_local_rtt_measurement(*subject, harness));
  harness.state().blocked_sends = 1;
  const auto sent_before = harness.state().sent.size();
  ASSERT_TRUE(subject->pump_once());
  ASSERT_TRUE(subject->pump_once());
  ASSERT_EQ(harness.state().sent.size(), sent_before + 1);
  const auto probe = harness.state().sent.back();
  EXPECT_EQ(probe.size(), 0x224U);
  harness.clock.now_us += 300;
  harness.state().incoming.push_back(probe);
  ASSERT_TRUE(subject->pump_once());
  const auto snapshot = subject->snapshot();
  EXPECT_NE(snapshot.state, State::failed);
  ASSERT_TRUE(snapshot.rtt_statistics);
  EXPECT_EQ(snapshot.rtt_statistics->attempted, 2U);
  EXPECT_EQ(snapshot.rtt_statistics->successful, 1U);
  EXPECT_EQ(snapshot.rtt_statistics->lost, 1U);
  EXPECT_EQ(snapshot.rtt_statistics->raw_mean, std::chrono::microseconds{300});
  EXPECT_TRUE(harness.state().writable_waits.empty());
  EXPECT_EQ(harness.events.failures(), 0U);
}

TEST_F(SenkushaClientRtt, TimestampsSpanTheSendCallAndTheDatagramRead) {
  auto subject = single_probe_client(harness);
  auto* clock = &harness.clock;
  harness.state().on_send = [clock](const datagram::OwnedDatagram& value) {
    if (value.size() == 0x224) clock->now_us += 3'000;
  };
  ASSERT_NO_FATAL_FAILURE(reach_local_rtt_measurement(*subject, harness));
  ASSERT_TRUE(subject->pump_once());
  harness.state().on_send = {};
  const auto probe = harness.state().sent.back();
  ASSERT_EQ(probe.size(), 0x224U);
  auto wrong_tag = probe;
  wrong_tag[22] ^= std::byte{0x01};
  harness.clock.now_us += 10'000;
  harness.state().incoming.push_back(probe);
  harness.state().incoming.push_back(wrong_tag);
  harness.state().on_receive.push_back([] {});
  harness.state().on_receive.push_back([clock] { clock->now_us += 5'000; });
  ASSERT_TRUE(subject->pump_once());
  const auto snapshot = subject->snapshot();
  ASSERT_TRUE(snapshot.rtt_statistics);
  EXPECT_EQ(snapshot.rtt_statistics->successful, 1U);
  EXPECT_EQ(snapshot.rtt_statistics->raw_mean,
            std::chrono::microseconds{13'000});
}

}  // namespace
}  // namespace senkusha_test
