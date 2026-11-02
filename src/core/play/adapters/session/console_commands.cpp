#include "src/core/play/adapters/session/console_commands.hpp"

#include "src/core/net/transport/http/url.hpp"
#include "src/core/play/adapters/links/link_driver.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace libchiaking::play {

ConsoleCommands::ConsoleCommands(platform::ThreadingProvider& threading,
                                 const CommandRoute& route,
                                 const LockedSessionLifecycle& lifecycle,
                                 platform::ClockProvider& clock,
                                 Seats::Core& seats, CommandSettings settings)
    : route_(route),
      clock_(clock),
      controller_speaker_(settings.controller_speaker),
      rest_(threading, route, lifecycle, clock, settings.rest_flush),
      pin_(threading, clock, route),
      seats_(seats),
      keyboard_(threading, route),
      microphone_(threading, route, rest_, settings.microphone_depth),
      accessibility_mutex_(threading),
      invitation_mutex_(threading) {}

auto ConsoleCommands::go_home() -> Expected<void, InputRefusal> {
  if (const auto refusal = route_.console_refusal())
    return unexpected(*refusal);
  if (!rest_.idle()) return unexpected(InputRefusal::not_ready);
  go_home_pending_.store(true, std::memory_order_release);
  route_.wake();
  return {};
}

auto ConsoleCommands::set_audio_3d(bool enabled)
    -> Expected<void, InputRefusal> {
  if (const auto refusal = route_.audio_3d_refusal())
    return unexpected(*refusal);
  if (audio_3d_.exchange(enabled, std::memory_order_acq_rel) == enabled)
    return {};
  if (!route_.accepting()) return {};
  audio_3d_pending_.store(true, std::memory_order_release);
  route_.wake();
  return {};
}

auto ConsoleCommands::set_system_ui(Session::SystemUi state)
    -> Expected<void, InputRefusal> {
  if (const auto refusal = route_.cloud_refusal()) return unexpected(*refusal);
  if (system_ui_.exchange(state, std::memory_order_acq_rel) == state) return {};
  system_ui_pending_.store(true, std::memory_order_release);
  route_.wake();
  return {};
}

auto ConsoleCommands::set_accessibility(
    const AccessibilityRequest& accessibility) -> Expected<void, InputRefusal> {
  if (const auto refusal = route_.cloud_refusal()) return unexpected(*refusal);
  {
    const platform::Lock lock(accessibility_mutex_);
    if (accessibility_sent_ == accessibility) return {};
    accessibility_sent_ = accessibility;
    accessibility_wanted_ = accessibility;
  }
  accessibility_pending_.store(true, std::memory_order_release);
  route_.wake();
  return {};
}

auto ConsoleCommands::accessibility_deep_link(
    const AccessibilityRequest& accessibility) -> std::string {
  const auto flag = [](bool on) { return on ? "1" : "0"; };
  net::http::Query link{accessibility_action};
  link.add("tts", flag(accessibility.screen_reader()))
      .add("ttsspeed", std::to_string(accessibility.screen_reader_speed()))
      .add("ttsvolume", std::to_string(accessibility.screen_reader_volume()))
      .add("textsize", flag(accessibility.text_size() > 0))
      .add("boldtext", flag(accessibility.bold_text()))
      .add("contrast", flag(accessibility.higher_contrast()));
  return std::string{link.view()};
}

void ConsoleCommands::flush_accessibility() {
  if (!accessibility_pending_.exchange(false, std::memory_order_acq_rel))
    return;
  std::optional<AccessibilityRequest> wanted;
  {
    const platform::Lock lock(accessibility_mutex_);
    wanted.swap(accessibility_wanted_);
  }
  if (!wanted) return;
  const auto sent =
      route_.driver().send_deep_link(accessibility_deep_link(*wanted));
  route_.report("accessibility update", sent);
  if (sent) return;
  const platform::Lock lock(accessibility_mutex_);
  if (accessibility_sent_ == wanted) accessibility_sent_.reset();
}

auto ConsoleCommands::capture_press(Session::CreatePress press) noexcept
    -> CloudMessaging::CapturePress {
  switch (press) {
    case Session::CreatePress::single_tap:
      return CloudMessaging::CapturePress::single_tap;
    case Session::CreatePress::double_tap:
      return CloudMessaging::CapturePress::double_tap;
    case Session::CreatePress::long_press:
      return CloudMessaging::CapturePress::long_press;
  }
  return CloudMessaging::CapturePress::single_tap;
}

auto ConsoleCommands::join_invitation(std::string invitation_id,
                                      std::string session_id)
    -> Expected<void, InputRefusal> {
  if (const auto refusal = route_.invitation_refusal())
    return unexpected(*refusal);
  if (!route_.accepting()) return unexpected(InputRefusal::not_running);
  {
    const platform::Lock lock(invitation_mutex_);
    invitation_wanted_.emplace(std::move(invitation_id), std::move(session_id));
  }
  invitation_pending_.store(true, std::memory_order_release);
  route_.wake();
  return {};
}

