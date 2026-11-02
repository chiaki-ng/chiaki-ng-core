#include "tests/unit/core/net/protocol/senkusha/lifecycle_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

namespace senkusha_test {
namespace {

namespace senkusha = libchiaking::net::protocol::senkusha;
using senkusha::Error;
using senkusha::State;

class SenkushaLifecycle : public ::testing::Test {
 protected:
  enum class OutboundStage : std::uint8_t {
    starting,
    measuring,
    finishing,
    disconnecting,
  };

  template <class Input>
  static void fails_once_from_the_handshake(const Input& event,
                                            Error expected) {
    LifecycleRun run;
    ASSERT_TRUE(run.machine.process(lifecycle::StartRequested{1, 100, 30, {}}));
    ASSERT_TRUE(run.machine.process(event));
    EXPECT_EQ(run.machine.state(), State::failed);
    EXPECT_EQ(run.context.terminal_error, expected);
    EXPECT_TRUE(run.cleaned_up_once());
    EXPECT_FALSE(run.machine.process(lifecycle::CancelRequested{1}));
    EXPECT_EQ(run.context.stale_events, 1U);
    EXPECT_TRUE(run.cleaned_up_once());
  }

  template <class Input>
  static void fails_once_while_measuring_rtt(const Input& event, Error expected,
                                             bool finish_measurement) {
    LifecycleRun run;
    ASSERT_NO_FATAL_FAILURE(run.reach_measuring_rtt());
    if (finish_measurement) {
      ASSERT_TRUE(run.machine.process(lifecycle::RttMeasured{1, {}}));
    }
    ASSERT_TRUE(run.machine.process(event));
    EXPECT_EQ(run.machine.state(), State::failed);
    EXPECT_EQ(run.context.terminal_error, expected);
    EXPECT_TRUE(run.cleaned_up_once());
    EXPECT_FALSE(run.machine.process(lifecycle::CancelRequested{1}));
    EXPECT_EQ(run.effects<lifecycle::StopTakion>(), 1U);
  }

  template <class Input>
  static void fails_once_while_measuring_inbound_mtu(const Input& event,
                                                     Error expected) {
    LifecycleRun run;
    ASSERT_NO_FATAL_FAILURE(run.reach_measuring_inbound_mtu());
    ASSERT_TRUE(run.machine.process(event));
    EXPECT_EQ(run.machine.state(), State::failed);
    EXPECT_EQ(run.context.terminal_error, expected);
    EXPECT_TRUE(run.cleaned_up_once());
    EXPECT_FALSE(run.machine.process(lifecycle::CancelRequested{1}));
    EXPECT_EQ(run.effects<lifecycle::StopTakion>(), 1U);
  }

  template <class Input>
  static void fails_once_in_outbound_stage(OutboundStage stage,
                                           const Input& event, Error expected) {
    LifecycleRun run;
    ASSERT_NO_FATAL_FAILURE(run.reach_starting_outbound_mtu());
    if (stage != OutboundStage::starting) {
      ASSERT_TRUE(run.machine.process(lifecycle::OutboundMtuReady{1}));
    }
    if (stage == OutboundStage::finishing ||
        stage == OutboundStage::disconnecting) {
      ASSERT_TRUE(
          run.machine.process(lifecycle::OutboundMtuMeasured{1, 1420, 1454}));
    }
    if (stage == OutboundStage::disconnecting) {
      ASSERT_TRUE(run.machine.process(lifecycle::OutboundMtuDisabled{1}));
    }
    run.context.deadline_us = 800;
    ASSERT_TRUE(run.machine.process(event));
    EXPECT_EQ(run.machine.state(), State::failed);
    EXPECT_EQ(run.context.terminal_error, expected);
    EXPECT_FALSE(run.context.deadline_us);
    EXPECT_TRUE(run.cleaned_up_once());
    EXPECT_FALSE(run.machine.process(lifecycle::CancelRequested{1}));
    EXPECT_EQ(run.context.stale_events, 1U);
    EXPECT_TRUE(run.cleaned_up_once());
  }

