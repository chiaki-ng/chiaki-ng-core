#include "src/core/play/adapters/links/link_evidence.hpp"

#include <algorithm>
#include <array>

namespace libchiaking::play {

auto LinkEvidence::causes() noexcept -> std::span<const Causes> {
  using E = net::protocol::remote_play::StreamError;
  using C = EndReason::Cause;
  static constexpr std::array table{
      Causes{E::invalid_state, C::invalid_state},
      Causes{E::invalid_configuration, C::invalid_preferences},
      Causes{E::provider_unavailable, C::provider_unavailable},
      Causes{E::ctrl_failed, C::control_failed},
      Causes{E::route_failed, C::route_failed},
      Causes{E::pipeline_failed, C::pipeline_failed},
      Causes{E::controller_rejected, C::controller_rejected},
      Causes{E::pin_rejected, C::login_pin_rejected},
      Causes{E::shutdown_failed, C::shutdown_failed},
      Causes{E::ctrl_network_failure, C::control_network_failure},
      Causes{E::ctrl_timeout, C::control_timeout},
      Causes{E::ctrl_truncated, C::control_truncated},
      Causes{E::ctrl_invalid_response, C::control_invalid_response},
      Causes{E::ctrl_too_large, C::control_too_large},
      Causes{E::ctrl_refused, C::control_refused},
      Causes{E::ctrl_handoff_failure, C::control_handoff_failure},
      Causes{E::session_init_truncated, C::session_request_truncated},
      Causes{E::session_init_network_failure,
             C::session_request_network_failure},
      Causes{E::session_init_timeout, C::session_request_timeout},
      Causes{E::session_init_invalid_response,
             C::session_request_invalid_response},
      Causes{E::session_init_too_large, C::session_request_too_large},
      Causes{E::session_init_refused, C::session_request_refused},
      Causes{E::session_init_version_mismatch,
             C::session_request_version_mismatch},
      Causes{E::session_init_in_use, C::session_request_in_use},
      Causes{E::invalid_argument, C::invalid_argument},
      Causes{E::session_init_connection_refused,
             C::session_request_connection_refused},
      Causes{E::ctrl_connection_refused, C::control_connection_refused},
      Causes{E::resource_exhausted, C::resource_exhausted},
      Causes{E::session_init_refused, C::pairing_rejected},
      Causes{E::pipeline_failed, C::connection_lost},
  };
  return table;
}

auto LinkEvidence::quits() noexcept -> std::span<const Quits> {
  using Q = net::protocol::remote_play::QuitReason;
  using R = ConsoleQuit::Reason;
  static constexpr std::array table{
      Quits{Q::stopped, R::stopped},
      Quits{Q::session_request_unknown, R::session_request_unknown},
      Quits{Q::session_request_connection_refused,
            R::session_request_connection_refused},
      Quits{Q::session_request_rp_in_use, R::console_in_use},
      Quits{Q::session_request_rp_crash, R::remote_play_crashed},
      Quits{Q::session_request_rp_version_mismatch,
            R::remote_play_version_mismatch},
      Quits{Q::ctrl_unknown, R::control_unknown},
      Quits{Q::ctrl_connect_failed, R::control_connect_failed},
      Quits{Q::ctrl_connection_refused, R::control_connection_refused},
      Quits{Q::stream_connection_unknown, R::stream_unknown},
      Quits{Q::stream_connection_remote_disconnected, R::console_disconnected},
      Quits{Q::stream_connection_remote_shutdown, R::console_shut_down},
      Quits{Q::stream_connection_time_limit_reached, R::time_limit_reached},
      Quits{Q::stream_connection_trial_ended, R::trial_ended},
      Quits{Q::psn_regist_failed, R::internet_pairing_failed},
  };
  return table;
}

auto LinkEvidence::cause(net::protocol::remote_play::StreamError error) noexcept
    -> EndReason::Cause {
  const auto table = causes();
  const auto found = std::ranges::find(table, error, &Causes::first);
  return found != table.end() ? found->second : EndReason::Cause::invalid_state;
}

auto LinkEvidence::error(EndReason::Cause cause) noexcept
    -> net::protocol::remote_play::StreamError {
  const auto table = causes();
  const auto found = std::ranges::find(table, cause, &Causes::second);
  return found != table.end()
             ? found->first
             : net::protocol::remote_play::StreamError::invalid_state;
}

auto LinkEvidence::quit(const net::protocol::remote_play::QuitDetail& quit)
    -> std::optional<ConsoleQuit> {
  const auto table = quits();
  const auto found = std::ranges::find(table, quit.reason, &Quits::first);
  if (found == table.end()) return std::nullopt;
  return ConsoleQuit{found->second, quit.text};
}

auto LinkEvidence::quit_reason(ConsoleQuit::Reason reason) noexcept
    -> net::protocol::remote_play::QuitReason {
  const auto table = quits();
  const auto found = std::ranges::find(table, reason, &Quits::second);
  return found != table.end() ? found->first
                              : net::protocol::remote_play::QuitReason::none;
}

auto LinkEvidence::rest(RestRequest::Outcome outcome) noexcept -> RestOutcome {
  switch (outcome) {
    case RestRequest::Outcome::none:
      return RestOutcome::none;
    case RestRequest::Outcome::sent:
      return RestOutcome::sent;
    case RestRequest::Outcome::refused:
      return RestOutcome::refused;
  }
  return RestOutcome::none;
}

}  // namespace libchiaking::play
