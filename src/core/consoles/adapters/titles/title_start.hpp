/// @file
/// @brief Declares TitleStart, which asks PSN to start an installed game on
/// one of the account's PS5 consoles and follows the start command until
/// PSN reports it completed or failed.
#pragma once

#include "libchiaking/detail/consoles/console_titles.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/net/transport/http/endpoint.hpp"
#include "src/core/psn/rest/cloud_assisted_navigation/cloud_assisted_navigation.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace libchiaking::platform {
class DiagnosticsProvider;
class HttpProvider;
}  // namespace libchiaking::platform

namespace libchiaking::users {
class ConsoleCalls;
}  // namespace libchiaking::users

namespace libchiaking::consoles {

/// @brief One start of a title through PSN's console commands.
///
/// run() sends the start command, then polls its state, pacing the polls by
/// the frequency PSN hints, until a final state, the deadline, cancellation
/// or a failure that cannot improve. Every state change is logged and told
/// to the watcher.
///
/// Holds only references and its pace; one object may serve one start at a
/// time. The providers and waiter must outlive it.
class TitleStart {
 public:
  /// @brief How long a start may take in all by default.
  static constexpr std::chrono::milliseconds default_deadline{10000};
  /// @brief The HTTP status PSN forbids a party command with.
  static constexpr std::uint16_t forbidden_status = 403;
  /// @brief Time between polls when PSN hints nothing.
  static constexpr std::chrono::milliseconds default_poll{1500};
  /// @brief Shortest time between polls PSN may ask for.
  static constexpr std::chrono::milliseconds shortest_poll{500};
  /// @brief Longest time between polls PSN may ask for.
  static constexpr std::chrono::milliseconds longest_poll{10000};

  /// @brief How long a start may take and how often it is polled.
  struct Pace {
    /// @brief How long the start may take in all, from sending the command.
    std::chrono::milliseconds deadline{default_deadline};
    /// @brief Time between polls when PSN hints nothing.
    std::chrono::milliseconds poll{default_poll};
  };

  /// @brief Waits between polls by sleeping on the platform's threading
  /// provider, waking at once when a stop is requested.
  class Pause final : public net::http::Waiter {
   public:
    /// @brief Makes a pause that sleeps on the given providers.
    ///
    /// @param threading Makes the sleeper's lock and condition variable; must
    ///     outlive this object.
    /// @param clock The monotonic clock sleeps are measured by; must outlive
    ///     this object.
    Pause(platform::ThreadingProvider& threading,
          const platform::ClockProvider& clock)
        : sleeper_(threading, clock) {}

    /// @brief Sleeps for a time on the monotonic clock unless stopped.
    ///
    /// @param milliseconds How long to sleep.
    /// @param stop Ends the sleep early.
    /// @return True when the time passed, false when stopped or the time is
    ///     negative.
    [[nodiscard]] auto wait_milliseconds(int milliseconds,
                                         const platform::StopToken& stop)
        -> bool override;

   private:
    /// @brief Parks the calling thread for each wait.
    platform::Sleeper sleeper_;
  };

  /// @brief Makes a start with the default pace and reply limit.
  ///
  /// @param http The HTTP provider; must outlive this object.
  /// @param waiter Waits between polls; must outlive this object.
  /// @param clock The monotonic clock the deadline is measured by; must
  ///     outlive this object.
  /// @param log Where the start's progress is logged; null logs nothing.
  TitleStart(platform::HttpProvider& http, net::http::Waiter& waiter,
             const platform::ClockProvider& clock,
             platform::DiagnosticsProvider* log = nullptr) noexcept;
  /// @brief Makes a start with a given pace.
  ///
  /// @param http The HTTP provider; must outlive this object.
  /// @param waiter Waits between polls; must outlive this object.
  /// @param clock The monotonic clock the deadline is measured by; must
  ///     outlive this object.
  /// @param log Where the start's progress is logged; null logs nothing.
  /// @param pace The deadline and default poll interval.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  TitleStart(platform::HttpProvider& http, net::http::Waiter& waiter,
             const platform::ClockProvider& clock,
             platform::DiagnosticsProvider* log, Pace pace,
             std::size_t maximum_response_bytes =
                 net::http::default_maximum_response_bytes) noexcept;

  /// @brief Starts a title on a console and follows the command.
  ///
  /// Blocks the calling thread for up to the pace's deadline. A poll PSN does
  /// not answer is logged and retried; one that shows the sign-in or app
  /// token gone, or cancellation, ends the start.
  ///
  /// @param as The user whose account holds the console.
  /// @param console_duid The console's device uid as PSN lists it.
  /// @param title The title; started by its start id.
  /// @param watcher Told each new state, starting with StartState::pending
  ///     once the command is accepted; may be null.
  /// @param stop Cancels the start.
  /// @return StartState::completed or StartState::failed, as PSN reported.
  /// @retval StartFailure::Reason::not_on_account The duid is blank.
  /// @retval StartFailure::Reason::refused PSN refused the command with a
  ///     client error other than an authorisation refusal.
  /// @retval StartFailure::Reason::cancelled `stop` fired.
  /// @retval StartFailure::Reason::timed_out No final state before the
  ///     deadline.
  /// @retval StartFailure::Reason::unanswered PSN did not answer the command,
  ///     or a poll showed the sign-in or app token gone.
  [[nodiscard]] auto run(users::PsUser& as, std::string_view console_duid,
                         const InstalledTitle& title, StartWatcher* watcher,
                         const platform::StopToken& stop)
      -> Expected<StartState, StartFailure>;

