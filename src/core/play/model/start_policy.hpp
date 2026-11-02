/// @file
/// @brief Declares StartPolicy, the rules for whether a stream may start and
/// whether a failed start is worth another attempt.
#pragma once

#include "libchiaking/detail/play/outcome.hpp"
#include "libchiaking/detail/play/preferences.hpp"
#include "libchiaking/detail/play/session.hpp"
#include "libchiaking/detail/play/start_refusal.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "src/core/play/model/start_lifecycle.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace libchiaking::play {

/// @brief Decides whether a player may start a stream and, after an attempt
/// fails, whether to wake the console and try again or give up.
///
/// The static members are pure rules. An instance counts the attempts of one
/// start at one Where and remembers when it gave up; it belongs to the
/// ReconnectingLink driving that start. A console at home gets
/// StartPolicy::home_attempts attempts, one over the internet
/// StartPolicy::internet_attempts, a cloud stream one. Not synchronised; use
/// an instance from one thread.
class StartPolicy {
 public:
  /// @brief The player's situation when a start is asked for.
  struct Standing {
    /// @brief Whether a session is already live on this ChiakiNg.
    bool session_live{};
    /// @brief Whether an internet pairing is in progress.
    bool pairing{};
    /// @brief Whether the player has a PSN account attached.
    bool has_psn_account{};
    /// @brief The player's PSN sign-in state; signed out until set.
    users::SignInState sign_in{users::SignInState::signed_out};
  };

  /// @brief What is known about why an attempt ended.
  struct Failure {
    /// @brief The stream's end cause, when the attempt failed with one.
    std::optional<EndReason::Cause> cause{};
    /// @brief Why the console ended the session, when it said.
    std::optional<ConsoleQuit::Reason> quit{};
    /// @brief Whether the attempt ended because the player cancelled it.
    bool left{};
    /// @brief How long the attempt streamed before it ended, empty when it
    /// never streamed; an attempt that streamed ended in a drop rather than
    /// a failed start.
    std::optional<std::chrono::microseconds> played_for{};
    /// @brief How long before the end the stream last showed network
    /// trouble, empty when it showed none.
    std::optional<std::chrono::microseconds> since_trouble{};
    /// @brief How long this machine had been without network when the
    /// attempt failed, from the first failure in a row that found none;
    /// empty when the attempt reached the network. An attempt without
    /// network never reached PSN or the console.
    std::optional<std::chrono::microseconds> offline_for{};
  };

  /// @brief What to do after a failed attempt.
  enum class Next : std::uint8_t {
    /// @brief Stop trying; the failure stands.
    give_up,
    /// @brief Rest for StartPolicy::rest_between_attempts, then start again.
    try_again,
    /// @brief Wake the console, rest, then start again. Chosen at home after
    /// the first failed attempt, and again every refused_wake_every attempts
    /// while the console refuses the connection.
    wake_then_try_again,
  };

  /// @brief Attempts allowed for a console on the local network, the first
  /// included.
  static constexpr std::uint32_t home_attempts = 13;
  /// @brief Attempts allowed for a console reached over the internet, the first
  /// included.
  static constexpr std::uint32_t internet_attempts = 2;
  /// @brief How many attempts apart the console is woken again while it
  /// keeps refusing the connection at home, which means it is not listening
  /// yet: a wake-up sent just as the Mac wakes can be lost before its network
  /// is back, and waking an awake console does nothing.
  static constexpr std::uint32_t refused_wake_every = 3;
  /// @brief How long to rest between one attempt and the next.
  static constexpr std::chrono::seconds rest_between_attempts{2};
  /// @brief Extra attempts, two seconds apart, a home or internet start gets
  /// while a console still closing this client's last session reports Remote
  /// Play in use.
  static constexpr std::uint32_t handover_attempts = 6;
  /// @brief Reconnections an internet stream gets after it drops for a
  /// network reason, StartPolicy::run_attempts; counted afresh only once a
  /// reconnection has streamed for StartPolicy::settled_play, so a link that
  /// keeps dropping gives up.
  static constexpr std::uint32_t reconnect_attempts = 3;
  /// @brief How long a reconnection must stream before its next drop gets a
  /// fresh set of reconnections.
  static constexpr std::chrono::seconds settled_play{60};
  /// @brief How shortly before a console ended the session network trouble
  /// must have shown for the end to count as a drop.
  static constexpr std::chrono::seconds trouble_window{15};
  /// @brief How long reconnections keep trying while this machine has no
  /// network without counting those tries against
  /// StartPolicy::run_attempts, measured by Failure::offline_for.
  static constexpr std::chrono::seconds offline_wait{90};

