#pragma once

#include "libchiaking/detail/expected.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/net/protocol/takion/source_tap.hpp"
#include "src/core/play/adapters/links/link_driver.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::testing {

class ScriptedSession final : public play::Link {
 public:
  using StreamError = libchiaking::net::protocol::remote_play::StreamError;
  using StreamState = libchiaking::net::protocol::remote_play::StreamState;
  using Step = std::function<void(ScriptedSession&)>;
  using Result = Expected<void, StreamError>;

  explicit ScriptedSession(libchiaking::platform::StopSource stop,
                           std::optional<libchiaking::play::Venue> route =
                               libchiaking::play::Venue::home())
      : Link(route), stop_(std::move(stop)) {}

  auto start() -> Result override {
    calls.emplace_back("start");
    if (stop_.stop_requested()) {
      ending().record_failure(play::LinkFailure::abandoned(
          libchiaking::net::protocol::remote_play::FailureStage::unknown,
          StreamError::route_failed));
      state_ = StreamState::failed;
      sink_->on_event(libchiaking::net::protocol::remote_play::event::Failed{
          libchiaking::net::protocol::remote_play::StreamFailure::cancelled});
      return unexpected(StreamError::route_failed);
    }
    if (!start_result) return start_result;
    set_state(start_state);
    return {};
  }

  auto pump_once() -> Result override {
    calls.emplace_back("pump");
    ++pumps;
    if (script.empty()) {
      if (stop_when_idle) state_ = StreamState::stopped;
      return {};
    }
    auto step = std::move(script.front());
    script.pop_front();
    step(*this);
    return std::exchange(pump_result, Result{});
  }

  auto wait(std::chrono::milliseconds timeout) -> Result override {
    calls.emplace_back("wait");
    waits.push_back(timeout);
    return wait_result;
  }

  void wake() noexcept override { ++wakes; }

  auto select_datacenter(std::string_view name) -> Result override {
    datacenter.assign(name);
    return {};
  }

  auto stop() -> Result override {
    calls.emplace_back("stop");
    ++stops;
    finish_stop();
    return stop_result;
  }

  auto stop_for_sleep() -> Result override {
    calls.emplace_back("stop_for_sleep");
    ++sleep_stops;
    finish_stop();
    return stop_result;
  }

  auto state() const noexcept -> StreamState override { return state_; }

  auto send_controller_state(
      const net::protocol::remote_play::ControllerState& state)
      -> Result override {
    calls.emplace_back("controller");
    controllers.push_back(state);
    if (controller_results.empty()) return {};
    auto result = controller_results.front();
    controller_results.pop_front();
    return result;
  }

  auto submit_pin(std::span<const std::byte> bytes) -> Result override {
    calls.emplace_back("pin");
    pin.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return pin_result;
  }

  auto report_video_loss(std::uint16_t first, std::uint16_t last)
      -> Result override {
    calls.emplace_back("loss");
    losses.emplace_back(first, last);
    return loss_result;
  }

  auto execute(
      const libchiaking::net::protocol::remote_play::StreamCommand& command)
      -> Result override {
    calls.emplace_back(std::visit(Name{}, command));
    commands.push_back(std::visit(Copy{}, command));
    if (refused_command && *refused_command == command.index())
      return unexpected(command_error);
    return {};
  }

  auto send_microphone(std::span<const std::byte> frame) -> Result override {
    calls.emplace_back("microphone_frame");
    microphone_frames.emplace_back(frame.begin(), frame.end());
    return microphone_result;
  }

  auto connect_microphone() -> Result override {
    calls.emplace_back("microphone_connect");
    return microphone_connect_result;
  }

  auto set_microphone_muted(bool muted) -> Result override {
    calls.emplace_back(muted ? "microphone_mute" : "microphone_unmute");
    return {};
  }

  auto request_idr() -> Result override {
    calls.emplace_back("idr");
    ++keyframe_requests;
    return {};
  }