  /// @brief Has a console join a game session and follows the command, which
  /// accepts an invitation.
  ///
  /// Sends PSN's `sendGameIntent` command, then follows it exactly as run()
  /// follows a start.
  ///
  /// @param as The user whose account holds the console.
  /// @param console_duid The console's device uid as PSN lists it.
  /// @param platform The console's platform, such as `PS5` or `PS4`; the
  ///     game intent command carries it.
  /// @param game_intent The session's tokenised player game intent.
  /// @param watcher Told each new state; may be null.
  /// @param stop Cancels the join.
  /// @return StartState::completed or StartState::failed, as PSN reported.
  /// @retval StartFailure The failures run() lists.
  [[nodiscard]] auto join(users::PsUser& as, std::string_view console_duid,
                          std::string_view platform,
                          std::string_view game_intent, StartWatcher* watcher,
                          const platform::StopToken& stop)
      -> Expected<StartState, StartFailure>;

  /// @brief Has a console join a party and follows the command, which moves
  /// the party's voice chat to the console.
  ///
  /// Sends PSN's `party` command, waiting no longer than default_deadline for
  /// PSN to accept it, then follows it exactly as run() follows a start. PSN
  /// forbids the command for some consoles, such as a cloud console, with
  /// HTTP 403 even after the app token is renewed; that is a refusal, not a
  /// lapsed sign-in.
  ///
  /// @param as The user whose account holds the console.
  /// @param console_duid The console's device uid as PSN lists it.
  /// @param platform The console's platform, such as `PS5`.
  /// @param session_id The party session.
  /// @param watcher Told each new state; may be null.
  /// @param stop Cancels the join.
  /// @return StartState::completed or StartState::failed, as PSN reported.
  /// @retval StartFailure::Reason::refused PSN still forbade the command
  ///     with HTTP 403 after the app token was renewed.
  /// @retval StartFailure The failures run() lists.
  [[nodiscard]] auto join_party(users::PsUser& as,
                                std::string_view console_duid,
                                std::string_view platform,
                                std::string_view session_id,
                                StartWatcher* watcher,
                                const platform::StopToken& stop)
      -> Expected<StartState, StartFailure>;

  /// @brief The state a poll moves a start to.
  ///
  /// @param current The state so far.
  /// @param status The command state PSN returned.
  /// @return completed or failed when PSN says so (ignoring case); starting
  ///     when a pending start's action type is `command`; otherwise
  ///     `current`.
  static auto next(StartState current,
                   const psn::rest::CommandState& status) noexcept
      -> StartState;
  /// @brief The start failure for a console whose titles could not be read.
  ///
  /// @param why Why the titles were unavailable.
  /// @return The matching start failure, in StartState::pending.
  static auto not_started(const TitlesUnavailable& why) noexcept
      -> StartFailure;

 private:
  /// @brief The monotonic time.
  ///
  /// @return Milliseconds on the clock.
  auto now_ms() const noexcept -> std::int64_t;

  /// @brief Follows an accepted command until a final state, the deadline,
  /// cancellation or a failure that cannot improve.
  ///
  /// @param http The client the polls are sent through.
  /// @param calls The user's calls.
  /// @param command_id The accepted command.
  /// @param mobile Whether the command was created as a mobile client (a join);
  ///     its polls then carry the matching header.
  /// @param platform The command's platform, such as `PS5` or `PS4`.
  /// @param watcher Told each new state; may be null.
  /// @return StartState::completed or StartState::failed, as PSN reported.
  /// @retval StartFailure::Reason::cancelled The client's stop token fired.
  /// @retval StartFailure::Reason::timed_out No final state before the
  ///     deadline.
  /// @retval StartFailure::Reason::unanswered A poll showed the sign-in or
  ///     app token gone.
  [[nodiscard]] auto follow(net::http::Client& http, users::ConsoleCalls& calls,
                            std::string_view command_id, bool mobile,
                            std::string_view platform, StartWatcher* watcher)
      -> Expected<StartState, StartFailure>;
  /// @brief The start failure for a PSN failure.
  ///
  /// @param why PSN's failure.
  /// @param reached The state reached.
  /// @return Reason::cancelled for a cancellation, Reason::refused for a
  ///     client error that is not an authorisation refusal, otherwise
  ///     Reason::unanswered; each carries `why`.
  static auto failed(const users::Unanswered& why, StartState reached) noexcept
      -> StartFailure;
  /// @brief Whether a failed poll ends the start rather than being retried.
  ///
  /// @param why The poll's failure.
  /// @return True for cancellation, being off PSN, an expired sign-in or a
  ///     lapsed app token.
  static auto gives_up(const users::Unanswered& why) noexcept -> bool;
  /// @brief The wait before the next poll that PSN's hint asks for.
  ///
  /// @param frequency PSN's hint.
  /// @return The hint, clamped to shortest_poll and longest_poll.
  static auto hinted(std::chrono::seconds frequency) noexcept
      -> std::chrono::milliseconds;
  /// @brief A state's name, for the log.
  ///
  /// @param state The state.
  /// @return Its name.
  static auto name_of(StartState state) noexcept -> std::string_view;

  /// @brief The HTTP provider.
  platform::HttpProvider& http_;
  /// @brief Waits between polls.
  net::http::Waiter& waiter_;
  /// @brief The monotonic clock.
  const platform::ClockProvider& clock_;
  /// @brief Where progress is logged; null when nothing is logged.
  platform::DiagnosticsProvider* log_;
  /// @brief The deadline and default poll interval.
  Pace pace_;
  /// @brief Largest reply accepted, in bytes.
  std::size_t maximum_response_bytes_;
};

}  // namespace libchiaking::consoles
