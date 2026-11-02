/// @file
/// @brief Declares ConsoleKeyboard::Core, which carries the player's answers
/// to the console's on-screen keyboard prompt to the session thread.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/console_keyboard.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/play/adapters/session/command_route.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace libchiaking::play {

/// @brief The implementation behind the public ConsoleKeyboard: whether the
/// console has a keyboard prompt open, and the text and answer waiting to be
/// sent.
///
/// The frontend sets text and accepts or rejects from its own thread; the
/// request is held and the session thread is woken to send it with the next
/// ConsoleKeyboard::Core::flush. Accepting or rejecting closes the prompt at
/// once; text set before it is sent first. Every member locks one mutex. Owned
/// by ConsoleCommands, which also tells it when the console opens or closes
/// the prompt; the route must outlive it.
class ConsoleKeyboard::Core {
 public:
  /// @brief Replaces the prompt's text.
  ///
  /// @param text The whole text, at most ConsoleKeyboard::text_limit bytes.
  /// @return Nothing when queued; InputRefusal::invalid for text too long;
  ///     InputRefusal::not_running or InputRefusal::wrong_route from the route;
  ///     InputRefusal::not_ready when no prompt is open or an answer is already
  ///     queued.
  auto set_text(std::string_view text) -> Expected<void, InputRefusal>;
  /// @brief Accepts the prompt, sending any queued text first.
  ///
  /// @return As ConsoleKeyboard::Core::close.
  auto accept() -> Expected<void, InputRefusal>;
  /// @brief Dismisses the prompt, sending any queued text first.
  ///
  /// @return As ConsoleKeyboard::Core::close.
  auto reject() -> Expected<void, InputRefusal>;
  /// @brief Reports whether the console has a prompt open.
  ///
  /// @return `true` from the console opening the prompt until it or the
  ///     player closes it.
  auto active() const -> bool;
  /// @brief Reports whether something waits to be sent, so the session loop
  /// does not sleep.
  ///
  /// @return `true` while text or an answer is queued.
  auto pending() const -> bool;

 private:
  friend class ConsoleCommands;

  /// @brief What the next flush sends.
  enum class Send : std::uint8_t {
    /// @brief Nothing.
    none,
    /// @brief The text.
    text,
    /// @brief Acceptance.
    accept,
    /// @brief Dismissal.
    reject,
    /// @brief The text, then acceptance; dismissal instead if the text could
    /// not
    /// be sent.
    text_accept,
    /// @brief The text, then dismissal.
    text_reject
  };

  /// @brief Builds the prompt with nothing open.
  ///
  /// @param threading Provider of the prompt's mutex; must outlive the
  ///     prompt.
  /// @param route Where commands go; must outlive the prompt.
  Core(platform::ThreadingProvider& threading, const CommandRoute& route);

  /// @brief Closes the prompt with an answer and wakes the session thread.
  ///
  /// @param accepted Whether the player accepted.
  /// @return Nothing when queued; InputRefusal::not_running or
  ///     InputRefusal::wrong_route from the route; InputRefusal::not_ready when
  ///     no prompt is open.
  auto close(bool accepted) -> Expected<void, InputRefusal>;
  /// @brief Records that the console opened a prompt and logs it.
  ///
  /// @param length The length of the console's starting text, in bytes.
  void opened(std::size_t length);
  /// @brief Logs that the console changed the text.
  ///
  /// @param length The new length, in bytes.
  void text_changed(std::size_t length);
  /// @brief Records that the console closed the prompt itself, dropping
  /// anything queued.
  void closed_by_console();
  /// @brief Sends what is queued; called on the session thread while
  /// streaming. A failure is logged, not returned.
  void flush();

  /// @brief Where commands go.
  const CommandRoute& route_;
  /// @brief Guards the fields below.
  mutable platform::Mutex mutex_;
  /// @brief Whether a prompt is open.
  bool active_{};
  /// @brief What the next flush sends.
  Send send_{Send::none};
  /// @brief The text to send; wiped when replaced or destroyed.
  SecretString text_;
};

}  // namespace libchiaking::play
