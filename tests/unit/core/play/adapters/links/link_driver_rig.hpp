#pragma once

#include "libchiaking/detail/diagnostics/log_queue.hpp"
#include "src/core/play/adapters/links/link_driver.hpp"
#include "tests/support/live_providers.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace play_test::link_drivers {

using test_support::LiveProviders;

namespace remote_play = libchiaking::net::protocol::remote_play;

class FakeSession final : public libchiaking::play::Link {
 public:
  using StreamError = remote_play::StreamError;
  using StreamState = remote_play::StreamState;
  using Result = libchiaking::Expected<void, StreamError>;

  auto start() -> Result override {
    if (!start_result) return start_result;
    state_ = StreamState::establishing_route;
    sink_->on_event(remote_play::event::StateChanged{state_});
    return {};
  }

  auto pump_once() -> Result override {
    if (state_ == StreamState::stopped)
      return libchiaking::unexpected(StreamError::invalid_state);
    ++pumps;
    for (auto& event : std::exchange(pending_events, {}))
      sink_->on_event(std::move(event));
    return {};
  }

  auto wait(std::chrono::milliseconds timeout) -> Result override {
    libchiaking::platform::Lock lock(wake_mutex_);
    waiting.store(true);
    static_cast<void>(
        wake_condition_.wait_for(lock, timeout, [this] { return woken_; }));
    woken_ = false;
    waiting.store(false);
    return {};
  }

  void wake() noexcept override {
    {
      const libchiaking::platform::Lock lock(wake_mutex_);
      woken_ = true;
    }
    wakes.fetch_add(1);
    wake_condition_.notify_all();
  }

  auto select_datacenter(std::string_view name) -> Result override {
    if (name == "missing")
      return libchiaking::unexpected(StreamError::invalid_configuration);
    selected.assign(name);
    return {};
  }

  auto stop() -> Result override {
    ++stops;
    if (state_ != StreamState::stopped) {
      state_ = StreamState::stopped;
      sink_->on_event(ending().stopped_event());
    }
    return stop_result;
  }

  auto stop_for_sleep() -> Result override {
    ++sleep_stops;
    return stop();
  }

  auto state() const noexcept -> StreamState override { return state_; }

  auto send_controller_state(const remote_play::ControllerState& state)
      -> Result override {
    buttons.push_back(state.buttons);
    return controller_result;
  }

  auto submit_pin(std::span<const std::byte> bytes) -> Result override {
    pin.assign(bytes.begin(), bytes.end());
    return {};
  }

  auto report_video_loss(std::uint16_t first, std::uint16_t last)
      -> Result override {
    loss = {first, last};
    return {};
  }

  auto execute(const remote_play::StreamCommand& command) -> Result override {
    commands.push_back(command.index());
    return {};
  }

  auto send_microphone(std::span<const std::byte> frame) -> Result override {
    microphone_bytes = frame.size();
    return {};
  }

  auto connect_microphone() -> Result override {
    ++microphone_connects;
    return {};
  }

  auto set_microphone_muted(bool value) -> Result override {
    muted = value;
    return {};
  }

  auto request_idr() -> Result override {
    ++keyframe_requests;
    return {};
  }

  auto send_audio_state(remote_play::AudioStateKind,
                        std::span<const std::byte> body) -> Result override {
    audio_state.assign(body.begin(), body.end());
    return {};
  }

  void set_event_sink(
      libchiaking::play::LinkEventSink& sink) noexcept override {
    sink_ = &sink;
  }

  auto diagnostics() const noexcept -> remote_play::StreamDiagnostics override {
    remote_play::StreamDiagnostics value;
    value.available = true;
    value.video_units = pumps;
    return value;
  }

  auto takion_version() const noexcept
      -> std::optional<std::uint32_t> override {
    return 12;
  }

