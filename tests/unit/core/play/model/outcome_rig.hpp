#pragma once

#include "src/core/net/protocol/remote_play/rp_reason.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/outcome_checks.hpp"
#include "tests/support/play/session_rig.hpp"

#include <libchiaking/detail/play/outcome.hpp>
#include <libchiaking/testing/play/scripted_link.hpp>

#include <gtest/gtest.h>

#include <array>
#include <functional>
#include <optional>

namespace play_test {

using test_support::play::SessionRig;

class OutcomeRig : public ::testing::Test {
 protected:
  using StreamError = libchiaking::net::protocol::remote_play::StreamError;
  using QuitReason = libchiaking::net::protocol::remote_play::QuitReason;
  using MediaFailure = libchiaking::media::MediaFailure;
  using MediaFault = libchiaking::media::MediaFault;
  using ConsoleQuit = libchiaking::play::ConsoleQuit;
  using EndReason = libchiaking::play::EndReason;
  using Outcome = libchiaking::play::Outcome;
  using Cause = EndReason::Cause;
  using Ending = Outcome::Ending;
  using Phase = Outcome::Phase;
  using Script = std::function<void(libchiaking::play::testing::ScriptedLink&)>;

  struct CausePair {
    StreamError today;
    Cause typed;
  };

  struct QuitPair {
    QuitReason today;
    ConsoleQuit::Reason typed;
  };

  struct Ended {
    Ending ending;
    std::optional<Cause> cause;
  };

  static constexpr std::array causes{
      CausePair{StreamError::invalid_state, Cause::invalid_state},
      CausePair{StreamError::invalid_configuration, Cause::invalid_preferences},
      CausePair{StreamError::provider_unavailable, Cause::provider_unavailable},
      CausePair{StreamError::ctrl_failed, Cause::control_failed},
      CausePair{StreamError::route_failed, Cause::route_failed},
      CausePair{StreamError::pipeline_failed, Cause::pipeline_failed},
      CausePair{StreamError::controller_rejected, Cause::controller_rejected},
      CausePair{StreamError::pin_rejected, Cause::login_pin_rejected},
      CausePair{StreamError::shutdown_failed, Cause::shutdown_failed},
      CausePair{StreamError::ctrl_network_failure,
                Cause::control_network_failure},
      CausePair{StreamError::ctrl_timeout, Cause::control_timeout},
      CausePair{StreamError::ctrl_truncated, Cause::control_truncated},
      CausePair{StreamError::ctrl_invalid_response,
                Cause::control_invalid_response},
      CausePair{StreamError::ctrl_too_large, Cause::control_too_large},
      CausePair{StreamError::ctrl_refused, Cause::control_refused},
      CausePair{StreamError::ctrl_handoff_failure,
                Cause::control_handoff_failure},
      CausePair{StreamError::session_init_truncated,
                Cause::session_request_truncated},
      CausePair{StreamError::session_init_network_failure,
                Cause::session_request_network_failure},
      CausePair{StreamError::session_init_timeout,
                Cause::session_request_timeout},
      CausePair{StreamError::session_init_invalid_response,
                Cause::session_request_invalid_response},
      CausePair{StreamError::session_init_too_large,
                Cause::session_request_too_large},
      CausePair{StreamError::session_init_refused,
                Cause::session_request_refused},
      CausePair{StreamError::session_init_version_mismatch,
                Cause::session_request_version_mismatch},
      CausePair{StreamError::session_init_in_use,
                Cause::session_request_in_use},
      CausePair{StreamError::invalid_argument, Cause::invalid_argument},
      CausePair{StreamError::session_init_connection_refused,
                Cause::session_request_connection_refused},
      CausePair{StreamError::ctrl_connection_refused,
                Cause::control_connection_refused},
      CausePair{StreamError::resource_exhausted, Cause::resource_exhausted},
  };

  static constexpr std::array quits{
      QuitPair{QuitReason::stopped, ConsoleQuit::Reason::stopped},
      QuitPair{QuitReason::session_request_unknown,
               ConsoleQuit::Reason::session_request_unknown},
      QuitPair{QuitReason::session_request_connection_refused,
               ConsoleQuit::Reason::session_request_connection_refused},
      QuitPair{QuitReason::session_request_rp_in_use,
               ConsoleQuit::Reason::console_in_use},
      QuitPair{QuitReason::session_request_rp_crash,
               ConsoleQuit::Reason::remote_play_crashed},
      QuitPair{QuitReason::session_request_rp_version_mismatch,
               ConsoleQuit::Reason::remote_play_version_mismatch},
      QuitPair{QuitReason::ctrl_unknown, ConsoleQuit::Reason::control_unknown},
      QuitPair{QuitReason::ctrl_connect_failed,
               ConsoleQuit::Reason::control_connect_failed},
      QuitPair{QuitReason::ctrl_connection_refused,
               ConsoleQuit::Reason::control_connection_refused},
      QuitPair{QuitReason::stream_connection_unknown,
               ConsoleQuit::Reason::stream_unknown},
      QuitPair{QuitReason::stream_connection_remote_disconnected,
               ConsoleQuit::Reason::console_disconnected},
      QuitPair{QuitReason::stream_connection_remote_shutdown,
               ConsoleQuit::Reason::console_shut_down},
      QuitPair{QuitReason::stream_connection_time_limit_reached,
               ConsoleQuit::Reason::time_limit_reached},
      QuitPair{QuitReason::stream_connection_trial_ended,
               ConsoleQuit::Reason::trial_ended},
      QuitPair{QuitReason::psn_regist_failed,
               ConsoleQuit::Reason::internet_pairing_failed},
  };

  static constexpr std::array media_failures{
      MediaFailure::profile_validation, MediaFailure::frame_validation,
      MediaFailure::decoder_submission, MediaFailure::recovery_request,
      MediaFailure::decoder_output,     MediaFailure::audio_output};

  static constexpr std::array phases{Phase::none, Phase::start, Phase::session,
                                     Phase::session_unknown, Phase::media};

  static auto end_reason(const CausePair& cause, bool left) -> EndReason {
    return EndReason{cause.typed, {.left = left}};
  }

  static void ends(const Outcome::Evidence& evidence, const Ended& expected) {
    const Outcome outcome{evidence};
    EXPECT_EQ(outcome.ending(), expected.ending);
    EXPECT_EQ(outcome.cause(), expected.cause);
    EXPECT_EQ(outcome.succeeded(), expected.ending == Ending::finished);
    EXPECT_EQ(outcome.was_left(), evidence.left);
    EXPECT_EQ(outcome.reason() != nullptr,
              evidence.primary && !evidence.primary->was_left());
    EXPECT_EQ(outcome.cleanup() != nullptr, evidence.cleanup.has_value());
  }

  static auto ended(const Script& script, bool left_before_start = false)
      -> Outcome {
    SessionRig rig;
    script(rig.link);
    if (left_before_start) {
      EXPECT_TRUE(rig.session->leave());
    }
    rig.begin();
    const auto outcome = rig.session->wait_until_ended();
    const auto told = rig.events.ended();
    EXPECT_TRUE(told);
    if (told) {
      EXPECT_EQ(told->ending(), outcome.ending());
      EXPECT_EQ(told->cause(), outcome.cause());
      EXPECT_EQ(told->trace(), outcome.trace());
    }
    EXPECT_EQ(rig.events.count("ended"), 1u);
    return outcome;
  }
};

}  // namespace play_test