  template <class Input>
  static void fails_once_awaiting_big_echo(const Input& event, Error expected) {
    LifecycleRun run;
    ASSERT_NO_FATAL_FAILURE(run.reach_awaiting_big_echo());
    ASSERT_TRUE(run.machine.process(event));
    EXPECT_EQ(run.machine.state(), State::failed);
    EXPECT_EQ(run.context.terminal_error, expected);
    EXPECT_TRUE(run.cleaned_up_once());
    EXPECT_FALSE(run.machine.process(lifecycle::CancelRequested{1}));
    EXPECT_EQ(run.context.stale_events, 1U);
  }

  LifecycleRun run;
};

TEST_F(SenkushaLifecycle, MeasuresBandwidthOnlyWhenAsked) {
  ASSERT_NO_FATAL_FAILURE(run.reach_outbound_mtu_disabled(true));
  EXPECT_EQ(run.machine.state(), State::measuring_bandwidth);
  const auto* begin = run.last_effect<lifecycle::BeginBandwidth>();
  ASSERT_NE(begin, nullptr);
  EXPECT_EQ(begin->attempt_id, 1U);
  EXPECT_EQ(begin->inbound_mtu, 1344U);
  EXPECT_EQ(run.effects<lifecycle::BeginDisconnect>(), 0U);
  EXPECT_FALSE(
      run.machine.process(lifecycle::BandwidthMeasured{2, std::nullopt}));
  const senkusha::BandwidthMeasurement measured{58000, 0.0, 60000};
  ASSERT_TRUE(run.machine.process(lifecycle::BandwidthMeasured{1, measured}));
  EXPECT_EQ(run.machine.state(), State::disconnecting);
  EXPECT_TRUE(run.context.bandwidth ==
              std::optional<senkusha::BandwidthMeasurement>{measured});
  EXPECT_EQ(run.effects<lifecycle::BeginDisconnect>(), 1U);
}

TEST_F(SenkushaLifecycle, StillDisconnectsAfterAFailedBandwidthTest) {
  ASSERT_NO_FATAL_FAILURE(run.reach_outbound_mtu_disabled(true));
  ASSERT_TRUE(
      run.machine.process(lifecycle::BandwidthMeasured{1, std::nullopt}));
  EXPECT_EQ(run.machine.state(), State::disconnecting);
  EXPECT_FALSE(run.context.bandwidth);
}

TEST_F(SenkushaLifecycle, FailsWhenCancelledDuringTheBandwidthTest) {
  ASSERT_NO_FATAL_FAILURE(run.reach_outbound_mtu_disabled(true));
  ASSERT_TRUE(run.machine.process(lifecycle::CancelRequested{1}));
  EXPECT_EQ(run.machine.state(), State::failed);
  EXPECT_EQ(run.context.terminal_error, Error::cancelled);
}

TEST_F(SenkushaLifecycle, ReachesRttAlongTheNegotiatedPath) {
  ASSERT_TRUE(run.machine.process(lifecycle::StartRequested{1, 100, 30, {}}));
  EXPECT_EQ(run.machine.state(), State::connecting_takion);
  ASSERT_TRUE(run.machine.process(lifecycle::TakionConnected{1, 200, 50}));
  EXPECT_EQ(run.machine.state(), State::negotiating_version);
  ASSERT_TRUE(run.machine.process(lifecycle::ProfileCommitted{1, 20, 220, 50}));
  EXPECT_EQ(run.machine.state(), State::awaiting_bang);
  EXPECT_EQ(run.context.committed_version, std::optional<std::uint32_t>{20});
  EXPECT_EQ(run.effects<lifecycle::SendBig>(), 1U);
  ASSERT_TRUE(run.machine.process(lifecycle::BangAccepted{1}));
  EXPECT_EQ(run.machine.state(), State::enabling_echo);
  EXPECT_EQ(run.effects<lifecycle::BeginRtt>(), 1U);
}

TEST_F(SenkushaLifecycle, SkipsNegotiationOnAFixedVersion) {
  ASSERT_TRUE(run.machine.process(lifecycle::StartRequested{7, 100, 30, 15}));
  ASSERT_TRUE(run.machine.process(lifecycle::TakionConnected{7, 200, 50}));
  EXPECT_EQ(run.machine.state(), State::awaiting_bang);
  EXPECT_EQ(run.context.committed_version, std::optional<std::uint32_t>{15});
  const auto* send = run.last_effect<lifecycle::SendBig>();
  ASSERT_NE(send, nullptr);
  EXPECT_EQ(send->version, 15U);
  EXPECT_EQ(run.effects<lifecycle::SendBig>(), 1U);
}

TEST_F(SenkushaLifecycle, IgnoresStaleAndUnexpectedEvents) {
  ASSERT_TRUE(run.machine.process(lifecycle::StartRequested{1, 100, 30, {}}));
  const auto effects = run.context.effects.size();
  EXPECT_FALSE(run.machine.process(lifecycle::TakionConnected{2, 200, 50}));
  EXPECT_EQ(run.context.stale_events, 1U);
  EXPECT_EQ(run.context.effects.size(), effects);
  EXPECT_FALSE(run.machine.process(lifecycle::BangAccepted{1}));
  EXPECT_EQ(run.context.unexpected_events, 1U);
  EXPECT_EQ(run.machine.state(), State::connecting_takion);
}

TEST_F(SenkushaLifecycle, FailsOnceWithTheMappedErrorDuringTheHandshake) {
  {
    SCOPED_TRACE("deadline");
    fails_once_from_the_handshake(lifecycle::DeadlineExpired{1},
                                  Error::timeout);
  }
  {
    SCOPED_TRACE("cancel");
    fails_once_from_the_handshake(lifecycle::CancelRequested{1},
                                  Error::cancelled);
  }
  {
    SCOPED_TRACE("takion");
    fails_once_from_the_handshake(lifecycle::TakionFailed{1},
                                  Error::takion_failure);
  }
}

TEST_F(SenkushaLifecycle, KeepsThePolicyErrorOfARejectedBang) {
  ASSERT_TRUE(run.machine.process(lifecycle::StartRequested{1, 100, 30, 12}));
  ASSERT_TRUE(run.machine.process(lifecycle::TakionConnected{1, 200, 50}));
  ASSERT_TRUE(run.machine.process(
      lifecycle::BangRejected{1, Error::negotiation_failed}));
  EXPECT_EQ(run.context.terminal_error, Error::negotiation_failed);
}

TEST_F(SenkushaLifecycle, FailsAnOverflowingConnectDeadlineBeforeStarting) {
  ASSERT_TRUE(run.machine.process(lifecycle::StartRequested{
      1, std::numeric_limits<std::uint64_t>::max() - 5, 6, {}}));
  EXPECT_EQ(run.machine.state(), State::failed);
  EXPECT_EQ(run.context.terminal_error, Error::arithmetic_overflow);
  EXPECT_EQ(run.effects<lifecycle::StartTakion>(), 0U);
  EXPECT_EQ(run.effects<lifecycle::BestEffortDisconnect>(), 0U);
  EXPECT_EQ(run.effects<lifecycle::StopTakion>(), 0U);
  EXPECT_EQ(run.effects<lifecycle::PublishFailure>(), 1U);
}

TEST_F(SenkushaLifecycle, FailsAnOverflowingResponseDeadline) {
  ASSERT_TRUE(run.machine.process(lifecycle::StartRequested{2, 1, 1, 12}));
  ASSERT_TRUE(run.machine.process(lifecycle::TakionConnected{
      2, std::numeric_limits<std::uint64_t>::max(), 1}));
  EXPECT_EQ(run.machine.state(), State::failed);
  EXPECT_EQ(run.context.terminal_error, Error::arithmetic_overflow);
}

TEST_F(SenkushaLifecycle, FailsAnOverflowingDeadlineAfterTheProfileCommits) {
  ASSERT_TRUE(run.machine.process(lifecycle::StartRequested{3, 1, 1, {}}));
  ASSERT_TRUE(run.machine.process(lifecycle::TakionConnected{3, 2, 1}));
  ASSERT_TRUE(run.machine.process(lifecycle::ProfileCommitted{
      3, 20, std::numeric_limits<std::uint64_t>::max(), 1}));
  EXPECT_EQ(run.machine.state(), State::failed);
  EXPECT_EQ(run.context.terminal_error, Error::arithmetic_overflow);
}

TEST_F(SenkushaLifecycle, RunsOnceAndRefusesAZeroAttempt) {
  EXPECT_FALSE(run.machine.process(lifecycle::StartRequested{0, 1, 1, {}}));
  EXPECT_EQ(run.context.unexpected_events, 1U);
  ASSERT_TRUE(run.machine.process(lifecycle::StartRequested{1, 1, 1, {}}));
  ASSERT_TRUE(run.machine.process(lifecycle::CancelRequested{1}));
  EXPECT_FALSE(run.machine.process(lifecycle::StartRequested{2, 1, 1, {}}));
  EXPECT_EQ(run.machine.state(), State::failed);
  EXPECT_EQ(run.context.unexpected_events, 2U);
}

TEST_F(SenkushaLifecycle, CanBeCancelledWhileEnablingEcho) {
  ASSERT_NO_FATAL_FAILURE(run.reach_enabling_echo());
  ASSERT_TRUE(run.machine.process(lifecycle::CancelRequested{1}));
  EXPECT_EQ(run.machine.state(), State::failed);
  EXPECT_EQ(run.context.terminal_error, Error::cancelled);
}

TEST_F(SenkushaLifecycle, MeasuresRttThenMovesToTheInboundMtu) {
  ASSERT_NO_FATAL_FAILURE(run.reach_measuring_rtt());
  EXPECT_EQ(run.machine.state(), State::measuring_rtt);
  EXPECT_EQ(run.effects<lifecycle::BeginRttProbes>(), 1U);

  senkusha::RttStatistics statistics;
  statistics.attempted = 10;
  statistics.successful = 8;
  statistics.lost = 2;
  statistics.raw_mean = std::chrono::microseconds{1500};
  ASSERT_TRUE(run.machine.process(lifecycle::RttMeasured{1, statistics}));
  EXPECT_EQ(run.machine.state(), State::disabling_echo);
  ASSERT_TRUE(run.context.rtt_statistics);
  EXPECT_EQ(run.context.rtt_statistics->successful, 8U);
  EXPECT_EQ(run.context.rtt_statistics->raw_mean,
            std::chrono::microseconds{1500});
  EXPECT_EQ(run.effects<lifecycle::DisableEcho>(), 1U);

  ASSERT_TRUE(run.machine.process(lifecycle::EchoDisabled{1}));
  EXPECT_EQ(run.machine.state(), State::measuring_inbound_mtu);
  EXPECT_EQ(run.effects<lifecycle::BeginInboundMtu>(), 1U);
}

TEST_F(SenkushaLifecycle, DisablesEchoBeforeFailingAnRttWithoutSamples) {
  ASSERT_NO_FATAL_FAILURE(run.reach_measuring_rtt());
  ASSERT_TRUE(
      run.machine.process(lifecycle::RttUnmeasured{1, Error::no_rtt_samples}));
  EXPECT_EQ(run.machine.state(), State::disabling_echo);
  EXPECT_FALSE(run.context.rtt_statistics);
  EXPECT_EQ(run.effects<lifecycle::DisableEcho>(), 1U);
  EXPECT_EQ(run.effects<lifecycle::PublishFailure>(), 0U);

  ASSERT_TRUE(run.machine.process(lifecycle::EchoDisabled{1}));
  EXPECT_EQ(run.machine.state(), State::failed);
  EXPECT_EQ(run.context.terminal_error, Error::no_rtt_samples);
  EXPECT_EQ(run.effects<lifecycle::BeginInboundMtu>(), 0U);
  EXPECT_TRUE(run.cleaned_up_once());
}

TEST_F(SenkushaLifecycle, TimesOutWhileDisablingEchoAfterAnRttWithoutSamples) {
  ASSERT_NO_FATAL_FAILURE(run.reach_measuring_rtt());
  ASSERT_TRUE(
      run.machine.process(lifecycle::RttUnmeasured{1, Error::no_rtt_samples}));
  ASSERT_TRUE(run.machine.process(lifecycle::DeadlineExpired{1}));
  EXPECT_EQ(run.context.terminal_error, Error::timeout);
  EXPECT_EQ(run.effects<lifecycle::PublishFailure>(), 1U);
}

TEST_F(SenkushaLifecycle, ForgetsAPendingRttErrorOnceTheRttIsMeasured) {
  run.context.pending_rtt_error = Error::no_rtt_samples;
  ASSERT_NO_FATAL_FAILURE(run.reach_measuring_inbound_mtu());
  EXPECT_FALSE(run.context.pending_rtt_error);
}

TEST_F(SenkushaLifecycle, KeepsRttEventsScopedToTheAttemptAndInOrder) {
  ASSERT_NO_FATAL_FAILURE(run.reach_enabling_echo());
  EXPECT_FALSE(run.machine.process(lifecycle::RttMeasured{1, {}}));
  EXPECT_EQ(run.context.unexpected_events, 1U);
  EXPECT_FALSE(run.machine.process(lifecycle::EchoEnabled{2}));
  EXPECT_EQ(run.context.stale_events, 1U);
  EXPECT_EQ(run.machine.state(), State::enabling_echo);
}

TEST_F(SenkushaLifecycle, CleansUpOnceWhenAnRttWaitFails) {
  {
    SCOPED_TRACE("deadline");
    fails_once_while_measuring_rtt(lifecycle::DeadlineExpired{1},
                                   Error::timeout, false);
  }
  {
    SCOPED_TRACE("cancel");
    fails_once_while_measuring_rtt(lifecycle::CancelRequested{1},
                                   Error::cancelled, true);
  }
  {
    SCOPED_TRACE("takion");
    fails_once_while_measuring_rtt(lifecycle::TakionFailed{1},
                                   Error::takion_failure, false);
  }
}

TEST_F(SenkushaLifecycle, HandsTheInboundMtuToTheOutboundSearch) {
  ASSERT_NO_FATAL_FAILURE(run.reach_measuring_inbound_mtu());
  run.context.deadline_us = 500;
  ASSERT_TRUE(run.machine.process(lifecycle::InboundMtuMeasured{1, 1344}));
  EXPECT_EQ(run.machine.state(), State::starting_outbound_mtu);
  EXPECT_FALSE(run.context.deadline_us);
  EXPECT_EQ(run.context.inbound_mtu, std::optional<std::uint32_t>{1344});
  EXPECT_EQ(run.effects<lifecycle::BeginOutboundMtu>(), 1U);
  const auto* begin = run.last_effect<lifecycle::BeginOutboundMtu>();
  ASSERT_NE(begin, nullptr);
  EXPECT_EQ(begin->attempt_id, 1U);
  EXPECT_EQ(begin->inbound_mtu, 1344U);
}

TEST_F(SenkushaLifecycle, IgnoresAStaleInboundMtu) {
  ASSERT_NO_FATAL_FAILURE(run.reach_measuring_inbound_mtu());
  EXPECT_FALSE(run.machine.process(lifecycle::InboundMtuMeasured{2, 1400}));
  EXPECT_EQ(run.context.stale_events, 1U);
  EXPECT_FALSE(run.context.inbound_mtu);
}

TEST_F(SenkushaLifecycle, CleansUpOnceWhenTheInboundMtuFails) {
  {
    SCOPED_TRACE("measurement");
    fails_once_while_measuring_inbound_mtu(
        lifecycle::MeasurementFailed{1, Error::mtu_failed}, Error::mtu_failed);
  }
  {
    SCOPED_TRACE("deadline");
    fails_once_while_measuring_inbound_mtu(lifecycle::DeadlineExpired{1},
                                           Error::timeout);
  }
  {
    SCOPED_TRACE("cancel");
    fails_once_while_measuring_inbound_mtu(lifecycle::CancelRequested{1},
                                           Error::cancelled);
  }
  {
    SCOPED_TRACE("takion");
    fails_once_while_measuring_inbound_mtu(lifecycle::TakionFailed{1},
                                           Error::takion_failure);
  }
}

TEST_F(SenkushaLifecycle, MeasuresTheOutboundMtuThenDisconnects) {
  ASSERT_NO_FATAL_FAILURE(run.reach_starting_outbound_mtu());
  run.context.deadline_us = 500;
  ASSERT_TRUE(run.machine.process(lifecycle::OutboundMtuReady{1}));
  EXPECT_EQ(run.machine.state(), State::measuring_outbound_mtu);
  EXPECT_FALSE(run.context.deadline_us);
  const auto* probes = run.last_effect<lifecycle::BeginOutboundMtuProbes>();
  ASSERT_NE(probes, nullptr);
  EXPECT_EQ(probes->attempt_id, 1U);
  EXPECT_EQ(probes->inbound_mtu, 1344U);

  run.context.deadline_us = 600;
  ASSERT_TRUE(
      run.machine.process(lifecycle::OutboundMtuMeasured{1, 1420, 1454}));
  EXPECT_EQ(run.machine.state(), State::finishing_outbound_mtu);
  EXPECT_FALSE(run.context.deadline_us);
  EXPECT_EQ(run.context.outbound_mtu, std::optional<std::uint32_t>{1420});
  const auto* disable = run.last_effect<lifecycle::DisableOutboundMtu>();
  ASSERT_NE(disable, nullptr);
  EXPECT_EQ(disable->attempt_id, 1U);
  EXPECT_EQ(disable->inbound_mtu, 1344U);
  EXPECT_EQ(disable->upper_bound, 1454U);

  run.context.deadline_us = 700;
  ASSERT_TRUE(run.machine.process(lifecycle::OutboundMtuDisabled{1}));
  EXPECT_EQ(run.machine.state(), State::disconnecting);
  EXPECT_FALSE(run.context.deadline_us);
  const auto* disconnect = run.last_effect<lifecycle::BeginDisconnect>();
  ASSERT_NE(disconnect, nullptr);
  EXPECT_EQ(disconnect->attempt_id, 1U);
}

TEST_F(SenkushaLifecycle, KeepsOutboundMtuEventsScopedAndInOrder) {
  ASSERT_NO_FATAL_FAILURE(run.reach_starting_outbound_mtu());
  EXPECT_FALSE(run.machine.process(lifecycle::OutboundMtuReady{2}));
  EXPECT_EQ(run.context.stale_events, 1U);
  EXPECT_FALSE(
      run.machine.process(lifecycle::OutboundMtuMeasured{1, 1400, 1454}));
  EXPECT_EQ(run.context.unexpected_events, 1U);
  EXPECT_FALSE(run.context.outbound_mtu);
  ASSERT_TRUE(run.machine.process(lifecycle::OutboundMtuReady{1}));
  EXPECT_FALSE(run.machine.process(lifecycle::OutboundMtuDisabled{1}));
  EXPECT_EQ(run.context.unexpected_events, 2U);
  EXPECT_EQ(run.machine.state(), State::measuring_outbound_mtu);
}

TEST_F(SenkushaLifecycle, CleansUpOnceWhenAnOutboundMtuWaitFails) {
  for (const auto stage :
       {OutboundStage::starting, OutboundStage::measuring,
        OutboundStage::finishing, OutboundStage::disconnecting}) {
    SCOPED_TRACE(testing::Message() << "stage " << static_cast<int>(stage));
    {
      SCOPED_TRACE("measurement");
      fails_once_in_outbound_stage(
          stage, lifecycle::MeasurementFailed{1, Error::mtu_failed},
          Error::mtu_failed);
    }
    {
      SCOPED_TRACE("deadline");
      fails_once_in_outbound_stage(stage, lifecycle::DeadlineExpired{1},
                                   Error::timeout);
    }
    {
      SCOPED_TRACE("cancel");
      fails_once_in_outbound_stage(stage, lifecycle::CancelRequested{1},
                                   Error::cancelled);
    }
    {
      SCOPED_TRACE("takion");
      fails_once_in_outbound_stage(stage, lifecycle::TakionFailed{1},
                                   Error::takion_failure);
    }
  }
}

TEST_F(SenkushaLifecycle, CompletesTheDisconnectOnce) {
  ASSERT_NO_FATAL_FAILURE(run.reach_outbound_mtu_disabled());
  EXPECT_EQ(run.machine.state(), State::disconnecting);
  EXPECT_EQ(run.effects<lifecycle::BeginDisconnect>(), 1U);
  EXPECT_FALSE(run.machine.process(lifecycle::OutboundMtuDisabled{1}));
  EXPECT_EQ(run.context.unexpected_events, 1U);
  EXPECT_FALSE(run.machine.process(lifecycle::DisconnectCompleted{2}));
  EXPECT_EQ(run.context.stale_events, 1U);

  run.context.deadline_us = 900;
  ASSERT_TRUE(run.machine.process(lifecycle::DisconnectCompleted{1}));
  EXPECT_EQ(run.machine.state(), State::completed);
  EXPECT_FALSE(run.context.deadline_us);
  EXPECT_FALSE(run.context.active_attempt_id);
  EXPECT_EQ(run.effects<lifecycle::PublishSuccess>(), 1U);
  EXPECT_EQ(run.effects<lifecycle::PublishFailure>(), 0U);
  EXPECT_EQ(run.effects<lifecycle::BestEffortDisconnect>(), 0U);
  EXPECT_EQ(run.effects<lifecycle::StopTakion>(), 0U);
  EXPECT_EQ(run.context.committed_version, std::optional<std::uint32_t>{12});
  EXPECT_TRUE(run.context.rtt_statistics);
  EXPECT_EQ(run.context.inbound_mtu, std::optional<std::uint32_t>{1344});
  EXPECT_EQ(run.context.outbound_mtu, std::optional<std::uint32_t>{1420});

  EXPECT_FALSE(run.machine.process(lifecycle::DisconnectCompleted{1}));
  EXPECT_EQ(run.context.stale_events, 2U);
  EXPECT_FALSE(run.machine.process(lifecycle::CancelRequested{1}));
  EXPECT_EQ(run.context.stale_events, 3U);
  EXPECT_FALSE(run.machine.process(lifecycle::StartRequested{2, 1, 1, {}}));
  EXPECT_EQ(run.context.unexpected_events, 2U);
  EXPECT_EQ(run.machine.state(), State::completed);
  EXPECT_EQ(run.effects<lifecycle::PublishSuccess>(), 1U);
}

TEST_F(SenkushaLifecycle, MovesOnFromABigEchoWhetherItArrivesOrTimesOut) {
  for (const bool receive_echo : {true, false}) {
    SCOPED_TRACE(receive_echo ? "received" : "timed out");
    LifecycleRun echo;
    ASSERT_NO_FATAL_FAILURE(echo.reach_awaiting_big_echo());
    EXPECT_EQ(echo.machine.state(), State::awaiting_big_echo);
    EXPECT_FALSE(echo.machine.process(lifecycle::BangAccepted{1}));
    EXPECT_EQ(echo.context.unexpected_events, 1U);
    EXPECT_FALSE(echo.machine.process(lifecycle::BigEchoReceived{2}));
    EXPECT_EQ(echo.context.stale_events, 1U);
    if (receive_echo)
      ASSERT_TRUE(echo.machine.process(lifecycle::BigEchoReceived{1}));
    else
      ASSERT_TRUE(echo.machine.process(lifecycle::DeadlineExpired{1}));
    EXPECT_EQ(echo.machine.state(), State::enabling_echo);
    EXPECT_FALSE(echo.context.deadline_us);
    EXPECT_FALSE(echo.context.terminal_error);
    EXPECT_EQ(echo.effects<lifecycle::BeginRtt>(), 1U);
    EXPECT_EQ(echo.effects<lifecycle::PublishFailure>(), 0U);
    EXPECT_FALSE(echo.machine.process(lifecycle::BigEchoReceived{1}));
    EXPECT_EQ(echo.context.unexpected_events, 2U);
  }
}

TEST_F(SenkushaLifecycle, CleansUpOnceWhenABigEchoWaitFails) {
  {
    SCOPED_TRACE("cancel");
    fails_once_awaiting_big_echo(lifecycle::CancelRequested{1},
                                 Error::cancelled);
  }
  {
    SCOPED_TRACE("takion");
    fails_once_awaiting_big_echo(lifecycle::TakionFailed{1},
                                 Error::takion_failure);
  }
}

TEST_F(SenkushaLifecycle, ExpectsATypedBangRatherThanABigEcho) {
  ASSERT_TRUE(run.machine.process(lifecycle::StartRequested{1, 1, 1, 12}));
  ASSERT_TRUE(run.machine.process(lifecycle::TakionConnected{1, 2, 5}));
  EXPECT_FALSE(run.machine.process(lifecycle::BigEchoReceived{1}));
  EXPECT_EQ(run.context.unexpected_events, 1U);
  EXPECT_EQ(run.machine.state(), State::awaiting_bang);
  ASSERT_TRUE(run.machine.process(lifecycle::BangAccepted{1}));
  EXPECT_EQ(run.machine.state(), State::enabling_echo);
}

}  // namespace
}  // namespace senkusha_test
