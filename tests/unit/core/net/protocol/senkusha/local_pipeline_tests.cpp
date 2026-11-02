#include "tests/support/crypto/refusing.hpp"
#include "tests/unit/core/net/protocol/senkusha/local_pipeline_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace senkusha_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;
namespace remote_play = libchiaking::net::protocol::remote_play;

class LocalCalibratedPipelineFallback : public LocalPipelineRig {
 protected:
  enum class Scenario : std::uint8_t {
    no_fallback,
    fallback_starts,
    cancelled_calibration,
    fallback_unavailable,
    active_start_fails,
  };

  static void run(Scenario scenario) {
    SenkushaHarness harness;
    SenkushaHarness fallback_harness;
    const auto route = candidate_route();
    auto subject = single_probe_candidate_client(harness, route);
    ASSERT_TRUE(subject->start());
    if (scenario == Scenario::cancelled_calibration) {
      ASSERT_TRUE(subject->cancel());
    }
    LocalPipelineCrypto crypto;
    crypto.fail_generate = scenario == Scenario::active_start_fails;
    test_support::RefusingTakionCrypto takion_crypto;
    const auto registered = console(RegistrationTarget::ps4_9);
    auto pipeline_inputs = inputs(harness, crypto, takion_crypto, registered);
    unsigned fallback_calls{};
    pipeline_inputs.transition_route = [](remote_play::EstablishedRoute)
        -> libchiaking::Expected<remote_play::EstablishedRoute,
                                 LocalPipelineError> {
      ADD_FAILURE() << "the route was transitioned";
      return libchiaking::unexpected(LocalPipelineError::route_unavailable);
    };
    if (scenario != Scenario::no_fallback) {
      pipeline_inputs.fallback_route =
          [&]() -> libchiaking::Expected<remote_play::EstablishedRoute,
                                         LocalPipelineError> {
        ++fallback_calls;
        EXPECT_EQ(harness.state().close_count, 1U);
        if (scenario == Scenario::fallback_unavailable)
          return libchiaking::unexpected(LocalPipelineError::route_unavailable);
        auto calibration = remote_play::LocalStreamPreparation::
            make_reference_fallback_calibration(registered.target);
        EXPECT_TRUE(calibration);
        if (!calibration)
          return libchiaking::unexpected(LocalPipelineError::route_unavailable);
        return remote_play::EstablishedRoute{
            {fallback_harness.transport(), route.local_endpoint,
             route.remote_endpoint, remote_play::RouteKind::local_direct,
             remote_play::DontFragmentPolicy::disabled},
            *calibration};
      };
    }
    remote_play::LocalCalibratedPipeline pipeline{
        std::move(subject), ready_ticket(), std::move(pipeline_inputs)};
    harness.clock.now_us = 31'000'001;
    const auto progress = pipeline.pump_once();
    EXPECT_EQ(harness.state().close_count, 1U);
    EXPECT_EQ(fallback_calls,
              scenario == Scenario::no_fallback ||
                      scenario == Scenario::cancelled_calibration
                  ? 0U
                  : 1U);
    if (scenario == Scenario::fallback_starts) {
      EXPECT_TRUE(progress);
      EXPECT_EQ(pipeline.state(), LocalPipelineState::active);
      EXPECT_TRUE(pipeline.active());
      EXPECT_EQ(fallback_harness.state().sent.size(), 1U);
      EXPECT_EQ(fallback_harness.state().close_count, 0U);
      EXPECT_TRUE(pipeline.pump_once());
      EXPECT_EQ(fallback_calls, 1U);
    } else {
      ASSERT_FALSE(progress);
      EXPECT_EQ(pipeline.state(), LocalPipelineState::failed);
      EXPECT_EQ(progress.error(), scenario == Scenario::active_start_fails
                                      ? LocalPipelineError::active_start_failed
                                      : LocalPipelineError::senkusha_failed);
    }
    EXPECT_TRUE(pipeline.request_stop());
    EXPECT_TRUE(pipeline.request_stop());
    EXPECT_EQ(harness.state().close_count, 1U);
    EXPECT_EQ(fallback_harness.state().close_count,
              scenario == Scenario::fallback_starts ||
                      scenario == Scenario::active_start_fails
                  ? 1U
                  : 0U);
  }
};

