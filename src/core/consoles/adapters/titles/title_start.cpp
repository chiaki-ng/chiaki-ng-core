#include "src/core/consoles/adapters/titles/title_start.hpp"

#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/net/transport/http/status.hpp"
#include "src/core/users/ports/console_calls.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <string>

namespace libchiaking::consoles {

auto TitleStart::Pause::wait_milliseconds(int milliseconds,
                                          const platform::StopToken& stop)
    -> bool {
  if (milliseconds < 0) return false;
  return sleeper_.sleep_for(std::chrono::milliseconds{milliseconds}, stop);
}

TitleStart::TitleStart(platform::HttpProvider& http, net::http::Waiter& waiter,
                       const platform::ClockProvider& clock,
                       platform::DiagnosticsProvider* log) noexcept
    : TitleStart(http, waiter, clock, log, Pace{}) {}

TitleStart::TitleStart(platform::HttpProvider& http, net::http::Waiter& waiter,
                       const platform::ClockProvider& clock,
                       platform::DiagnosticsProvider* log, Pace pace,
                       std::size_t maximum_response_bytes) noexcept
    : http_(http),
      waiter_(waiter),
      clock_(clock),
      log_(log),
      pace_(pace),
      maximum_response_bytes_(maximum_response_bytes) {}

auto TitleStart::run(users::PsUser& as, std::string_view console_duid,
                     const InstalledTitle& title, StartWatcher* watcher,
                     const platform::StopToken& stop)
    -> Expected<StartState, StartFailure> {
  using Reason = StartFailure::Reason;
  if (text::trim(console_duid).empty())
    return unexpected(
        StartFailure{Reason::not_on_account, StartState::pending});

  net::http::Client http{http_, clock_, stop, log_, &waiter_};
  users::ConsoleCalls calls{as};
  const auto command =
      calls.start_title(http, console_duid, title.start_id(), pace_.deadline,
                        maximum_response_bytes_);
  if (!command) return unexpected(failed(command.error(), StartState::pending));
  return follow(http, calls, command.value(), true, "PS5", watcher);
}

auto TitleStart::join(users::PsUser& as, std::string_view console_duid,
                      std::string_view platform, std::string_view game_intent,
                      StartWatcher* watcher, const platform::StopToken& stop)
    -> Expected<StartState, StartFailure> {
  using Reason = StartFailure::Reason;
  if (text::trim(console_duid).empty())
    return unexpected(
        StartFailure{Reason::not_on_account, StartState::pending});

  net::http::Client http{http_, clock_, stop, log_, &waiter_};
  users::ConsoleCalls calls{as};
  const auto command =
      calls.join_session(http, console_duid, platform, game_intent,
                         pace_.deadline, maximum_response_bytes_);
  if (!command) return unexpected(failed(command.error(), StartState::pending));
  return follow(http, calls, command.value(), true, platform, watcher);
}

auto TitleStart::join_party(users::PsUser& as, std::string_view console_duid,
                            std::string_view platform,
                            std::string_view session_id, StartWatcher* watcher,
                            const platform::StopToken& stop)
    -> Expected<StartState, StartFailure> {
  using Reason = StartFailure::Reason;
  if (text::trim(console_duid).empty())
    return unexpected(
        StartFailure{Reason::not_on_account, StartState::pending});

  net::http::Client http{http_, clock_, stop, log_, &waiter_};
  users::ConsoleCalls calls{as};
  const auto command = calls.join_party(
      http, console_duid, platform, session_id,
      std::min(pace_.deadline, default_deadline), maximum_response_bytes_);
  if (!command &&
      command.error().reason ==
          users::Unanswered::Reason::store_access_lapsed &&
      command.error().http_status == forbidden_status)
    return unexpected(
        StartFailure{Reason::refused, StartState::pending, command.error()});
  if (!command) return unexpected(failed(command.error(), StartState::pending));
  return follow(http, calls, command.value(), true, platform, watcher);
}

auto TitleStart::follow(net::http::Client& http, users::ConsoleCalls& calls,
                        std::string_view command_id, bool mobile,
                        std::string_view platform, StartWatcher* watcher)
    -> Expected<StartState, StartFailure> {
  using platform::LogSeverity;
  using Reason = StartFailure::Reason;
  auto state = StartState::pending;
  const auto deadline = now_ms() + pace_.deadline.count();
  http.log(LogSeverity::info, "title start: command accepted");
  if (watcher != nullptr) watcher->on_state(state);

  for (;;) {
    auto remaining = deadline - now_ms();
    if (remaining <= 0) break;
    auto next_wait = pace_.poll;
    const auto poll = calls.title_start_progress(
        http, command_id, mobile, platform,
        std::chrono::milliseconds{remaining}, maximum_response_bytes_);
    if (!poll && gives_up(poll.error()))
      return unexpected(failed(poll.error(), state));
    if (!poll) {
      http.log(LogSeverity::debug,
               "title start: poll went unanswered (HTTP " +
                   std::to_string(poll.error().http_status) + ")");
    } else {
      if (const auto& status = poll.value().state) {
        const auto reached = next(state, *status);
        if (reached != state) {
          state = reached;
          http.log(LogSeverity::info,
                   "title start: state " + std::string{name_of(state)});
          if (watcher != nullptr) watcher->on_state(state);
        }
        if (state == StartState::completed || state == StartState::failed)
          return state;
      }
      if (const auto frequency = poll.value().frequency)
        next_wait = hinted(*frequency);
    }
    remaining = deadline - now_ms();
    if (remaining <= 0) break;
    if (!http.wait(std::min(next_wait, std::chrono::milliseconds{remaining})))
      return unexpected(StartFailure{Reason::cancelled, state});
  }
  http.log(LogSeverity::info, "title start: deadline reached in state " +
                                  std::string{name_of(state)});
  return unexpected(StartFailure{Reason::timed_out, state});
}

auto TitleStart::next(StartState current,
                      const psn::rest::CommandState& status) noexcept
    -> StartState {
  if (text::ieq(status.status, "completed")) return StartState::completed;
  if (text::ieq(status.status, "failed")) return StartState::failed;
  if (text::ieq(status.action_type, "command") &&
      current == StartState::pending)
    return StartState::starting;
  return current;
}

auto TitleStart::not_started(const TitlesUnavailable& why) noexcept
    -> StartFailure {
  using Reason = StartFailure::Reason;
  switch (why.reason) {
    case TitlesUnavailable::Reason::ps5_only:
      return {Reason::ps5_only};
    case TitlesUnavailable::Reason::not_paired:
      return {Reason::not_paired};
    case TitlesUnavailable::Reason::not_on_account:
      return {Reason::not_on_account};
    case TitlesUnavailable::Reason::forgotten:
      return {Reason::forgotten};
    case TitlesUnavailable::Reason::unanswered:
      if (why.unanswered) return failed(*why.unanswered, StartState::pending);
      break;
    case TitlesUnavailable::Reason::not_available:
      break;
  }
  return {Reason::not_available};
}

auto TitleStart::now_ms() const noexcept -> std::int64_t {
  return static_cast<std::int64_t>(clock_.now_microseconds() /
                                   microseconds_per_millisecond);
}

auto TitleStart::failed(const users::Unanswered& why,
                        StartState reached) noexcept -> StartFailure {
  using Reason = StartFailure::Reason;
  if (why.reason == users::Unanswered::Reason::cancelled)
    return {Reason::cancelled, reached, why};
  if (why.reason == users::Unanswered::Reason::unavailable &&
      net::HttpStatus{why.http_status}.is_client_error() &&
      !net::HttpStatus{why.http_status}.is_auth_refused())
    return {Reason::refused, reached, why};
  return {Reason::unanswered, reached, why};
}

auto TitleStart::gives_up(const users::Unanswered& why) noexcept -> bool {
  using Reason = users::Unanswered::Reason;
  return why.reason == Reason::cancelled || why.reason == Reason::signed_out ||
         why.reason == Reason::sign_in_expired ||
         why.reason == Reason::store_access_lapsed;
}

auto TitleStart::hinted(std::chrono::seconds frequency) noexcept
    -> std::chrono::milliseconds {
  return std::clamp(
      std::chrono::duration_cast<std::chrono::milliseconds>(frequency),
      shortest_poll, longest_poll);
}

auto TitleStart::name_of(StartState state) noexcept -> std::string_view {
  switch (state) {
    case StartState::pending:
      return "pending";
    case StartState::starting:
      return "starting";
    case StartState::completed:
      return "completed";
    case StartState::failed:
      return "failed";
  }
  return "unknown";
}

}  // namespace libchiaking::consoles
