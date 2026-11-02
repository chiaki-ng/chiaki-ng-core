#pragma once

#include "libchiaking/detail/play/internet_pairer.hpp"
#include "libchiaking/platform/default_factory.hpp"
#include "src/core/play/adapters/links/internet/internet_pairer_core.hpp"
#include "tests/support/api/fakes.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test {

class RemotePairingRig {
 public:
  using Pairing = libchiaking::play::InternetPairer::Core::Attempt;
  using Stage = libchiaking::consoles::InternetPairingStage;
  using StreamError = libchiaking::net::protocol::remote_play::StreamError;
  using StreamEvent = libchiaking::net::protocol::remote_play::StreamEvent;
  using StreamState = libchiaking::net::protocol::remote_play::StreamState;
  using FailureStage = libchiaking::net::protocol::remote_play::FailureStage;
  using RegisteredConsole =
      libchiaking::net::protocol::remote_play::RegisteredConsole;

  struct Script {
    std::vector<std::vector<StreamEvent>> pumps;
    std::optional<libchiaking::play::LinkFailure> failure;
    std::function<void()> on_pump;
    libchiaking::Expected<void, StreamError> start_result{};
    int pumped{};
    int stops{};
    int wakes{};
  };

  class Session final : public libchiaking::play::Link {
   public:
    explicit Session(std::shared_ptr<Script> script) noexcept
        : Link(libchiaking::play::Venue::internet()),
          script_(std::move(script)) {}

    auto start() -> libchiaking::Expected<void, StreamError> override {
      namespace event = libchiaking::net::protocol::remote_play::event;
      if (!script_->start_result) return script_->start_result;
      state_ = StreamState::establishing_route;
      sink_->on_event(event::StateChanged{state_});
      sink_->on_event(event::SetupProgress{FailureStage::psn_open});
      return {};
    }

    auto pump_once() -> libchiaking::Expected<void, StreamError> override {
      namespace event = libchiaking::net::protocol::remote_play::event;
      const auto index = static_cast<std::size_t>(script_->pumped++);
      if (script_->on_pump) script_->on_pump();
      if (index < script_->pumps.size()) {
        for (auto& happened : script_->pumps[index])
          sink_->on_event(std::move(happened));
        return {};
      }
      if (script_->failure) {
        ending().record_failure(*script_->failure);
        state_ = StreamState::failed;
        sink_->on_event(event::StateChanged{state_});
        return libchiaking::unexpected(script_->failure->result());
      }
      return {};
    }

    auto wait(std::chrono::milliseconds)
        -> libchiaking::Expected<void, StreamError> override {
      return {};
    }
    void wake() noexcept override { ++script_->wakes; }

    auto stop() -> libchiaking::Expected<void, StreamError> override {
      ++script_->stops;
      state_ = StreamState::stopped;
      return {};
    }

    auto state() const noexcept -> StreamState override { return state_; }
    auto send_controller_state(
        const libchiaking::net::protocol::remote_play::ControllerState&)
        -> libchiaking::Expected<void, StreamError> override {
      return {};
    }
    auto submit_pin(std::span<const std::byte>)
        -> libchiaking::Expected<void, StreamError> override {
      return {};
    }
    auto report_video_loss(std::uint16_t, std::uint16_t)
        -> libchiaking::Expected<void, StreamError> override {
      return {};
    }
    void set_event_sink(
        libchiaking::play::LinkEventSink& sink) noexcept override {
      sink_ = &sink;
    }

   private:
    std::shared_ptr<Script> script_;
    libchiaking::play::LinkEventSink* sink_{};
    StreamState state_{StreamState::idle};
  };

  static auto registered_console() -> RegisteredConsole {
    using libchiaking::net::protocol::remote_play::RegistrationTarget;
    RegisteredConsole console;
    console.target = RegistrationTarget::ps5_1;
    console.mac = {std::byte{0x7a}, std::byte{0x1b}, std::byte{0x2c},
                   std::byte{0x3d}, std::byte{0x4e}, std::byte{0x5f}};
    for (std::size_t index = 0; index < console.registration_key.size();
         ++index)
      console.registration_key[index] = static_cast<std::byte>(index + 1);
    for (std::size_t index = 0; index < console.key.size(); ++index)
      console.key[index] = static_cast<std::byte>(0xa0 + index);
    console.key_type = 2;
    console.console_pin = 24680;
    std::ranges::copy(std::string_view{"PS4-Living"}, console.nickname.begin());
    std::ranges::copy(std::string_view{"5f4e3d2c"}, console.ap_key.begin());
    return console;
  }

  static auto progress(FailureStage stage) -> std::vector<StreamEvent> {
    std::vector<StreamEvent> events;
    events.emplace_back(
        libchiaking::net::protocol::remote_play::event::SetupProgress{stage});
    return events;
  }

  static auto registered() -> std::vector<StreamEvent> {
    std::vector<StreamEvent> events;
    events.emplace_back(
        libchiaking::net::protocol::remote_play::event::Registered{
            registered_console()});
    return events;
  }

  auto pairing(libchiaking::consoles::PairingWatcher* watcher = nullptr,
               libchiaking::platform::StopToken stop = {}) -> Pairing {
    return Pairing{factory, log, watcher, std::move(stop)};
  }

  libchiaking::platform::DefaultPlatformFactory factory;
  test_support::FakeLog log;
};

class StagesHeard final : public libchiaking::consoles::PairingWatcher {
 public:
  void on_stage(libchiaking::consoles::InternetPairingStage stage) override {
    stages.push_back(stage);
  }

  std::vector<libchiaking::consoles::InternetPairingStage> stages;
};

}  // namespace play_test