TEST_F(LocalCalibratedPipelineFallback,
       FailsAFailedCalibrationWithoutAFallback) {
  run(Scenario::no_fallback);
}

TEST_F(LocalCalibratedPipelineFallback,
       StartsOnTheFallbackRouteAfterAFailedCalibration) {
  run(Scenario::fallback_starts);
}

TEST_F(LocalCalibratedPipelineFallback, NeverAsksForAFallbackAfterACancel) {
  run(Scenario::cancelled_calibration);
}

TEST_F(LocalCalibratedPipelineFallback,
       FailsWhenTheFallbackRouteIsUnavailable) {
  run(Scenario::fallback_unavailable);
}

TEST_F(LocalCalibratedPipelineFallback,
       ClosesTheFallbackRouteWhenTheActiveStartFails) {
  run(Scenario::active_start_fails);
}

class LocalCalibratedPipelineRetainedRoute : public LocalPipelineRig {
 protected:
  enum class Scenario : std::uint8_t {
    switches,
    stopped_before_failing,
    switch_closes_the_route,
    active_start_fails,
    first_send_fails,
    destroyed_before_failing,
    dont_fragment_fails,
  };

  static void run(Scenario scenario) {
    SenkushaHarness harness;
    LocalPipelineCrypto crypto;
    crypto.fail_generate = scenario == Scenario::active_start_fails;
    test_support::RefusingTakionCrypto takion_crypto;
    const auto registered = console(RegistrationTarget::ps5_1);
    auto pipeline_inputs = inputs(harness, crypto, takion_crypto, registered);
    pipeline_inputs.retained_route_fallback =
        remote_play::LocalStreamPreparation::
            make_reference_fallback_calibration(registered.target);
    auto transport = harness.transport();
    const auto* original = transport.get();
    unsigned switches{};
    pipeline_inputs.transition_route = [&](remote_play::EstablishedRoute route)
        -> libchiaking::Expected<remote_play::EstablishedRoute,
                                 LocalPipelineError> {
      ++switches;
      EXPECT_EQ(route.transport.get(), original);
      EXPECT_EQ(route.kind, remote_play::RouteKind::remote_rudp);
      EXPECT_EQ(route.dont_fragment, remote_play::DontFragmentPolicy::disabled);
      EXPECT_EQ(harness.state().dont_fragment, std::vector<bool>{false});
      EXPECT_EQ(route.calibration.inbound_mtu, 1454U);
      EXPECT_EQ(route.calibration.outbound_mtu, 1454U);
      EXPECT_EQ(route.calibration.round_trip_time.adjusted_mean.count(), 1000);
      EXPECT_EQ(route.calibration.takion_version, takion::takion_version_ps5);
      EXPECT_EQ(harness.state().close_count, 0U);
      if (scenario == Scenario::switch_closes_the_route) {
        static_cast<void>(route.transport.close());
        return libchiaking::unexpected(LocalPipelineError::route_unavailable);
      }
      return route;
    };
    if (scenario == Scenario::first_send_fails)
      harness.state().fail_send = true;
    if (scenario == Scenario::dont_fragment_fails)
      harness.state().fail_dont_fragment = true;
    auto pipeline = remote_play::LocalStart::start_calibration(
        {std::move(transport),
         {},
         {},
         remote_play::RouteKind::remote_rudp,
         remote_play::DontFragmentPolicy::enabled},
        ready_ticket(), {pipeline_inputs, harness.events, {}});
    ASSERT_TRUE(pipeline);
    if (scenario == Scenario::stopped_before_failing) {
      EXPECT_TRUE(pipeline.value()->request_stop());
    } else if (scenario == Scenario::destroyed_before_failing) {
      pipeline.value().reset();
    } else {
      harness.state().fail_send = false;
      harness.clock.now_us = 31'000'001;
      const auto pumped = pipeline.value()->pump_once();
      EXPECT_EQ(switches, scenario == Scenario::dont_fragment_fails ? 0U : 1U);
      EXPECT_EQ(harness.state().dont_fragment, std::vector<bool>{false});
      if (scenario == Scenario::switches ||
          scenario == Scenario::first_send_fails) {
        EXPECT_TRUE(pumped);
        EXPECT_TRUE(pipeline.value()->active());
        EXPECT_TRUE(pipeline.value()->calibration_failure());
        EXPECT_EQ(harness.state().close_count, 0U);
      } else {
        EXPECT_FALSE(pumped);
        EXPECT_FALSE(pipeline.value()->active());
        EXPECT_EQ(harness.state().close_count, 1U);
      }
      EXPECT_TRUE(pipeline.value()->request_stop());
    }
    EXPECT_EQ(harness.state().close_count, 1U);
    if (pipeline.value()) {
      EXPECT_TRUE(pipeline.value()->request_stop());
    }
    pipeline.value().reset();
    EXPECT_EQ(harness.state().close_count, 1U);
    EXPECT_EQ(switches,
              scenario == Scenario::stopped_before_failing ||
                      scenario == Scenario::destroyed_before_failing ||
                      scenario == Scenario::dont_fragment_fails
                  ? 0U
                  : 1U);
  }
};

