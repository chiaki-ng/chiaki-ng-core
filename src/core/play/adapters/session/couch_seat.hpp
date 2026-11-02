/// @file
/// @brief Declares CouchSeat, one pad's seat on the console's side of the
/// couch protocol.
#pragma once

#include "src/core/play/adapters/session/seats_core.hpp"
#include "src/core/play/model/seat_lifecycle.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace libchiaking::play {

/// @brief One pad's seat on the console: where its join stands, when a
/// requested join times out, how many passcodes it tried and the account it
/// joins as.
///
/// A deadline is kept only while a join is requested. A seat is emptied by
/// replacing it with a fresh one. Owned by the console half of the couch
/// protocol and used on its thread; it is not synchronised.
class CouchSeat {
 public:
  /// @brief Where the seat stands, as seen from outside.
  ///
  /// @return The state.
  auto state() const noexcept -> Seats::Core::ConsoleSeatState {
    if (lifecycle_.join_requested())
      return Seats::Core::ConsoleSeatState::join_requested;
    if (lifecycle_.entering_passcode())
      return Seats::Core::ConsoleSeatState::entering_passcode;
    if (lifecycle_.joined()) return Seats::Core::ConsoleSeatState::joined;
    return Seats::Core::ConsoleSeatState::empty;
  }

  /// @brief Reports whether the seat waits for the console.
  ///
  /// @return `true` for a requested join or a passcode demand.
  auto awaiting_console() const noexcept -> bool {
    return state() == Seats::Core::ConsoleSeatState::join_requested ||
           state() == Seats::Core::ConsoleSeatState::entering_passcode;
  }

  /// @brief Reports whether a requested join has waited too long.
  ///
  /// @param now_us The monotonic time, in microseconds.
  /// @return `true` for a requested join past its deadline.
  auto timed_out(std::uint64_t now_us) const noexcept -> bool {
    return state() == Seats::Core::ConsoleSeatState::join_requested &&
           deadline_us_ && now_us >= *deadline_us_;
  }

  /// @brief Reports whether the seat used its passcode attempts.
  ///
  /// @return `true` at Seats::Core::passcode_attempt_limit.
  auto attempts_used() const noexcept -> bool {
    return attempts_ >= Seats::Core::passcode_attempt_limit;
  }

  /// @brief Reports whether a passcode was already tried for this join.
  ///
  /// @return `true` after the first attempt.
  auto retrying() const noexcept -> bool { return attempts_ > 0; }

  /// @brief Reads the account the pad joins as.
  ///
  /// @return The base64 account; empty when the join named none.
  auto account() const noexcept -> const std::string& { return account_; }

  /// @brief Marks the join requested.
  ///
  /// @param deadline_us When it times out; empty for never.
  void request(std::optional<std::uint64_t> deadline_us) noexcept {
    lifecycle_.request_join();
    deadline_us_ = deadline_us;
  }

  /// @brief Marks a fresh join requested as an account, with no passcode tried.
  ///
  /// @param account The base64 account.
  /// @param deadline_us When it times out.
  void request_as(const std::string& account, std::uint64_t deadline_us) {
    attempts_ = 0;
    account_ = account;
    request(deadline_us);
  }

  /// @brief Counts a passcode sent and marks the join requested again.
  ///
  /// @param deadline_us When it times out.
  void retry(std::uint64_t deadline_us) noexcept {
    ++attempts_;
    request(deadline_us);
  }

  /// @brief Records that the console asked for a passcode; the join stops
  /// timing out while the player answers.
  void ask_passcode() noexcept {
    lifecycle_.ask_passcode();
    deadline_us_.reset();
  }

  /// @brief Records that the pad was admitted.
  void admit() noexcept {
    lifecycle_.admit();
    deadline_us_.reset();
  }

 private:
  /// @brief Where the seat stands.
  SeatLifecycle lifecycle_;
  /// @brief When a requested join times out, in monotonic microseconds; empty
  /// when it does not.
  std::optional<std::uint64_t> deadline_us_;
  /// @brief Passcodes tried for this join.
  std::uint8_t attempts_{};
  /// @brief The base64 PSN account the pad joins as.
  std::string account_;
};

}  // namespace libchiaking::play
