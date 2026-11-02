#include "src/core/play/adapters/session/command_route.hpp"

#include "src/core/play/adapters/links/link_driver.hpp"
#include "src/core/play/adapters/session/locked_session_lifecycle.hpp"

#include <magic_enum/magic_enum.hpp>

#include <string>

namespace libchiaking::play {

CommandRoute::CommandRoute(LinkDriver& driver,
                           const LockedSessionLifecycle& lifecycle,
                           platform::DiagnosticsProvider& log,
                           std::optional<Venue> venue) noexcept
    : driver_(driver), lifecycle_(lifecycle), log_(log), venue_(venue) {}

auto CommandRoute::accepting() const noexcept -> bool {
  return lifecycle_.accepting();
}

auto CommandRoute::console() const noexcept -> bool {
  return !console_refusal();
}

auto CommandRoute::refusal() const noexcept -> std::optional<InputRefusal> {
  if (!lifecycle_.accepting()) return InputRefusal::not_running;
  return std::nullopt;
}

auto CommandRoute::console_refusal() const noexcept
    -> std::optional<InputRefusal> {
  if (!lifecycle_.accepting()) return InputRefusal::not_running;
  if (!venue_ || !venue_->on_console()) return InputRefusal::wrong_route;
  return std::nullopt;
}

auto CommandRoute::microphone_refusal() const noexcept
    -> std::optional<InputRefusal> {
  if (!lifecycle_.accepting()) return InputRefusal::not_running;
  if (!venue_) return InputRefusal::wrong_route;
  return std::nullopt;
}

auto CommandRoute::audio_3d_refusal() const noexcept
    -> std::optional<InputRefusal> {
  if (!venue_ || venue_->on_old_service()) return InputRefusal::wrong_route;
  return std::nullopt;
}

auto CommandRoute::cloud_refusal() const noexcept
    -> std::optional<InputRefusal> {
  if (venue_ != Venue::cloud(catalog::ServiceGeneration::ps5_cloud))
    return InputRefusal::wrong_route;
  return std::nullopt;
}

auto CommandRoute::invitation_refusal() const noexcept
    -> std::optional<InputRefusal> {
  if (!venue_ || venue_->on_console()) return InputRefusal::wrong_route;
  return std::nullopt;
}

auto CommandRoute::old_service_refusal() const noexcept
    -> std::optional<InputRefusal> {
  if (!venue_ || !venue_->on_old_service()) return InputRefusal::wrong_route;
  return std::nullopt;
}

auto CommandRoute::name() const noexcept -> std::string_view {
  return venue_ ? venue_->name() : std::string_view{"unknown"};
}

void CommandRoute::wake() const noexcept { driver_.wake(); }

void CommandRoute::log(platform::LogSeverity severity,
                       std::string_view message) const noexcept {
  log_.write({severity, message});
}

void CommandRoute::report(
    std::string_view what,
    const Expected<void, net::protocol::remote_play::StreamError>& result)
    const noexcept {
  try {
    std::string line{what};
    line += ": result=";
    line += result ? std::string_view{"ok"} : result_name(result.error());
    log(result ? platform::LogSeverity::info : platform::LogSeverity::warning,
        line);
  } catch (...) {
  }
}

auto CommandRoute::result_name(
    net::protocol::remote_play::StreamError error) noexcept
    -> std::string_view {
  return magic_enum::enum_name(error);
}

}  // namespace libchiaking::play