TEST_F(LocalCalibratedPipelineRetainedRoute,
       SwitchesToTheRetainedRouteAfterAFailedCalibration) {
  run(Scenario::switches);
}

TEST_F(LocalCalibratedPipelineRetainedRoute,
       KeepsTheRouteUntilAStopBeforeFailing) {
  run(Scenario::stopped_before_failing);
}

TEST_F(LocalCalibratedPipelineRetainedRoute, FailsWhenTheSwitchClosesTheRoute) {
  run(Scenario::switch_closes_the_route);
}

TEST_F(LocalCalibratedPipelineRetainedRoute,
       ClosesTheRouteOnceWhenTheActiveStartFails) {
  run(Scenario::active_start_fails);
}

TEST_F(LocalCalibratedPipelineRetainedRoute,
       SwitchesEvenWhenTheFirstSendFailed) {
  run(Scenario::first_send_fails);
}

TEST_F(LocalCalibratedPipelineRetainedRoute,
       ClosesTheRouteOnceWhenDestroyedBeforeFailing) {
  run(Scenario::destroyed_before_failing);
}

TEST_F(LocalCalibratedPipelineRetainedRoute,
       NeverSwitchesWhenDontFragmentCannotBeCleared) {
  run(Scenario::dont_fragment_fails);
}

class LocalCalibratedPipelineHandoff : public LocalPipelineRig {
 protected:
  LocalPipelineCrypto crypto;
  test_support::RefusingTakionCrypto takion_crypto;
  RegisteredConsole registered = console(RegistrationTarget::ps4_9);
  CandidateRoute route = candidate_route();
};

TEST_F(LocalCalibratedPipelineHandoff, HandsTheCalibratedRouteToActiveOnce) {
  SenkushaHarness harness;
  auto subject = single_probe_candidate_client(harness, route);
  ASSERT_NO_FATAL_FAILURE(complete(*subject, harness));
  ASSERT_EQ(subject->snapshot().state, State::completed);
  const auto sent_before = harness.state().sent.size();
  remote_play::LocalCalibratedPipeline pipeline{
      std::move(subject), ready_ticket(),
      inputs(harness, crypto, takion_crypto, registered)};
  ASSERT_TRUE(pipeline.pump_once());
  EXPECT_EQ(pipeline.state(), LocalPipelineState::active);
  EXPECT_TRUE(pipeline.active());
  EXPECT_EQ(crypto.generate_calls, 1U);
  EXPECT_EQ(harness.state().sent.size(), sent_before + 1);
  EXPECT_EQ(harness.state().close_count, 0U);
  EXPECT_TRUE(pipeline.request_stop());
  EXPECT_TRUE(pipeline.request_stop());
  EXPECT_EQ(harness.state().close_count, 1U);
  EXPECT_FALSE(pipeline.active());
  const auto stopped_pump = pipeline.pump_once();
  ASSERT_FALSE(stopped_pump);
  EXPECT_EQ(stopped_pump.error(), LocalPipelineError::invalid_state);
}