  /// @brief Starts counting attempts of one start at the first attempt.
  ///
  /// @param where Where the stream is being started.
  /// @param handover Whether a home or internet start follows this client's
  ///     own session with the same console, which earns
  ///     StartPolicy::handover_attempts more tries while the console reports
  ///     Remote Play in use; ignored for the cloud.
  /// @param reconnect Whether a drop after streaming is reconnected.
  explicit StartPolicy(Where where,
                       ConsoleHandover handover = ConsoleHandover::fresh,
                       bool reconnect = true) noexcept
      : where_(where),
        handover_left_(where != Where::cloud &&
                               handover == ConsoleHandover::just_left
                           ? handover_attempts
                           : 0),
        reconnection_(reconnect && where != Where::cloud
                          ? Reconnection::allowed
                          : Reconnection::refused) {}

  /// @brief Decides whether a start may begin at all.
  ///
  /// @param where Where the stream would run.
  /// @param standing The player's situation.
  /// @return StartRefusal::busy when a session is live or a pairing runs, at
  ///     any Where; for internet and cloud, StartRefusal::needs_psn_sign_in
  ///     when there is no account or the player is signed out and
  ///     StartRefusal::sign_in_expired when the sign-in expired; empty when the
  ///     start may begin. A home start needs no sign-in.
  [[nodiscard]] static auto refusal(Where where, const Standing& standing)
      -> std::optional<StartRefusal>;
  /// @brief Reads the player's situation for StartPolicy::refusal.
  ///
  /// @param player The player who wants to start.
  /// @param session_live Whether a session is already live.
  /// @param pairing Whether an internet pairing is in progress.
  /// @return The standing, with the account and sign-in read from @p player.
  static auto standing_of(const users::PsUser& player, bool session_live,
                          bool pairing) -> Standing;
  /// @brief Reports whether an attempt ended because someone meant it to.
  ///
  /// @param failure What is known about the end.
  /// @return `true` when the player left, or the console said the session was
  ///     stopped, the console is in use, it disconnected, it shut down, the
  ///     streaming time limit was reached or the game trial ended.
  static auto ended_on_purpose(const Failure& failure) noexcept -> bool;

  /// @brief Whether a failed attempt is waiting for this machine's network to
  /// return, so it does not count against the run.
  ///
  /// @param failure The failed attempt.
  /// @return `true` when the machine had been offline for less than
  ///     StartPolicy::offline_wait when the attempt failed.
  static auto waiting_for_network(const Failure& failure) noexcept -> bool;
  /// @brief Reports whether a failure is the kind another attempt can fix.
  ///
  /// @param where Where the stream was started.
  /// @param failure What is known about the end.
  /// @return `false` for anything ended on purpose. At home, `true` for control
  ///     or session-request network failures, timeouts and refused connections,
  ///     or, when no cause is known, for a console quit that reports an unknown
  ///     or refused session request. Over the internet, `true` for a failed
  ///     route and control or session-request network failures. Never `true`
  ///     for the cloud.
  static auto worth_another_try(Where where, const Failure& failure) noexcept
      -> bool;
  /// @brief Reports how many attempts a start gets.
  ///
  /// @param where Where the stream is being started.
  /// @return StartPolicy::home_attempts, StartPolicy::internet_attempts, or 1
  ///     for the cloud.
  static auto attempts(Where where) noexcept -> std::uint32_t;

