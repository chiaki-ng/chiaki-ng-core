#include "tests/unit/core/net/protocol/senkusha/client_adapter_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace senkusha_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class SenkushaClientInboundMtu : public SenkushaClientRig {
 protected:
  static constexpr std::uint64_t mtu_timeout_us = 5'000;
  static constexpr std::uint32_t attempts_per_candidate = 3;

  void SetUp() override {
    SenkushaClientRig::SetUp();
    if (IsSkipped()) return;
    subject = single_probe_client(harness);
    ASSERT_NO_FATAL_FAILURE(reach_inbound_mtu_measurement(*subject, harness));
  }

  auto sent_mtu_command() const -> std::optional<takion::SenkushaMtuCommand> {
    return senkusha_command<takion::SenkushaMtuCommand>(
        harness.state().sent.back());
  }

  SenkushaHarness harness;
  std::unique_ptr<Client> subject;
};

TEST_F(SenkushaClientInboundMtu, SucceedsAtTheUpperBoundOnTheMatchingFrame) {
  const auto before = harness.state().sent.size();
  ASSERT_TRUE(subject->pump_once());
  ASSERT_EQ(harness.state().sent.size(), before + 1);
  const auto command = sent_mtu_command();
  ASSERT_TRUE(command);
  EXPECT_EQ(command->id, 1U);
  EXPECT_EQ(command->mtu_request, 1454U);
  EXPECT_FALSE(command->mtu_sent);
  EXPECT_FALSE(command->number);
  EXPECT_FALSE(command->send_delay);
  EXPECT_FALSE(command->delta);

  harness.state().incoming.push_back(inbound_video_response(2));
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::measuring_inbound_mtu);
  EXPECT_FALSE(subject->snapshot().inbound_mtu);
  EXPECT_EQ(harness.state().sent.size(), before + 1);

  harness.state().incoming.push_back(inbound_video_response(1));
  ASSERT_TRUE(subject->pump_once());
  const auto snapshot = subject->snapshot();
  EXPECT_EQ(snapshot.state, State::starting_outbound_mtu);
  EXPECT_EQ(snapshot.inbound_mtu, std::optional<std::uint32_t>{1454});
  EXPECT_EQ(harness.events.failures(), 0U);
}

TEST_F(SenkushaClientInboundMtu,
       ConvergesOnTheLowerBoundWhenEveryProbeTimesOut) {
  const std::vector<std::uint32_t> expected_candidates{
      1454, 1015, 795, 685, 630, 603, 589, 582, 579, 577};
  std::vector<std::uint32_t> observed_candidates;
  std::uint32_t previous_id{};

  while (subject->snapshot().state == State::measuring_inbound_mtu) {
    ASSERT_LT(observed_candidates.size(),
              expected_candidates.size() * attempts_per_candidate + 1);
    const auto before = harness.state().sent.size();
    ASSERT_TRUE(subject->pump_once());
    ASSERT_EQ(harness.state().sent.size(), before + 1);
    const auto command = sent_mtu_command();
    ASSERT_TRUE(command);
    ASSERT_EQ(command->id, previous_id + 1);
    previous_id = command->id;
    observed_candidates.push_back(command->mtu_request);

    queue_reliable_ack(harness.state(), last_sequence(harness));
    harness.clock.now_us += mtu_timeout_us;
    ASSERT_TRUE(subject->pump_once());
  }

  ASSERT_EQ(observed_candidates.size(),
            expected_candidates.size() * attempts_per_candidate);
  for (std::size_t index = 0; index < observed_candidates.size(); ++index)
    EXPECT_EQ(observed_candidates[index],
              expected_candidates[index / attempts_per_candidate])
        << "attempt " << index;
  const auto snapshot = subject->snapshot();
  EXPECT_EQ(snapshot.state, State::starting_outbound_mtu);
  EXPECT_EQ(snapshot.inbound_mtu, std::optional<std::uint32_t>{576});
  EXPECT_EQ(harness.events.failures(), 0U);
  EXPECT_EQ(harness.state().close_count, 0U);
}

TEST_F(SenkushaClientInboundMtu, StartsTheTimeoutWhenTheCommandIsSent) {
  harness.clock.now_us += 4'000;
  const auto before = harness.state().sent.size();
  ASSERT_TRUE(subject->pump_once());
  ASSERT_EQ(harness.state().sent.size(), before + 1);
  const auto first = sent_mtu_command();
  ASSERT_TRUE(first);
  EXPECT_EQ(first->id, 1U);

  harness.clock.now_us += mtu_timeout_us - 1;
  ASSERT_TRUE(subject->pump_once());
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(harness.state().sent.size(), before + 1);

  harness.clock.now_us += 1;
  ASSERT_TRUE(subject->pump_once());
  ASSERT_TRUE(subject->pump_once());
  ASSERT_EQ(harness.state().sent.size(), before + 2);
  const auto second = sent_mtu_command();
  ASSERT_TRUE(second);
  EXPECT_EQ(second->id, 2U);
  EXPECT_EQ(subject->snapshot().state, State::measuring_inbound_mtu);
  EXPECT_EQ(harness.events.failures(), 0U);
}

TEST_F(SenkushaClientInboundMtu, FailsCalibrationWhenTheCommandCannotBeSent) {
  harness.state().fail_send = true;
  const auto pumped = subject->pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), Error::takion_failure);
  EXPECT_EQ(subject->snapshot().state, State::failed);
  EXPECT_EQ(harness.events.failures(), 1U);
  EXPECT_EQ(harness.state().close_count, 1U);
}

TEST_F(SenkushaClientInboundMtu,
       KeepsMeasuringWithoutWaitingWhileTheCommandIsBlocked) {
  harness.state().blocked_sends = 1'000;
  const auto before = harness.clock.now_us;
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::measuring_inbound_mtu);
  EXPECT_TRUE(harness.state().writable_waits.empty());
  EXPECT_EQ(harness.clock.now_us, before);
  EXPECT_EQ(harness.events.failures(), 0U);
  EXPECT_EQ(harness.state().close_count, 0U);
}

}  // namespace
}  // namespace senkusha_test
