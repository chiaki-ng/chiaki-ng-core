/// @file
/// @brief Declares the reasons a running session refuses a request from the
/// frontend.
#pragma once

#include <cstdint>

namespace libchiaking::play {

/// @brief Why a session did not take a request made through one of its
/// controls.
///
/// Returned as the error of `Expected<void, InputRefusal>` by Controllers,
/// Seats, ConsoleKeyboard, Microphone, SharedSeat and Session::go_home. A
/// refusal is decided at once, on the calling thread; it says the request was
/// not queued, not that the console rejected it.
enum class InputRefusal : std::uint8_t {
  /// @brief The request itself is malformed, for example seat 0 named where
  /// only an added seat is allowed, unknown button bits, or text over the
  /// limit.
  invalid,
  /// @brief The session is not accepting requests: it has not started running
  /// yet, it is leaving, or it has ended.
  not_running,
  /// @brief The request needs a console and this session is a cloud session.
  wrong_route,
  /// @brief The target is occupied or the request queue is full.
  busy,
  /// @brief The seat is held by someone else, for example a shared guest.
  not_owner,
  /// @brief The session is running but not in a state that allows the
  /// request, for example no console keyboard is open or a rest mode request
  /// is under way.
  not_ready,
};

}  // namespace libchiaking::play