  /// @brief Decides what follows a failed attempt and counts the next one.
  ///
  /// Once it gives up it keeps giving up.
  ///
  /// An in-use refusal during a handover is retried without counting against
  /// StartPolicy::attempts or the first-failure wake.
  ///
  /// @param failure What is known about the failed attempt.
  /// @return Next::try_again for an in-use refusal while handover tries are
  ///     left; Next::give_up when the failure is not worth another try or the
  ///     counted attempts are spent; Next::wake_then_try_again after the first
  ///     counted failure at home, and after every refused_wake_every-th one
  ///     that the console refused; otherwise Next::try_again. An attempt
  ///     that had streamed and then failed in a way worth another try, or
  ///     dropped as StartPolicy::dropped says, is given up when reconnection
  ///     is off, and otherwise starts a run of StartPolicy::run_attempts
  ///     reconnections, unless one is running and the attempt streamed for
  ///     less than StartPolicy::settled_play; StartPolicy::next_in_run then
  ///     decides each step.
  auto after(const Failure& failure) noexcept -> Next;
  /// @brief Reports whether an attempt that had streamed ended because the
  /// network dropped it.
  ///
  /// @param failure What is known about the end.
  /// @return `false` when the player left. `true` when the console ended the
  ///     session, which it reports the same way whoever ended it, but the
  ///     stream showed network trouble within StartPolicy::trouble_window
  ///     before. Otherwise, unless it
  ///     ended on purpose, `true` for a lost connection, a control network
  ///     failure or timeout, a failed route, or a session request network
  ///     failure or timeout.
  static auto dropped(const Failure& failure) noexcept -> bool;
  /// @brief Reports how many reconnections one run after a drop gets.
  ///
  /// @param where Where the stream was started.
  /// @return StartPolicy::home_attempts less one at home, keeping a home
  ///     stream's tries as before; StartPolicy::reconnect_attempts otherwise.
  static auto run_attempts(Where where) noexcept -> std::uint32_t;
  /// @brief Reads the number of the current attempt.
  ///
  /// @return 1 for the first attempt, counting up with each retry, handover
  ///     tries included, so it can pass StartPolicy::attempts.
  auto attempt() const noexcept -> std::uint32_t { return attempt_; }

 private:
  /// @brief Where the stream is being started; fixed at construction.
  Where where_;
  /// @brief In-use refusals still worth waiting out after a handover.
  std::uint32_t handover_left_{};
  /// @brief Attempts spent waiting out a handover; they are numbered but do
  /// not count against StartPolicy::attempts or the first-failure wake.
  std::uint32_t handover_used_{};
  /// @brief The current attempt, from 1; advanced by StartPolicy::after.
  std::uint32_t attempt_{1};
  /// @brief Whether drops after streaming are reconnected.
  enum class Reconnection : std::uint8_t {
    /// @brief A drop after streaming starts a run of reconnections.
    allowed,
    /// @brief A drop after streaming gives up.
    refused,
  };
  /// @brief Whether drops after streaming are reconnected; fixed at
  /// construction.
  const Reconnection reconnection_;
  /// @brief Where a start stands: still starting, reconnecting after a drop
  /// or given up; whether the console was woken.
  StartLifecycle lifecycle_;
  /// @brief Reconnections left in the current run.
  std::uint32_t run_left_{};
  /// @brief Reconnections tried in the current run.
  std::uint32_t run_tries_{};

  /// @brief Decides the next step inside a run of reconnections.
  ///
  /// @param failure What is known about the end.
  /// @return Next::give_up when the failure is not worth another try or the
  ///     run is spent; Next::try_again while the console reports Remote Play
  ///     in use and the wait is not spent, and for a failure while this
  ///     machine has been without network for less than
  ///     StartPolicy::offline_wait, neither counting against the run nor
  ///     waking the console; otherwise
  ///     Next::try_again, or at
  ///     home Next::wake_then_try_again on a run's first reconnection when
  ///     the session has not woken the console yet, and on every
  ///     refused_wake_every-th refused one.
  auto next_in_run(const Failure& failure) noexcept -> Next;
};

}  // namespace libchiaking::play
