/// @file
/// @brief Declares SessionLifecycle, the state a live session is in and
/// whether the player is leaving it or the host is going to sleep, and the
/// Boost.SML machine behind it.
#pragma once

#include <boost/sml.hpp>

#include <cstdint>

namespace libchiaking::play::session_lifecycle {

/// @brief Run state: the session object exists but its stream has not
/// started. The initial run state.
struct Created {};
/// @brief Run state: the stream started and its loop is running.
struct Running {};
/// @brief Run state: the session ended; final.
struct Finished {};

/// @brief Exit state: nobody has asked the session to end. The initial exit
/// state.
struct Staying {};
/// @brief Exit state: the player, or a rest that was not for sleep, is ending
/// the session.
struct Leaving {};
/// @brief Exit state: the session is being parked because the host device is
/// going to sleep, or because the console went to rest for a host sleep.
struct Sleeping {};

/// @brief Event: the stream started; moves Created to Running.
struct Begin {};
/// @brief Event: the session ended, from any run state.
struct Finish {};
/// @brief Event: the player asked to leave; moves Staying to Leaving.
struct Cancel {};
/// @brief Event: the host is going to sleep while the session runs; moves
/// Staying to Sleeping.
struct Sleep {};
/// @brief Event: a rest-mode request to the console finished, so the session
/// ends for that reason.
struct Conclude {
  /// @brief Whether the rest was asked for because the host is going to sleep
  /// (moves to Sleeping) rather than to end the session (moves to Leaving).
  bool for_sleep{};
};

/// @brief The transition table of a session's lifecycle.
///
/// Two orthogonal regions run side by side. The run region goes Created,
/// Running, Finished; a Finish repeated after Finished is ignored. The exit
/// region starts at Staying and records why the session will end: Cancel
/// moves to Leaving, Sleep to Sleeping, and Conclude to either according to
/// Conclude::for_sleep. Once Leaving, only a Conclude for sleep moves on; once
/// Sleeping, nothing does. Stateless; owned by the Boost.SML machine in
/// SessionLifecycle.
struct Machine {
  /// @brief Builds the transition table for Boost.SML.
  ///
  /// @return The table of both regions.
  auto operator()() const noexcept {
    using namespace boost::sml;
    const auto for_sleep = [](const Conclude& event) {
      return event.for_sleep;
    };
    const auto unchanged = [] {};
    return make_transition_table(
        *state<Created> + event<Begin> = state<Running>,
        state<Created> + event<Finish> = state<Finished>,
        state<Running> + event<Finish> = state<Finished>,
        state<Finished> + event<Finish> / unchanged,

        *state<Staying> + event<Cancel> = state<Leaving>,
        state<Staying> + event<Sleep> = state<Sleeping>,
        state<Staying> + event<Conclude>[for_sleep] = state<Sleeping>,
        state<Staying> + event<Conclude>[!for_sleep] = state<Leaving>,
        state<Leaving> + event<Cancel> / unchanged,
        state<Leaving> + event<Conclude>[for_sleep] = state<Sleeping>,
        state<Leaving> + event<Conclude>[!for_sleep] / unchanged,
        state<Sleeping> + event<Cancel> / unchanged,
        state<Sleeping> + event<Conclude> / unchanged);
  }
};

}  // namespace libchiaking::play::session_lifecycle

namespace libchiaking::play {

/// @brief Where a live session stands and why it is going to end.
///
/// A plain machine with one owner, which serialises every call;
/// LockedSessionLifecycle shares one between a session's run loop and the
/// frontend threads that read it and cancel or park the session. Events that
/// do not apply in the current state are ignored, so calls never fail.
class SessionLifecycle {
 public:
  /// @enum Phase
  /// @brief The run state, as seen from outside the machine.
  ///
  /// @var Phase created
  /// @brief The session exists but its stream has not started.
  ///
  /// @var Phase running
  /// @brief The stream started and the session loop runs.
  ///
  /// @var Phase finished
  /// @brief The session ended; it never runs again.
  enum class Phase : std::uint8_t { created, running, finished };

  /// @brief Makes a lifecycle in the created and staying states.
  SessionLifecycle() = default;

  /// @brief Reads the run state.
  ///
  /// @return Phase::created until the stream started, Phase::running while it
  ///     runs, Phase::finished once the session ended.
  auto phase() const noexcept -> Phase {
    if (machine_.is(boost::sml::state<session_lifecycle::Running>))
      return Phase::running;
    if (machine_.is(boost::sml::state<session_lifecycle::Finished>))
      return Phase::finished;
    return Phase::created;
  }

  /// @brief Reports whether the session has been asked to end for any reason.
  ///
  /// @return `true` once the exit region left Staying, whether the player is
  ///     leaving or the host is going to sleep.
  auto cancelled() const noexcept -> bool {
    return !machine_.is(boost::sml::state<session_lifecycle::Staying>);
  }

  /// @brief Reports whether the session is being parked for a host sleep.
  ///
  /// @return `true` once a Sleep, or a Conclude for sleep, was processed.
  auto sleeping() const noexcept -> bool {
    return machine_.is(boost::sml::state<session_lifecycle::Sleeping>);
  }

  /// @brief Reports whether the session may take commands for the console.
  ///
  /// @return `true` only while the stream runs and nobody asked it to end.
  auto accepting() const noexcept -> bool {
    return machine_.is(boost::sml::state<session_lifecycle::Running>) &&
           machine_.is(boost::sml::state<session_lifecycle::Staying>);
  }

  /// @brief Records that the stream started.
  ///
  /// @return `true` when the session moved from created to running; `false`
  ///     when it had already started or ended.
  auto begin() noexcept -> bool {
    return machine_.process_event(session_lifecycle::Begin{});
  }

  /// @brief Records that the session ended; repeating it is harmless.
  void finish() noexcept {
    machine_.process_event(session_lifecycle::Finish{});
  }

  /// @brief Records that the player asked to leave; ignored when the session is
  /// already leaving or parked for sleep.
  void cancel() noexcept {
    machine_.process_event(session_lifecycle::Cancel{});
  }

  /// @brief Parks the session for a host sleep while the stream runs.
  ///
  /// @return `true` when the session moved to sleeping; `false` when the stream
  ///     is not running or the session was already asked to end, in which case
  ///     the caller cancels it instead.
  auto sleep() noexcept -> bool {
    if (!machine_.is(boost::sml::state<session_lifecycle::Running>))
      return false;
    return machine_.process_event(session_lifecycle::Sleep{});
  }

  /// @brief Records that a rest-mode request to the console finished.
  ///
  /// @param for_sleep Whether the rest was asked for because the host is going
  ///     to sleep; then the session is parked, otherwise it is leaving.
  void conclude(bool for_sleep) noexcept {
    machine_.process_event(session_lifecycle::Conclude{for_sleep});
  }

 private:
  /// @brief The Boost.SML machine holding both regions.
  boost::sml::sm<session_lifecycle::Machine> machine_;
};

}  // namespace libchiaking::play
