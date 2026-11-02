#include "src/core/play/adapters/session/prompts/console_keyboard_core.hpp"

#include "src/core/play/adapters/links/link_driver.hpp"

#include <string>
#include <utility>

namespace libchiaking::play {

ConsoleKeyboard::Core::Core(platform::ThreadingProvider& threading,
                            const CommandRoute& route)
    : route_(route), mutex_(threading) {}

auto ConsoleKeyboard::Core::set_text(std::string_view text)
    -> Expected<void, InputRefusal> {
  if (text.size() > text_limit) return unexpected(InputRefusal::invalid);
  {
    const platform::Lock lock(mutex_);
    if (const auto refusal = route_.console_refusal())
      return unexpected(*refusal);
    if (!active_ || (send_ != Send::none && send_ != Send::text))
      return unexpected(InputRefusal::not_ready);
    text_.assign(text);
    send_ = Send::text;
  }
  route_.wake();
  return {};
}

auto ConsoleKeyboard::Core::close(bool accepted)
    -> Expected<void, InputRefusal> {
  {
    const platform::Lock lock(mutex_);
    if (const auto refusal = route_.console_refusal())
      return unexpected(*refusal);
    if (!active_) return unexpected(InputRefusal::not_ready);
    const bool text = send_ == Send::text;
    if (accepted)
      send_ = text ? Send::text_accept : Send::accept;
    else
      send_ = text ? Send::text_reject : Send::reject;
    active_ = false;
  }
  route_.wake();
  return {};
}

auto ConsoleKeyboard::Core::accept() -> Expected<void, InputRefusal> {
  return close(true);
}

auto ConsoleKeyboard::Core::reject() -> Expected<void, InputRefusal> {
  return close(false);
}

auto ConsoleKeyboard::Core::active() const -> bool {
  const platform::Lock lock(mutex_);
  return active_;
}

auto ConsoleKeyboard::Core::pending() const -> bool {
  const platform::Lock lock(mutex_);
  return send_ != Send::none;
}

void ConsoleKeyboard::Core::opened(std::size_t length) {
  {
    const platform::Lock lock(mutex_);
    active_ = true;
    send_ = Send::none;
  }
  route_.log(platform::LogSeverity::info,
             "Console keyboard opened: length=" + std::to_string(length));
}

void ConsoleKeyboard::Core::text_changed(std::size_t length) {
  route_.log(platform::LogSeverity::info,
             "Console keyboard text changed: length=" + std::to_string(length));
}

void ConsoleKeyboard::Core::closed_by_console() {
  {
    const platform::Lock lock(mutex_);
    active_ = false;
    send_ = Send::none;
  }
  route_.log(platform::LogSeverity::info,
             "Console keyboard closed by the console: length=0");
}

void ConsoleKeyboard::Core::flush() {
  Send send = Send::none;
  SecretString text;
  {
    const platform::Lock lock(mutex_);
    send = std::exchange(send_, Send::none);
    text = text_.clone();
  }
  if (send == Send::none) return;
  auto& driver = route_.driver();
  if (send == Send::text || send == Send::text_accept ||
      send == Send::text_reject) {
    const auto sent = driver.execute(
        net::protocol::remote_play::command::KeyboardSetText{text.clone()});
    if (!sent) {
      route_.log(platform::LogSeverity::warning,
                 "Console keyboard text was not sent: result=" +
                     std::string{CommandRoute::result_name(sent.error())});
      if (send == Send::text_accept) send = Send::reject;
    }
  }
  if (send == Send::accept || send == Send::text_accept)
    route_.report(
        "Console keyboard accepted: length=" + std::to_string(text.size()),
        driver.execute(net::protocol::remote_play::command::KeyboardAccept{}));
  else if (send == Send::reject || send == Send::text_reject)
    route_.report(
        "Console keyboard dismissed",
        driver.execute(net::protocol::remote_play::command::KeyboardReject{}));
}

auto ConsoleKeyboard::set_text(std::string_view text)
    -> Expected<void, InputRefusal> {
  return core_->set_text(text);
}

auto ConsoleKeyboard::accept() -> Expected<void, InputRefusal> {
  return core_->accept();
}

auto ConsoleKeyboard::reject() -> Expected<void, InputRefusal> {
  return core_->reject();
}

auto ConsoleKeyboard::active() const -> bool { return core_->active(); }

}  // namespace libchiaking::play