  Result start_result{};
  Result stop_result{};
  Result controller_result{};
  std::vector<remote_play::StreamEvent> pending_events;
  std::vector<std::uint32_t> buttons;
  std::vector<std::byte> pin;
  std::vector<std::byte> audio_state;
  std::vector<std::size_t> commands;
  std::pair<std::uint16_t, std::uint16_t> loss{};
  std::string selected;
  std::uint64_t pumps{};
  std::size_t microphone_bytes{};
  unsigned stops{};
  unsigned sleep_stops{};
  unsigned microphone_connects{};
  unsigned keyframe_requests{};
  bool muted{};
  std::atomic<unsigned> wakes{};
  std::atomic<bool> waiting{};

 private:
  libchiaking::play::LinkEventSink* sink_{};
  StreamState state_{StreamState::idle};
  libchiaking::platform::Mutex wake_mutex_{LiveProviders::threading()};
  libchiaking::platform::ConditionVariable wake_condition_{LiveProviders::threading(), LiveProviders::steady()};
  bool woken_{};
};

class IdleSession final : public libchiaking::play::Link {
 public:
  using Result = libchiaking::Expected<void, remote_play::StreamError>;

  auto start() -> Result override { return {}; }
  auto pump_once() -> Result override { return {}; }
  auto stop() -> Result override { return {}; }
  auto state() const noexcept -> remote_play::StreamState override {
    return remote_play::StreamState::idle;
  }
  auto send_controller_state(const remote_play::ControllerState&)
      -> Result override {
    return {};
  }
  auto submit_pin(std::span<const std::byte>) -> Result override { return {}; }
  auto report_video_loss(std::uint16_t, std::uint16_t) -> Result override {
    return {};
  }
  void set_event_sink(libchiaking::play::LinkEventSink&) noexcept override {}
};

class RecordingSink final : public libchiaking::play::LinkDriverSink {
 public:
  void on_event(remote_play::StreamEvent event) override {
    if (const auto* changed =
            std::get_if<remote_play::event::StateChanged>(&event)) {
      states.push_back(changed->state);
      if (on_state) on_state(changed->state);
    }
    if (const auto* media =
            std::get_if<remote_play::event::EncodedMedia>(&event))
      media_events.push_back(*media);
  }

  void on_video_frame(libchiaking::media::VideoFrame frame) override {
    frames.push_back(std::move(frame));
  }

  auto count(remote_play::StreamState wanted) const -> std::ptrdiff_t {
    return std::ranges::count(states, wanted);
  }

  std::function<void(remote_play::StreamState)> on_state;
  std::vector<remote_play::StreamState> states;
  std::vector<remote_play::event::EncodedMedia> media_events;
  std::vector<libchiaking::media::VideoFrame> frames;
};

class Rig {
 public:
  explicit Rig(std::optional<libchiaking::play::Venue> route =
                   libchiaking::play::Venue::home())
      : driver_(std::unique_ptr<libchiaking::play::Link>{session_}, stop_,
                LiveProviders::threading(), sink_, log_, route) {}

  auto logged(std::string_view needle) -> bool {
    while (const auto record = log_.read())
      lines_.emplace_back(record->message());
    return std::ranges::any_of(
        lines_, [needle](const std::string& line) { return line == needle; });
  }

  auto session() -> FakeSession& { return *session_; }
  auto stop() -> libchiaking::platform::StopSource& { return stop_; }
  auto sink() -> RecordingSink& { return sink_; }
  auto driver() -> libchiaking::play::LinkDriver& { return driver_; }

  static auto since(std::uint64_t began) -> std::chrono::microseconds {
    return std::chrono::microseconds{
        static_cast<std::int64_t>(LiveProviders::steady_us() - began)};
  }

 private:
  FakeSession* session_{new FakeSession};
  libchiaking::platform::StopSource stop_;
  RecordingSink sink_;
  libchiaking::diagnostics::LogQueue log_{LiveProviders::steady()};
  std::vector<std::string> lines_;
  libchiaking::play::LinkDriver driver_;
};

}  // namespace play_test::link_drivers