TEST_F(LocalCalibratedPipelineHandoff,
       ClosesTheRouteOnceWhenStoppedWhileCalibrating) {
  SenkushaHarness harness;
  auto calibrating = single_probe_candidate_client(harness, route);
  ASSERT_TRUE(calibrating->start());
  remote_play::LocalCalibratedPipeline cancelled{
      std::move(calibrating), ready_ticket(),
      inputs(harness, crypto, takion_crypto, registered)};
  EXPECT_TRUE(cancelled.request_stop());
  EXPECT_EQ(harness.state().close_count, 1U);
}

TEST_F(LocalCalibratedPipelineHandoff,
       ClosesTheRouteOnceWhenTheActiveStartFails) {
  SenkushaHarness harness;
  auto to_fail = single_probe_candidate_client(harness, route);
  ASSERT_NO_FATAL_FAILURE(complete(*to_fail, harness));
  crypto.fail_generate = true;
  remote_play::LocalCalibratedPipeline failed{
      std::move(to_fail), ready_ticket(),
      inputs(harness, crypto, takion_crypto, registered)};
  const auto failure = failed.pump_once();
  ASSERT_FALSE(failure);
  EXPECT_EQ(failure.error(), LocalPipelineError::active_start_failed);
  EXPECT_EQ(failed.state(), LocalPipelineState::failed);
  EXPECT_EQ(harness.state().close_count, 1U);
  EXPECT_FALSE(failed.active());
  EXPECT_TRUE(failed.request_stop());
  EXPECT_EQ(harness.state().close_count, 1U);
}

class LocalStartCalibration : public LocalPipelineRig {
 protected:
  auto direct_candidate() -> remote_play::RouteCandidate {
    return {harness.transport(),
            {},
            {},
            remote_play::RouteKind::local_direct,
            remote_play::DontFragmentPolicy::enabled};
  }

  static auto calibration_config() -> ClientConfig {
    ClientConfig config;
    config.policy.probe.count = 1;
    config.profile.handoff = TransportHandoffPolicy::close_after_use;
    return config;
  }

  auto calibration_inputs() -> remote_play::LocalCalibrationInputs {
    return {inputs(harness, crypto, takion_crypto, registered), harness.events,
            calibration_config()};
  }

  LocalPipelineCrypto crypto;
  test_support::RefusingTakionCrypto takion_crypto;
  RegisteredConsole registered = console(RegistrationTarget::ps4_9);
  SenkushaHarness harness;
};

TEST_F(LocalStartCalibration, StartsAndOwnsOneDirectAttempt) {
  auto started = remote_play::LocalStart::start_calibration(
      direct_candidate(), ready_ticket(), calibration_inputs());
  ASSERT_TRUE(started);
  EXPECT_EQ(started.value()->state(), LocalPipelineState::calibrating);
  EXPECT_EQ(harness.state().sent.size(), 1U);
  EXPECT_EQ(harness.state().close_count, 0U);
  EXPECT_TRUE(started.value()->request_stop());
  EXPECT_EQ(harness.state().close_count, 1U);
}

TEST_F(LocalStartCalibration, RefusesATicketWithoutAnAttemptAndClosesTheRoute) {
  auto ticket = ready_ticket();
  ticket.attempt_id = 0;
  const auto rejected = remote_play::LocalStart::start_calibration(
      direct_candidate(), std::move(ticket), calibration_inputs());
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(),
            remote_play::LocalCalibrationStartError::invalid_ticket);
  EXPECT_TRUE(harness.state().sent.empty());
  EXPECT_EQ(harness.state().close_count, 1U);
}

