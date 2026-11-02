/// @file
/// @brief Declares LoginPin::Core, which carries the player's login code to
/// the console and lets the frontend wait for the outcome.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/access_code.hpp"
#include "libchiaking/detail/play/login_pin.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/play/adapters/session/command_route.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace libchiaking::play {

/// @brief The implementation behind the public LoginPin: one queued login
/// code and threads waiting for it to be sent.
///
/// A newer submission replaces an older one that was not sent, and its ticket
/// replaces the older ticket. The session thread sends the code with
/// LoginPin::Core::flush. Every member locks one mutex; waiters sleep on a
/// condition variable. The destructor wakes the waiters and blocks until they
/// have left. Owned by ConsoleCommands; the route must outlive it.
class LoginPin::Core {
 public:
  /// @brief Wakes every waiter and blocks until each has returned.
  ~Core();
  /// @brief Not copyable; waiters refer to this object.
  Core(const Core&) = delete;
  /// @brief Not copy-assignable; waiters refer to this object.
  auto operator=(const Core&) -> Core& = delete;

  /// @brief Queues a login code and wakes the session thread.
  ///
  /// @param code The code.
  /// @return The ticket of this submission; InputRefusal::not_running when the
  ///     session does not accept commands.
  auto submit(AccessCode code) -> Expected<Ticket, InputRefusal>;
  /// @brief Reads where a submission stands.
  ///
  /// @param ticket A ticket from LoginPin::Core::submit.
  /// @return Progress::accepted once sent; Progress::pending while queued and
  ///     the session runs; Progress::rejected for an older ticket, a failed or
  ///     abandoned submission, or when the session stopped.
  auto progress(Ticket ticket) const -> Progress;
  /// @brief Blocks until the latest submission is sent, fails, the session
  /// stops or the time runs out.
  ///
  /// A submission still queued when the wait ends is abandoned.
  ///
  /// @param timeout The longest wait.
  /// @return Verdict::accepted when sent, or when nothing was submitted;
  ///     Verdict::rejected otherwise.
  auto await(std::chrono::milliseconds timeout = default_await) -> Verdict;
  /// @brief Reports whether a code waits to be sent.
  ///
  /// @return `true` while a submission is queued.
  auto queued() const -> bool;

 private:
  friend class ConsoleCommands;

  /// @enum State
  /// @brief Where the latest submission stands.
  ///
  /// @var State idle
  /// @brief Nothing was submitted.
  ///
  /// @var State queued
  /// @brief A code waits to be sent.
  ///
  /// @var State accepted
  /// @brief The code was sent.
  ///
  /// @var State failed
  /// @brief Sending failed or the wait gave up.
  enum class State : std::uint8_t { idle, queued, accepted, failed };

  /// @brief Builds the prompt with nothing queued.
  ///
  /// @param threading Provider of the prompt's mutex and condition
  ///     variable; must outlive the prompt.
  /// @param clock Monotonic clock LoginPin::Core::await times out on; must
  ///     outlive the prompt.
  /// @param route Where the code goes; must outlive the prompt.
  Core(platform::ThreadingProvider& threading,
       const platform::ClockProvider& clock, const CommandRoute& route);

  /// @brief Sends the queued code; called on the session thread.
  ///
  /// The code is sent outside the lock; a submission that replaced it in the
  /// meantime stays queued.
  ///
  /// @return Nothing when there was nothing to send or it was sent; the link's
  ///     error otherwise, which is also logged.
  auto flush() -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Wakes the waiters so they look at the session again.
  void interrupt() noexcept;
  /// @brief Settles the queued submission and wakes the waiters; call with the
  /// mutex held.
  ///
  /// @param accepted Whether the code was sent.
  void resolve(bool accepted) noexcept;

  /// @brief Where the code goes.
  const CommandRoute& route_;
  /// @brief Guards the fields below.
  mutable platform::Mutex mutex_;
  /// @brief Signalled when the submission settles, the session stops or the
  /// prompt closes.
  platform::ConditionVariable changed_;
  /// @brief Where the latest submission stands.
  State state_{State::idle};
  /// @brief The queued code; cleared once settled.
  std::optional<AccessCode> code_;
  /// @brief The id of the latest ticket, counting from 1.
  std::uint64_t generation_{};
  /// @brief Submissions not yet awaited.
  unsigned submissions_{};
  /// @brief Threads waiting in LoginPin::Core::await.
  unsigned awaiting_{};
  /// @brief Set by the destructor; ends every wait.
  bool closed_{};
};

}  // namespace libchiaking::play
