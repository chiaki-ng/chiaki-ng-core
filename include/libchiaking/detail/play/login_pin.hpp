/// @file
/// @brief Declares LoginPin, through which a frontend answers a console's
/// request for a login PIN.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/access_code.hpp"
#include "libchiaking/detail/play/input_refusal.hpp"

#include <chrono>
#include <cstdint>

namespace libchiaking::play {

/// @brief Submits the login PIN a console asks for when a session starts.
///
/// A console profile can be protected by a four-digit PIN. When the console
/// asks for it the session calls SessionEvents::on_login_pin_needed, and the
/// frontend answers through this object, obtained from Session::login_pin. The
/// session stays in SessionState::connecting until the console is satisfied.
///
/// @see SessionEvents::on_login_pin_needed
class LoginPin {
 public:
  /// @brief Implementation behind LoginPin; not part of the public surface.
  class Core;

  /// @brief Time LoginPin::await waits unless told otherwise.
  static constexpr std::chrono::seconds default_await{10};

  /// @brief Says what became of the submission LoginPin::await waited for.
  enum class Verdict : std::uint8_t {
    /// @brief The code was sent to the console.
    accepted,
    /// @brief The code was not sent: sending failed, the wait ran out, or
    /// the session stopped taking input.
    rejected,
  };
  /// @brief Says where a submission stands.
  enum class Progress : std::uint8_t {
    /// @brief The code is queued and not sent yet.
    pending,
    /// @brief The code was sent to the console.
    accepted,
    /// @brief The code will not be sent: a later submission replaced it,
    /// sending failed, or the session stopped taking input.
    rejected,
  };

  /// @brief Identifies one submission, so its progress can be asked for
  /// later.
  class Ticket {
   public:
    /// @brief Wraps a submission number; tickets normally come from
    /// LoginPin::submit.
    ///
    /// @param id Number of the submission.
    explicit constexpr Ticket(std::uint64_t id) noexcept : id_(id) {}

    /// @brief Returns the submission number; later submissions have higher
    /// numbers.
    ///
    /// @return The submission number; later submissions have higher
    ///     numbers.
    constexpr auto id() const noexcept -> std::uint64_t { return id_; }

    /// @brief Compares the submission numbers.
    friend constexpr auto operator==(const Ticket&, const Ticket&) noexcept
        -> bool = default;

   private:
    std::uint64_t id_;
  };

  /// @brief Wraps a session's PIN handling; called by the library when a
  /// session is created.
  ///
  /// @param core Implementation to expose. Borrowed; must outlive this
  ///     object.
  explicit LoginPin(Core& core) noexcept : core_(&core) {}
  /// @brief Not copyable; the session holds the one LoginPin.
  LoginPin(const LoginPin&) = delete;
  /// @brief Not copy-assignable; the session holds the one LoginPin.
  auto operator=(const LoginPin&) -> LoginPin& = delete;

  /// @brief Queues a PIN to be sent to the console.
  ///
  /// The call returns at once; the session's thread sends the code. A code
  /// that is still queued is replaced by a newer one, and its ticket then
  /// reads Progress::rejected. The code is held in wiping storage and erased
  /// as soon as it has been sent or given up on.
  ///
  /// @param code The four digits the player entered.
  /// @return A ticket for LoginPin::progress.
  /// @retval InputRefusal::not_running The session is leaving or has ended.
  /// @note Sending the code is not the same as the console accepting it. A
  ///     wrong PIN makes the console ask again:
  ///     SessionEvents::on_login_pin_needed is called once more with its
  ///     argument set.
  [[nodiscard]] auto submit(AccessCode code) -> Expected<Ticket, InputRefusal>;
  /// @brief Reports where a submission stands, without waiting.
  ///
  /// @param ticket Ticket returned by LoginPin::submit.
  /// @return The progress of that submission. A ticket that is not the latest
  ///     submission always reads Progress::rejected.
  auto progress(Ticket ticket) const -> Progress;
  /// @brief Waits until the latest submission has been sent or given up on.
  ///
  /// The call blocks the calling thread until the code has been sent, sending
  /// has failed, the session stops taking input, or the timeout passes. When
  /// the timeout passes while the code is still queued, the submission is
  /// abandoned and will not be sent later.
  ///
  /// @param timeout Longest time to wait.
  /// @return Verdict::accepted when the code was sent; Verdict::rejected
  ///     otherwise.
  /// @note With no submission outstanding the call returns Verdict::accepted
  ///     immediately.
  auto await(std::chrono::milliseconds timeout = default_await) -> Verdict;

 private:
  Core* core_;
};

}  // namespace libchiaking::play
