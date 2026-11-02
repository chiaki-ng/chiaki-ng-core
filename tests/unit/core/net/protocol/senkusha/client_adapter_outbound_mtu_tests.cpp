#include "tests/unit/core/net/protocol/senkusha/client_adapter_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace senkusha_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class SenkushaClientOutboundMtu : public SenkushaClientRig {
 protected:
  void SetUp() override {
    SenkushaClientRig::SetUp();
    if (IsSkipped()) return;
    subject = single_probe_client(harness);
  }

  auto sent_client_mtu_command() const
      -> std::optional<takion::SenkushaClientMtuCommand> {
    return senkusha_command<takion::SenkushaClientMtuCommand>(
        harness.state().sent.back());
  }

  void expect_failed_once(Error error) {
    const auto again = subject->pump_once();
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error(), error);
    EXPECT_EQ(harness.events.failures(), 1U);
  }

  SenkushaHarness harness;
  std::unique_ptr<Client> subject;
};

TEST_F(SenkushaClientOutboundMtu,
       SendsTheClientMtuCommandOnTheSenkushaChannel) {
  ASSERT_NO_FATAL_FAILURE(reach_starting_outbound_mtu(*subject, harness));
  const auto command = sent_client_mtu_command();
  ASSERT_TRUE(command);
  EXPECT_EQ(command->id, 1U);
  EXPECT_EQ(command->mtu_request, 1454U);
  EXPECT_TRUE(command->state);
  EXPECT_EQ(command->mtu_down, std::optional<std::uint32_t>{1454});
  EXPECT_EQ(subject->snapshot().state, State::starting_outbound_mtu);
  EXPECT_EQ(harness.events.failures(), 0U);
}

TEST_F(SenkushaClientOutboundMtu, WaitsForTheExactServerCommand) {
  ASSERT_NO_FATAL_FAILURE(reach_starting_outbound_mtu(*subject, harness));
  auto peer_channel = peer(0xa1b2c3d6);

  queue_reliable_control(
      harness.state(), peer_channel,
      takion::Senkusha{takion::SenkushaClientMtuCommand{2, 1454, true, 1454}},
      7, 8);
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::starting_outbound_mtu);

  queue_reliable_control(
      harness.state(), peer_channel,
      takion::Senkusha{takion::SenkushaMtuCommand{
          1, 1454, std::nullopt, std::optional<std::uint32_t>{1}, std::nullopt,
          std::nullopt}},
      7, 8);
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::starting_outbound_mtu);
  EXPECT_EQ(harness.events.failures(), 0U);

  queue_reliable_control(
      harness.state(), peer_channel,
      takion::Senkusha{takion::SenkushaClientMtuCommand{1, 1454, true, 1454}},
      7, 8);
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::measuring_outbound_mtu);
  EXPECT_EQ(harness.events.failures(), 0U);
}

TEST_F(SenkushaClientOutboundMtu, FailsExactlyOnceWhenTheServerNeverAnswers) {
  ASSERT_NO_FATAL_FAILURE(reach_starting_outbound_mtu(*subject, harness));
  const auto deadline = subject->snapshot().deadline_us;
  ASSERT_TRUE(deadline);
  harness.clock.now_us = *deadline + 1;
  const auto pumped = subject->pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), Error::timeout);
  EXPECT_EQ(subject->snapshot().state, State::failed);
  EXPECT_EQ(subject->snapshot().error, Error::timeout);
  EXPECT_EQ(harness.events.failures(), 1U);
  EXPECT_EQ(harness.state().close_count, 1U);
  expect_failed_once(Error::timeout);
}

TEST_F(SenkushaClientOutboundMtu, FailsAtOnceWhenTheCommandCannotBeSent) {
  ASSERT_NO_FATAL_FAILURE(
      reach_starting_outbound_mtu(*subject, harness, false));
  harness.state().fail_send = true;
  const auto pumped = subject->pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), Error::takion_failure);
  EXPECT_EQ(subject->snapshot().state, State::failed);
  EXPECT_EQ(harness.events.failures(), 1U);
  EXPECT_EQ(harness.state().close_count, 1U);
  expect_failed_once(Error::takion_failure);
}

TEST_F(SenkushaClientOutboundMtu, CountsAShortProbeSendAsAnImmediateLoss) {
  ASSERT_NO_FATAL_FAILURE(reach_starting_outbound_mtu(*subject, harness));
  auto peer_channel = peer(0xa1b2c3d6);
  queue_reliable_control(
      harness.state(), peer_channel,
      takion::Senkusha{takion::SenkushaClientMtuCommand{1, 1454, true, 1454}},
      7, 8);
  ASSERT_TRUE(subject->pump_once());
  ASSERT_EQ(subject->snapshot().state, State::measuring_outbound_mtu);

  const auto clock_before = harness.clock.now_us;
  const auto sent_before = harness.state().sent.size();
  harness.state().forced_send_size = 1;
  std::size_t pumps = 0;
  while (subject->snapshot().state == State::measuring_outbound_mtu &&
         pumps < 64) {
    ASSERT_TRUE(subject->pump_once()) << "pump " << pumps;
    ++pumps;
  }
  harness.state().forced_send_size.reset();
  EXPECT_EQ(harness.clock.now_us, clock_before);
  EXPECT_EQ(harness.state().sent.size(), sent_before + 30);
  EXPECT_EQ(subject->snapshot().outbound_mtu,
            std::optional<std::uint32_t>{576});
  EXPECT_EQ(subject->snapshot().state, State::finishing_outbound_mtu);
  EXPECT_EQ(harness.events.failures(), 0U);
  EXPECT_EQ(harness.state().close_count, 0U);
}