  auto send_audio_state(
      libchiaking::net::protocol::remote_play::AudioStateKind kind,
      std::span<const std::byte> body) -> Result override {
    if (kind ==
        libchiaking::net::protocol::remote_play::AudioStateKind::port_states) {
      calls.emplace_back("audio_route");
      audio_route.assign(body.begin(), body.end());
      return {};
    }
    calls.emplace_back("audio_state");
    audio_state_kind = kind;
    audio_state.assign(body.begin(), body.end());
    return {};
  }

  auto send_create(play::CloudMessaging::CapturePress press)
      -> Result override {
    calls.emplace_back("create");
    creates.push_back(press);
    return {};
  }

  auto send_deep_link(std::string text) -> Result override {
    calls.emplace_back("deep_link");
    deep_links.push_back(std::move(text));
    if (deep_link_results.empty()) return {};
    auto result = deep_link_results.front();
    deep_link_results.pop_front();
    return result;
  }

  auto send_invitation(std::string invitation_id, std::string session_id)
      -> Result override {
    calls.emplace_back("invitation");
    invitations.emplace_back(std::move(invitation_id), std::move(session_id));
    return {};
  }

  auto open_saved_data() -> Result override {
    calls.emplace_back("saved_data");
    ++saved_data_requests;
    return {};
  }

  auto toggle_system_menu(std::uint32_t pad) -> Result override {
    calls.emplace_back("system_menu");
    system_menu_pads.push_back(pad);
    ++system_menu_toggles;
    return {};
  }

  auto send_keyboard(const play::KeyboardState& state) -> Result override {
    calls.emplace_back("keyboard");
    keyboard = state;
    return {};
  }

  void set_event_sink(play::LinkEventSink& sink) noexcept override {
    sink_ = &sink;
  }

  auto diagnostics() const noexcept
      -> libchiaking::net::protocol::remote_play::StreamDiagnostics override {
    ++diagnostics_reads;
    return link;
  }

  auto takion_version() const noexcept
      -> std::optional<std::uint32_t> override {
    return version;
  }

  void emit(libchiaking::net::protocol::remote_play::StreamEvent event) {
    sink_->on_event(std::move(event));
  }

  void announce_source_keys(
      std::uint32_t takion_version,
      const net::protocol::takion::SourceReceiveKeys& keys) {
    if (source_tap != nullptr) source_tap->on_source_keys(takion_version, keys);
  }

  void forward_source(const net::protocol::takion::SourceDatagram& datagram) {
    if (source_tap != nullptr) source_tap->on_source_datagram(datagram);
  }

  void set_state(StreamState state) {
    state_ = state;
    sink_->on_event(
        libchiaking::net::protocol::remote_play::event::StateChanged{state});
  }

  void fail(play::LinkFailure detail) {
    ending().record_failure(detail);
    state_ = StreamState::failed;
    sink_->on_event(libchiaking::net::protocol::remote_play::event::Failed{
        libchiaking::net::protocol::remote_play::StreamFailure::
            pipeline_failed});
  }

  void fail_silently() { state_ = StreamState::failed; }

  void fail_cleanup(play::LinkFailure detail) {
    ending().record_cleanup_failure(std::move(detail));
  }

  void quit_with(libchiaking::net::protocol::remote_play::QuitReason reason,
                 std::string_view text) {
    ending().record_quit(reason, text);
  }

  auto count(std::string_view name) const -> std::size_t {
    std::size_t total = 0;
    for (const auto& call : calls) total += call == name ? 1U : 0U;
    return total;
  }

  auto calls_between_pumps(std::size_t earlier) const
      -> std::vector<std::string> {
    std::vector<std::string> between;
    std::size_t seen = 0;
    for (const auto& call : calls) {
      if (call == "pump") {
        ++seen;
        continue;
      }
      if (seen == earlier) between.push_back(call);
    }
    return between;
  }