auto ConsoleCommands::open_saved_data() -> Expected<void, InputRefusal> {
  if (const auto refusal = route_.old_service_refusal())
    return unexpected(*refusal);
  if (!route_.accepting()) return unexpected(InputRefusal::not_running);
  saved_data_pending_.store(true, std::memory_order_release);
  route_.wake();
  return {};
}

auto ConsoleCommands::press_create(Session::CreatePress press)
    -> Expected<void, InputRefusal> {
  if (const auto refusal = route_.cloud_refusal()) return unexpected(*refusal);
  if (!route_.accepting()) return unexpected(InputRefusal::not_running);
  create_pending_.store(
      static_cast<std::uint8_t>(static_cast<std::uint8_t>(press) + 1U),
      std::memory_order_release);
  route_.wake();
  return {};
}

auto ConsoleCommands::system_service_status_link(std::string_view name,
                                                 std::string_view value)
    -> std::string {
  net::http::Query link{system_service_status};
  link.add(name, value);
  return std::string{link.view()};
}

void ConsoleCommands::send_system_ui_state() {
  const auto state = system_ui_.load(std::memory_order_acquire);
  const bool overlaid = state == Session::SystemUi::overlaid;
  const bool hidden = state == Session::SystemUi::background;
  const auto covered =
      system_service_status_link("isSystemUiOverlaid", overlaid ? "1" : "0");
  const auto background =
      system_service_status_link("isInBackgroundExecution", hidden ? "1" : "0");
  auto& driver = route_.driver();
  if (overlaid) {
    route_.report("system UI overlaid", driver.send_deep_link(covered));
    route_.report("system UI foreground", driver.send_deep_link(background));
    return;
  }
  if (hidden) {
    route_.report("system UI uncovered", driver.send_deep_link(covered));
    route_.report("system UI background", driver.send_deep_link(background));
    return;
  }
  route_.report("system UI foreground", driver.send_deep_link(background));
  route_.report("system UI uncovered", driver.send_deep_link(covered));
}

auto ConsoleCommands::send_keyboard(const KeyboardState& state)
    -> Expected<void, InputRefusal> {
  if (!route_.accepting()) return unexpected(InputRefusal::not_running);
  if (const auto sent = route_.driver().send_keyboard(state); !sent)
    route_.report("keyboard", sent);
  return {};
}

auto ConsoleCommands::audio_3d() const noexcept -> bool {
  return audio_3d_.load(std::memory_order_acquire);
}

auto ConsoleCommands::audio_3d_due() const noexcept -> bool {
  return audio_3d_pending_.load(std::memory_order_acquire) &&
         (!audio_configured_.load(std::memory_order_acquire) ||
          audio_3d_pacing_.remaining(clock_.now_microseconds()).count() == 0);
}

void ConsoleCommands::send_audio_3d(bool enabled, std::string_view reason) {
  audio_3d_pacing_.sent(clock_.now_microseconds());
  if (route_.venue() != Venue::cloud(catalog::ServiceGeneration::ps5_cloud)) {
    const std::array<std::byte, 1> profile{
        std::byte{enabled ? audio_3d_profile : stereo_profile}};
    const auto result = route_.driver().send_audio_state(
        net::protocol::remote_play::AudioStateKind::channel_count, profile);
    route_.report(std::string{"audio 3d profile "} +
                      std::to_string(std::to_integer<unsigned>(profile[0])) +
                      " (" + std::string{reason} + ")",
                  result);
    return;
  }
  const auto body = net::protocol::remote_play::audio_state_flags(
      audio_3d_selector, audio_3d_available, enabled ? 1U : 0U);
  const auto result = route_.driver().send_audio_state(
      net::protocol::remote_play::AudioStateKind::flags, body);
  route_.report(std::string{"audio 3d "} +
                    (enabled ? "requested" : "released") + " (" +
                    std::string{reason} + ")",
                result);
}

void ConsoleCommands::observe(
    const net::protocol::remote_play::StreamEvent& event) {
  if (const auto* opened =
          std::get_if<net::protocol::remote_play::event::KeyboardOpen>(
              &event)) {
    keyboard_.opened(opened->text.size());
  } else if (const auto* changed = std::get_if<
                 net::protocol::remote_play::event::KeyboardTextChange>(
                 &event)) {
    keyboard_.text_changed(changed->text.size());
  } else if (std::holds_alternative<
                 net::protocol::remote_play::event::KeyboardRemoteClose>(
                 event)) {
    keyboard_.closed_by_console();
  } else if (std::holds_alternative<
                 net::protocol::remote_play::event::StreamInfoReady>(event)) {
    audio_configured_.store(true, std::memory_order_release);
    audio_3d_pending_.store(false, std::memory_order_release);
    if (audio_3d()) send_audio_3d(true, "stream ready");
    if (route_.venue() == Venue::cloud(catalog::ServiceGeneration::ps5_cloud)) {
      report_audio_routes(true, "stream ready");
      audio_routes_.started(clock_.now_microseconds());
    }
    if (!route_.old_service_refusal())
      route_.report("system service status",
                    route_.driver().send_deep_link(
                        system_service_status_link("spartacus", "1")));
  }
}

