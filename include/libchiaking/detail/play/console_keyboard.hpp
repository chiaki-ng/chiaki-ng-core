/// @file
/// @brief Declares ConsoleKeyboard, the frontend's side of the console's
/// on-screen keyboard.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/input_refusal.hpp"

#include <cstddef>
#include <string_view>

namespace libchiaking::play {

/// @brief Answers the on-screen keyboard the console opens during remote play.
///
/// The console opens the keyboard and the session reports it through
/// SessionEvents::on_keyboard_opened. From then until the keyboard closes the
/// frontend may replace its text and then accept or dismiss it. Every call only
/// queues the request; it is sent by the session's own thread while the stream
/// is running, and a send failure is written to the session journal rather than
/// returned.
///
/// Obtained from Session::keyboard and valid for as long as that Session. All
/// members may be called from any thread. Cloud sessions have no console
/// keyboard and refuse every request with InputRefusal::wrong_route.
///
/// @see SessionEvents::on_keyboard_opened
/// @see SessionEvents::on_keyboard_closed
class ConsoleKeyboard {
 public:
  /// @brief Implementation behind the keyboard; owned by the session.
  class Core;

  /// @brief Largest text, in bytes, that set_text accepts.
  static constexpr std::size_t text_limit = 4096;

  /// @brief Binds the keyboard to the session's implementation.
  ///
  /// Frontends do not construct this; they use Session::keyboard.
  ///
  /// @param core Implementation owned by the session. It must outlive this
  ///     object.
  explicit ConsoleKeyboard(Core& core) noexcept : core_(&core) {}
  /// @brief Not copyable; a session has one keyboard.
  ConsoleKeyboard(const ConsoleKeyboard&) = delete;
  /// @brief Not copy-assignable; a session has one keyboard.
  auto operator=(const ConsoleKeyboard&) -> ConsoleKeyboard& = delete;

  /// @brief Replaces the text in the open keyboard.
  ///
  /// A later call made before the first one is sent replaces it, so only the
  /// newest text reaches the console. The text is held in memory that is
  /// wiped when it is released.
  ///
  /// @param text The complete new text, at most text_limit bytes.
  /// @return Nothing on success; the text is queued.
  /// @retval InputRefusal::invalid The text is longer than text_limit.
  /// @retval InputRefusal::not_running The session is not accepting requests.
  /// @retval InputRefusal::wrong_route The session is a cloud session.
  /// @retval InputRefusal::not_ready No keyboard is open, or it is already
  ///     being closed.
  [[nodiscard]] auto set_text(std::string_view text)
      -> Expected<void, InputRefusal>;
  /// @brief Confirms the keyboard's text and closes the keyboard.
  ///
  /// Any text queued by set_text is sent first. If that text cannot be sent
  /// the keyboard is dismissed instead of accepted. active returns false as
  /// soon as this call succeeds.
  ///
  /// @return Nothing on success; the request is queued.
  /// @retval InputRefusal::not_running The session is not accepting requests.
  /// @retval InputRefusal::wrong_route The session is a cloud session.
  /// @retval InputRefusal::not_ready No keyboard is open.
  [[nodiscard]] auto accept() -> Expected<void, InputRefusal>;
  /// @brief Dismisses the keyboard without confirming its text.
  ///
  /// active returns false as soon as this call succeeds.
  ///
  /// @return Nothing on success; the request is queued.
  /// @retval InputRefusal::not_running The session is not accepting requests.
  /// @retval InputRefusal::wrong_route The session is a cloud session.
  /// @retval InputRefusal::not_ready No keyboard is open.
  [[nodiscard]] auto reject() -> Expected<void, InputRefusal>;
  /// @brief Reports whether a keyboard is open and can still be answered.
  ///
  /// @return True from the moment the console opens the keyboard until the
  ///     console closes it or accept or reject succeeds.
  auto active() const -> bool;

 private:
  Core* core_;
};

}  // namespace libchiaking::play