  Result start_result{};
  StreamState start_state{StreamState::establishing_route};
  Result pump_result{};
  Result wait_result{};
  Result stop_result{};
  Result pin_result{};
  Result loss_result{};
  Result microphone_result{};
  Result microphone_connect_result{};
  std::deque<Result> controller_results;
  std::deque<Result> deep_link_results;
  std::optional<std::size_t> refused_command;
  StreamError command_error{StreamError::invalid_argument};
  bool stop_when_idle{true};
  std::deque<Step> script;
  net::protocol::takion::SourceTap* source_tap{};

  std::vector<std::string> calls;
  int pumps{};
  int wakes{};
  int stops{};
  int sleep_stops{};
  int keyframe_requests{};
  mutable int diagnostics_reads{};
  std::vector<std::chrono::milliseconds> waits;
  std::vector<net::protocol::remote_play::ControllerState> controllers;
  std::string pin;
  std::vector<std::pair<std::uint16_t, std::uint16_t>> losses;
  std::vector<libchiaking::net::protocol::remote_play::StreamCommand> commands;
  std::vector<std::vector<std::byte>> microphone_frames;
  libchiaking::net::protocol::remote_play::AudioStateKind audio_state_kind{
      libchiaking::net::protocol::remote_play::AudioStateKind::full};
  std::vector<std::byte> audio_state;
  std::vector<std::byte> audio_route;
  std::vector<std::string> deep_links;
  std::vector<std::pair<std::string, std::string>> invitations;
  int system_menu_toggles{};
  int saved_data_requests{};
  std::vector<std::uint32_t> system_menu_pads;
  std::vector<play::CloudMessaging::CapturePress> creates;
  play::KeyboardState keyboard;
  std::string datacenter;
  libchiaking::net::protocol::remote_play::StreamDiagnostics link{};
  std::optional<std::uint32_t> version;

 private:
  struct Copy {
    auto operator()(
        const libchiaking::net::protocol::remote_play::command::KeyboardSetText&
            value) const
        -> libchiaking::net::protocol::remote_play::StreamCommand {
      return value.clone();
    }
    auto operator()(
        const libchiaking::net::protocol::remote_play::command::CouchPasscode&
            value) const
        -> libchiaking::net::protocol::remote_play::StreamCommand {
      return value.clone();
    }
    template <class Other>
    auto operator()(const Other& value) const
        -> libchiaking::net::protocol::remote_play::StreamCommand {
      return value;
    }
  };

  struct Name {
    auto operator()(
        const libchiaking::net::protocol::remote_play::command::GotoBed&) const
        -> std::string {
      return "goto_bed";
    }
    auto operator()(
        const libchiaking::net::protocol::remote_play::command::GoHome&) const
        -> std::string {
      return "go_home";
    }
    auto operator()(const libchiaking::net::protocol::remote_play::command::
                        KeyboardSetText&) const -> std::string {
      return "keyboard_text";
    }
    auto operator()(
        const libchiaking::net::protocol::remote_play::command::KeyboardAccept&)
        const -> std::string {
      return "keyboard_accept";
    }
    auto operator()(
        const libchiaking::net::protocol::remote_play::command::KeyboardReject&)
        const -> std::string {
      return "keyboard_reject";
    }
    auto operator()(const libchiaking::net::protocol::remote_play::command::
                        CouchSetAccountId&) const -> std::string {
      return "couch_account";
    }
    auto operator()(
        const libchiaking::net::protocol::remote_play::command::CouchJoin&)
        const -> std::string {
      return "couch_join";
    }
    auto operator()(
        const libchiaking::net::protocol::remote_play::command::CouchRemove&)
        const -> std::string {
      return "couch_remove";
    }
    auto operator()(
        const libchiaking::net::protocol::remote_play::command::CouchPasscode&)
        const -> std::string {
      return "couch_passcode";
    }
    template <class Other>
    auto operator()(const Other&) const -> std::string {
      return "command";
    }
  };

  void finish_stop() {
    if (state_ == StreamState::stopped) return;
    state_ = StreamState::stopped;
    sink_->on_event(ending().stopped_event());
  }

  libchiaking::platform::StopSource stop_;
  play::LinkEventSink* sink_{};
  StreamState state_{StreamState::idle};
};

}  // namespace libchiaking::testing