TEST_F(SenkushaClientOutboundMtu,
       SendsAnExactUpperBoundProbeAndMatchesOnlyItsPong) {
  ASSERT_NO_FATAL_FAILURE(reach_measuring_outbound_mtu(*subject, harness));

  const auto probe = harness.state().sent.back();
  ASSERT_EQ(probe.size(), 1426U);
  EXPECT_EQ(probe[22], std::byte{0x11});
  EXPECT_EQ(probe[23], std::byte{0x22});
  EXPECT_EQ(probe[24], std::byte{0x33});
  EXPECT_EQ(probe[25], std::byte{0x44});
  constexpr std::array padding{std::byte{'C'}, std::byte{'H'}, std::byte{'I'},
                               std::byte{'A'}, std::byte{'K'}, std::byte{'I'}};
  for (std::size_t index = 26; index < probe.size(); ++index)
    EXPECT_EQ(probe[index], padding[(index - 26) % padding.size()])
        << "index " << index;

  auto stale = probe;
  stale[22] ^= std::byte{0x01};
  harness.clock.now_us += 50;
  harness.state().incoming.push_back(std::move(stale));
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::measuring_outbound_mtu);
  EXPECT_EQ(harness.state().sent.back(), probe);

  harness.clock.now_us += 50;
  harness.state().incoming.push_back(probe);
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::finishing_outbound_mtu);
  EXPECT_EQ(harness.events.failures(), 0U);
}

TEST_F(SenkushaClientOutboundMtu,
       SendsTheFinalDisableCommandAfterUpperSuccess) {
  ASSERT_NO_FATAL_FAILURE(reach_final_outbound_mtu_command(*subject, harness));
  const auto command = sent_client_mtu_command();
  ASSERT_TRUE(command);
  EXPECT_EQ(command->id, 2U);
  EXPECT_EQ(command->mtu_request, 1454U);
  EXPECT_FALSE(command->state);
  EXPECT_EQ(command->mtu_down, std::optional<std::uint32_t>{1454});
  EXPECT_TRUE(subject->snapshot().deadline_us.has_value());
  EXPECT_EQ(harness.events.failures(), 0U);
}

TEST_F(SenkushaClientOutboundMtu, CompletesOnlyOnTheExactFinalAck) {
  ASSERT_NO_FATAL_FAILURE(reach_final_outbound_mtu_command(*subject, harness));
  const auto final_sequence = last_sequence(harness);
  ASSERT_GT(final_sequence, 0U);
  queue_reliable_ack(harness.state(), final_sequence - 1);
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::finishing_outbound_mtu);
  EXPECT_TRUE(subject->snapshot().deadline_us.has_value());

  queue_reliable_ack(harness.state(), final_sequence);
  ASSERT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::completed);
  EXPECT_FALSE(subject->snapshot().deadline_us);
  EXPECT_EQ(harness.events.failures(), 0U);
}

TEST_F(SenkushaClientOutboundMtu, FailsExactlyOnceWhenTheFinalAckNeverArrives) {
  ASSERT_NO_FATAL_FAILURE(reach_final_outbound_mtu_command(*subject, harness));
  const auto deadline = subject->snapshot().deadline_us;
  ASSERT_TRUE(deadline);
  harness.clock.now_us = *deadline + 1;
  const auto pumped = subject->pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), Error::timeout);
  EXPECT_EQ(subject->snapshot().state, State::failed);
  EXPECT_EQ(harness.events.failures(), 1U);
  expect_failed_once(Error::timeout);
}

TEST_F(SenkushaClientOutboundMtu,
       ConvergesOnTheLowerBoundWhenEveryProbeTimesOut) {
  ASSERT_NO_FATAL_FAILURE(reach_measuring_outbound_mtu(*subject, harness));

  constexpr std::uint64_t mtu_timeout_us = 5'000;
  constexpr std::uint32_t attempts_per_candidate = 3;
  const std::vector<std::uint32_t> expected_candidates{
      1454, 1015, 795, 685, 630, 603, 589, 582, 579, 577};
  std::vector<std::uint32_t> observed_candidates;

  while (subject->snapshot().state == State::measuring_outbound_mtu) {
    ASSERT_LT(observed_candidates.size(),
              expected_candidates.size() * attempts_per_candidate + 1);
    const auto& probe = harness.state().sent.back();
    ASSERT_GE(probe.size(), 28U);
    observed_candidates.push_back(static_cast<std::uint32_t>(probe.size()) +
                                  28U);
    harness.clock.now_us += mtu_timeout_us;
    ASSERT_TRUE(subject->pump_once());
    if (subject->snapshot().state == State::measuring_outbound_mtu) {
      ASSERT_TRUE(subject->pump_once());
      ASSERT_TRUE(subject->snapshot().deadline_us.has_value());
    }
  }

  ASSERT_EQ(observed_candidates.size(),
            expected_candidates.size() * attempts_per_candidate);
  for (std::size_t index = 0; index < observed_candidates.size(); ++index)
    EXPECT_EQ(observed_candidates[index],
              expected_candidates[index / attempts_per_candidate])
        << "attempt " << index;
  EXPECT_EQ(subject->snapshot().state, State::finishing_outbound_mtu);
  ASSERT_TRUE(subject->pump_once());
  const auto final_command = sent_client_mtu_command();
  ASSERT_TRUE(final_command);
  EXPECT_EQ(final_command->id, 2U);
  EXPECT_EQ(final_command->mtu_request, 577U);
  EXPECT_FALSE(final_command->state);
  EXPECT_EQ(final_command->mtu_down, std::optional<std::uint32_t>{1454});
  EXPECT_EQ(harness.events.failures(), 0U);
}

}  // namespace
}  // namespace senkusha_test