void ConsoleCommands::report_audio_routes(bool first, std::string_view reason) {
  using net::protocol::remote_play::AudioPortGroup;
  using net::protocol::remote_play::AudioPortOutput;
  using net::protocol::remote_play::AudioStateKind;
  const auto label = [reason](std::string_view what) {
    return std::string{what} + " (" + std::string{reason} + ")";
  };
  if (controller_speaker_) {
    if (first)
      route_.report(label("audio layout"),
                    route_.driver().send_audio_state(
                        AudioStateKind::full,
                        net::protocol::remote_play::audio_state_full_layout()));
    const auto sent = route_.driver().send_audio_state(
        AudioStateKind::port_states,
        net::protocol::remote_play::audio_state_port_states(
            AudioPortGroup::controller, 0, AudioPortOutput::controller_speaker,
            net::protocol::remote_play::audio_port_full_volume));
    if (first || !sent) route_.report(label("controller speaker route"), sent);
  }
  const auto sent = route_.driver().send_audio_state(
      AudioStateKind::port_states,
      net::protocol::remote_play::audio_state_port_states(
          AudioPortGroup::main, 0, AudioPortOutput::main,
          net::protocol::remote_play::audio_port_full_volume));
  if (first || !sent) route_.report(label("main output route"), sent);
}

void ConsoleCommands::audio_profile_changed() {
  if (audio_3d()) send_audio_3d(true, "profile switch");
}

auto ConsoleCommands::pending(bool streaming) const -> bool {
  if (seats_.passcode_pending() || rest_.queued()) return true;
  if (!streaming) return false;
  return seats_.queued() != 0 ||
         go_home_pending_.load(std::memory_order_acquire) ||
         microphone_.pending() || keyboard_.pending() || audio_3d_due() ||
         audio_routes_.due(clock_.now_microseconds()) ||
         system_ui_pending_.load(std::memory_order_acquire) ||
         accessibility_pending_.load(std::memory_order_acquire) ||
         create_pending_.load(std::memory_order_acquire) != 0;
}

auto ConsoleCommands::service(net::protocol::remote_play::StreamState state,
                              std::chrono::milliseconds wait)
    -> CommandService {
  CommandService service;
  static_cast<void>(pin_.flush());
  static_cast<void>(seats_.flush_passcode());
  if (state == net::protocol::remote_play::StreamState::streaming) {
    service.refusals = seats_.flush();
    if (go_home_pending_.exchange(false, std::memory_order_acq_rel))
      route_.report("Console home screen request",
                    route_.driver().execute(
                        net::protocol::remote_play::command::GoHome{}));
    microphone_.flush();
    keyboard_.flush();
    if (system_ui_pending_.exchange(false, std::memory_order_acq_rel))
      send_system_ui_state();
    flush_accessibility();
    if (invitation_pending_.exchange(false, std::memory_order_acq_rel)) {
      std::optional<std::pair<std::string, std::string>> wanted;
      {
        const platform::Lock lock(invitation_mutex_);
        wanted.swap(invitation_wanted_);
      }
      if (wanted)
        route_.report("Game invitation",
                      route_.driver().send_invitation(
                          std::move(wanted->first), std::move(wanted->second)));
    }
    if (saved_data_pending_.exchange(false, std::memory_order_acq_rel))
      route_.report("Saved data management", route_.driver().open_saved_data());
    if (const auto queued =
            create_pending_.exchange(0, std::memory_order_acq_rel)) {
      route_.report("Create press",
                    route_.driver().send_create(capture_press(
                        static_cast<Session::CreatePress>(queued - 1U))));
    }
    if (audio_3d_due() &&
        audio_3d_pending_.exchange(false, std::memory_order_acq_rel) &&
        audio_configured_.load(std::memory_order_acquire))
      send_audio_3d(audio_3d(), "live toggle");
    if (audio_routes_.due(clock_.now_microseconds())) {
      report_audio_routes(false, "title start window");
      audio_routes_.repeated(clock_.now_microseconds());
    }
  }
  const auto step = rest_.advance(wait);
  service.rest_finished = step.finished;
  service.for_sleep = rest_.for_sleep();
  service.wait = step.wait;
  if (state == net::protocol::remote_play::StreamState::streaming &&
      audio_3d_pending_.load(std::memory_order_acquire)) {
    const auto held = audio_3d_pacing_.remaining(clock_.now_microseconds());
    if (held.count() > 0) service.wait = std::min(service.wait, held);
  }
  if (state == net::protocol::remote_play::StreamState::streaming &&
      audio_routes_.state() == AudioRouteAdvertisement::State::advertising)
    service.wait = std::min(
        service.wait, std::chrono::ceil<std::chrono::milliseconds>(
                          audio_routes_.remaining(clock_.now_microseconds())));
  return service;
}

void ConsoleCommands::interrupt() noexcept { pin_.interrupt(); }

void ConsoleCommands::conclude() noexcept {
  pin_.interrupt();
  seats_.discard();
}

void ConsoleCommands::forget_seats() noexcept { seats_.discard(); }

}  // namespace libchiaking::play
