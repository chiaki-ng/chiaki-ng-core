/// @file
/// @brief Declares LiveSession, the slot holding the one session a Play may
/// run at a time.
#pragma once

#include "libchiaking/detail/diagnostics/session_journal.hpp"
#include "libchiaking/detail/play/session.hpp"
#include "src/core/play/adapters/session/session_core.hpp"

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

namespace libchiaking::play {

/// @brief Holds the live session and serialises starts.
///
/// A Play runs one session at a time. A start holds
/// LiveSession::one_start_at_a_time for its whole length and puts the new
/// session here; an ended session is released the next time someone asks
/// whether the slot is busy. Also holds the journal new sessions write to,
/// and numbers the fresh journals it hands out. Every member locks a mutex,
/// so any thread may use it. Owned by Play::Core.
class LiveSession {
 public:
  /// @brief Builds an empty slot.
  ///
  /// @param threading Provider of the slot's mutexes and of the journals'
  ///     mutexes; must outlive the slot and every journal it hands out.
  /// @param clock Monotonic clock the journals time their entries on; must
  ///     outlive the slot and every journal it hands out.
  LiveSession(platform::ThreadingProvider& threading,
              const platform::ClockProvider& clock);
  /// @brief Not copyable; the slot is one per Play.
  LiveSession(const LiveSession&) = delete;
  /// @brief Not copy-assignable; the slot is one per Play.
  auto operator=(const LiveSession&) -> LiveSession& = delete;

  /// @brief Takes the start lock, blocking until any other start finishes.
  ///
  /// @return The held lock; the start keeps it until it returns.
  [[nodiscard]] auto one_start_at_a_time() -> platform::Lock;
  /// @brief Reports whether a session is live, releasing one that has ended.
  ///
  /// @return `true` when the held session has not ended, or ended but its run
  ///     thread is the caller.
  auto busy() -> bool;
  /// @brief Puts a started session in the slot, replacing what was there.
  ///
  /// @param session The session.
  /// @param core Its core, which the session owns.
  void hold(std::shared_ptr<Session> session, Session::Core& core);
  /// @brief Gives the live session.
  ///
  /// @return The session; null when the slot is empty or the session ended.
  auto current() const -> std::shared_ptr<Session>;
  /// @brief Reports whether a session is playing; blocks while a start is in
  /// progress.
  ///
  /// @return `true` when a session is held and has not ended.
  auto playing() -> bool;
  /// @brief Asks the live session to end when it is played by an account.
  ///
  /// @param account The account being forgotten.
  void leave_as(AccountId account);

  /// @brief Sets the journal later sessions write to.
  ///
  /// @param journal The journal; may be null.
  void journal_in(std::shared_ptr<diagnostics::SessionJournal> journal);
  /// @brief Gives the journal.
  ///
  /// @return The journal; null when none was set.
  auto journal() const -> std::shared_ptr<diagnostics::SessionJournal>;
  /// @brief Gives the journal a new session writes to.
  ///
  /// @return The journal set by LiveSession::journal_in; otherwise a fresh
  ///     one carrying the next attempt number, counted from 1 per slot.
  auto journal_for_session() -> std::shared_ptr<diagnostics::SessionJournal>;

 private:
  /// @brief The held session and its core.
  struct Held {
    /// @brief The session.
    std::shared_ptr<Session> session;
    /// @brief Its core; valid while LiveSession::Held::session is held.
    Session::Core* core{};
  };

  /// @brief Makes the mutexes of the fresh journals.
  platform::ThreadingProvider& threading_;
  /// @brief Times the entries of the fresh journals.
  const platform::ClockProvider& clock_;
  /// @brief The attempt number the last fresh journal carries.
  std::atomic<std::uint64_t> attempts_{};
  /// @brief Held by a start for its whole length.
  platform::Mutex starting_;
  /// @brief Guards the held session and the journal.
  mutable platform::Mutex mutex_;
  /// @brief The held session.
  Held held_;
  /// @brief The journal sessions write to.
  std::shared_ptr<diagnostics::SessionJournal> journal_;
};

}  // namespace libchiaking::play