TEST_F(LocalStartCalibration, ReportsASenkushaStartFailureAndClosesTheRoute) {
  harness.random.fail = true;
  const auto failed = remote_play::LocalStart::start_calibration(
      direct_candidate(), ready_ticket(), calibration_inputs());
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(),
            remote_play::LocalCalibrationStartError::senkusha_start_failed);
  EXPECT_EQ(harness.state().close_count, 1U);
}

class LocalPipelineLifecycle : public ::testing::Test {
 protected:
  using Lifecycle = remote_play::local_pipeline_lifecycle::Lifecycle;
  using Effect = std::optional<remote_play::local_pipeline_lifecycle::Effect>;
  using CalibrationFailed =
      remote_play::local_pipeline_lifecycle::CalibrationFailed;
  using LocalPipelineState = remote_play::LocalPipelineState;

  static auto recovery(bool retained_allowed, bool fresh_allowed,
                       CalibrationFailed failure) -> Effect {
    Lifecycle machine{retained_allowed, fresh_allowed};
    EXPECT_TRUE(machine.process(failure));
    auto effect = machine.take_effect();
    EXPECT_EQ(machine.state(), effect ? LocalPipelineState::calibrating
                                      : LocalPipelineState::failed);
    return effect;
  }

  static auto is_retained(const Effect& effect) -> bool {
    return effect &&
           std::holds_alternative<
               remote_play::local_pipeline_lifecycle::OpenRetainedRoute>(
               *effect);
  }

  static auto is_fresh(const Effect& effect) -> bool {
    return effect &&
           std::holds_alternative<
               remote_play::local_pipeline_lifecycle::OpenFreshRoute>(*effect);
  }
};

TEST_F(LocalPipelineLifecycle, ChoosesOneRecoveryRouteForAFailedCalibration) {
  EXPECT_TRUE(is_retained(recovery(true, true, {false, true})));
  EXPECT_TRUE(is_fresh(recovery(true, true, {false, false})));
  EXPECT_TRUE(is_fresh(recovery(false, true, {false, true})));
  EXPECT_FALSE(recovery(true, true, {true, true}));
  EXPECT_FALSE(recovery(false, false, {false, true}));
}

TEST_F(LocalPipelineLifecycle, DiscardsEverythingWhenStoppedAfterOpeningFails) {
  namespace lifecycle = remote_play::local_pipeline_lifecycle;
  Lifecycle opening{false, true};
  EXPECT_TRUE(opening.process(lifecycle::CalibrationSucceeded{}));
  EXPECT_FALSE(opening.process(lifecycle::StopRequested{}));
  EXPECT_TRUE(opening.process(lifecycle::OpeningFailed{}));
  EXPECT_EQ(opening.state(), LocalPipelineState::failed);
  EXPECT_TRUE(opening.process(lifecycle::StopRequested{}));
  const auto discard = opening.take_effect();
  ASSERT_TRUE(discard);
  EXPECT_TRUE(std::holds_alternative<lifecycle::DiscardAll>(*discard));
}

TEST_F(LocalPipelineLifecycle, StopsTheActiveSessionAndFailsWhenReleaseFails) {
  namespace lifecycle = remote_play::local_pipeline_lifecycle;
  Lifecycle running{false, false};
  EXPECT_TRUE(running.process(lifecycle::CalibrationSkipped{}));
  EXPECT_TRUE(running.process(lifecycle::ActiveStarted{}));
  EXPECT_EQ(running.state(), LocalPipelineState::active);
  EXPECT_TRUE(running.process(lifecycle::StopRequested{}));
  const auto stop = running.take_effect();
  ASSERT_TRUE(stop);
  EXPECT_TRUE(std::holds_alternative<lifecycle::StopActive>(*stop));
  EXPECT_TRUE(running.process(lifecycle::ReleaseFailed{}));
  EXPECT_EQ(running.state(), LocalPipelineState::failed);
  EXPECT_FALSE(running.process(lifecycle::ActiveClosed{}));
}

}  // namespace
}  // namespace senkusha_test
