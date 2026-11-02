#include "src/core/play/model/start_policy.hpp"

namespace libchiaking::play {

auto StartPolicy::standing_of(const users::PsUser& player, bool session_live,
                              bool pairing) -> Standing {
  return Standing{session_live, pairing, player.has_psn_account(),
                  player.sign_in_state()};
}

auto StartPolicy::refusal(Where where, const Standing& standing)
    -> std::optional<StartRefusal> {
  if (standing.session_live || standing.pairing) return StartRefusal::busy();
  if (where == Where::home) return std::nullopt;
  if (!standing.has_psn_account ||
      standing.sign_in == users::SignInState::signed_out)
    return StartRefusal::needs_psn_sign_in();
  if (standing.sign_in == users::SignInState::expired)
    return StartRefusal::sign_in_expired();
  return std::nullopt;
}

auto StartPolicy::attempts(Where where) noexcept -> std::uint32_t {
  switch (where) {
    case Where::home:
      return home_attempts;
    case Where::internet:
      return internet_attempts;
    case Where::cloud:
      break;
  }
  return 1;
}

auto StartPolicy::ended_on_purpose(const Failure& failure) noexcept -> bool {
  using Quit = ConsoleQuit::Reason;
  if (failure.left) return true;
  if (!failure.quit) return false;
  switch (*failure.quit) {
    case Quit::stopped:
    case Quit::console_in_use:
    case Quit::console_disconnected:
    case Quit::console_shut_down:
    case Quit::time_limit_reached:
    case Quit::trial_ended:
      return true;
    default:
      return false;
  }
}

auto StartPolicy::worth_another_try(Where where,
                                    const Failure& failure) noexcept -> bool {
  using Cause = EndReason::Cause;
  using Quit = ConsoleQuit::Reason;
  if (ended_on_purpose(failure)) return false;
  if (where == Where::home) {
    if (!failure.cause)
      return failure.quit == Quit::session_request_unknown ||
             failure.quit == Quit::session_request_connection_refused;
    switch (*failure.cause) {
      case Cause::control_network_failure:
      case Cause::control_timeout:
      case Cause::session_request_network_failure:
      case Cause::session_request_timeout:
      case Cause::session_request_connection_refused:
        return true;
      default:
        return false;
    }
  }
  if (where == Where::internet && failure.cause) {
    switch (*failure.cause) {
      case Cause::route_failed:
      case Cause::control_network_failure:
      case Cause::session_request_network_failure:
        return true;
      default:
        return false;
    }
  }
  return false;
}

auto StartPolicy::run_attempts(Where where) noexcept -> std::uint32_t {
  return where == Where::home ? home_attempts - 1 : reconnect_attempts;
}

auto StartPolicy::next_in_run(const Failure& failure) noexcept -> Next {
  if (!failure.left &&
      failure.cause == EndReason::Cause::session_request_in_use &&
      handover_left_ > 0) {
    --handover_left_;
    ++handover_used_;
    ++attempt_;
    return Next::try_again;
  }
  if (!(dropped(failure) || worth_another_try(where_, failure))) {
    lifecycle_.give_up();
    return Next::give_up;
  }
  if (waiting_for_network(failure)) {
    ++attempt_;
    return Next::try_again;
  }
  if (run_left_ == 0) {
    lifecycle_.give_up();
    return Next::give_up;
  }
  --run_left_;
  ++attempt_;
  const bool refused =
      failure.cause == EndReason::Cause::session_request_connection_refused ||
      (!failure.cause &&
       failure.quit == ConsoleQuit::Reason::session_request_connection_refused);
  const bool first_drop_wake = run_tries_ == 0 && !lifecycle_.woken();
  const bool refused_wake = refused && run_tries_ % refused_wake_every == 0;
  ++run_tries_;
  if (where_ != Where::home || !(first_drop_wake || refused_wake))
    return Next::try_again;
  lifecycle_.wake();
  return Next::wake_then_try_again;
}

auto StartPolicy::waiting_for_network(const Failure& failure) noexcept -> bool {
  return failure.offline_for && *failure.offline_for < offline_wait;
}

auto StartPolicy::dropped(const Failure& failure) noexcept -> bool {
  using Cause = EndReason::Cause;
  if (failure.left) return false;
  if (failure.quit == ConsoleQuit::Reason::console_disconnected &&
      failure.since_trouble && *failure.since_trouble <= trouble_window)
    return true;
  if (ended_on_purpose(failure) || !failure.cause) return false;
  switch (*failure.cause) {
    case Cause::connection_lost:
    case Cause::control_network_failure:
    case Cause::control_timeout:
    case Cause::route_failed:
    case Cause::session_request_network_failure:
    case Cause::session_request_timeout:
      return true;
    default:
      return false;
  }
}

auto StartPolicy::after(const Failure& failure) noexcept -> Next {
  if (!lifecycle_.finished() && failure.played_for) {
    if (reconnection_ == Reconnection::refused ||
        !(dropped(failure) || worth_another_try(where_, failure))) {
      lifecycle_.give_up();
      return Next::give_up;
    }
    if (!lifecycle_.reconnecting() || *failure.played_for >= settled_play) {
      lifecycle_.reconnect();
      run_left_ = run_attempts(where_);
      run_tries_ = 0;
      handover_left_ = handover_attempts;
    }
  }
  if (lifecycle_.reconnecting()) return next_in_run(failure);
  if (!lifecycle_.finished() && !failure.left && handover_left_ > 0 &&
      failure.cause == EndReason::Cause::session_request_in_use) {
    --handover_left_;
    ++handover_used_;
    ++attempt_;
    return Next::try_again;
  }
  const auto counted = attempt_ - handover_used_;
  if (lifecycle_.finished() || !worth_another_try(where_, failure) ||
      counted >= attempts(where_)) {
    lifecycle_.give_up();
    return Next::give_up;
  }
  const bool first = counted == 1;
  const bool refused =
      failure.cause == EndReason::Cause::session_request_connection_refused ||
      (!failure.cause &&
       failure.quit == ConsoleQuit::Reason::session_request_connection_refused);
  const bool wake_again = refused && (counted - 1) % refused_wake_every == 0;
  ++attempt_;
  if (where_ != Where::home || !(first || wake_again)) return Next::try_again;
  lifecycle_.wake();
  return Next::wake_then_try_again;
}

}  // namespace libchiaking::play
