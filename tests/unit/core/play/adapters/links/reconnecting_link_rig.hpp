#pragma once

#include "src/core/play/adapters/links/reconnecting_link.hpp"
#include "tests/support/live_providers.hpp"

#include <libchiaking/testing/manual_clock.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace play_test::reconnecting_links {

using test_support::LiveProviders;

namespace rp = libchiaking::net::protocol::remote_play;

class Script {
 public:
  void refuse_openings(std::vector<rp::StreamError> openings) {
    openings_ = std::move(openings);
  }

  auto next_refusal() -> std::optional<rp::StreamError> {
    const auto index = opened_++;
    if (index < openings_.size()) return openings_[index];
    return std::nullopt;
  }

  void count_stop() { ++stopped_; }
  void count_destruction() { ++destroyed_; }

  auto opened() const -> unsigned { return opened_; }
  auto stopped() const -> unsigned { return stopped_; }
  auto destroyed() const -> unsigned { return destroyed_; }

 private:
  std::vector<rp::StreamError> openings_;
  unsigned opened_{};
  unsigned stopped_{};
  unsigned destroyed_{};
};

class FakeLink final : public libchiaking::play::Link {
 public:
  explicit FakeLink(Script& script) : script_(script) {}
  FakeLink(const FakeLink&) = delete;
  auto operator=(const FakeLink&) -> FakeLink& = delete;
  ~FakeLink() override { script_.count_destruction(); }

  auto start() -> Result override {
    state_ = rp::StreamState::establishing_route;
    emit(rp::event::StateChanged{state_});
    return {};
  }
  auto pump_once() -> Result override { return {}; }
  auto stop() -> Result override {
    script_.count_stop();
    if (state_ != rp::StreamState::failed) state_ = rp::StreamState::stopped;
    return {};
  }
  auto state() const noexcept -> rp::StreamState override { return state_; }
  auto submit_pin(std::span<const std::byte>) -> Result override { return {}; }
  void set_event_sink(
      libchiaking::play::LinkEventSink& sink) noexcept override {
    sink_ = &sink;
  }
  auto diagnostics() const noexcept -> rp::StreamDiagnostics override {
    rp::StreamDiagnostics diagnostics{};
    diagnostics.video_units = 42;
    return diagnostics;
  }
  auto takion_version() const noexcept
      -> std::optional<std::uint32_t> override {
    return 20;
  }

  void stream() {
    state_ = rp::StreamState::streaming;
    emit(rp::event::StateChanged{state_});
  }

  void fail(rp::StreamError error) {
    ending().record_failure(
        libchiaking::play::LinkFailure{rp::FailureStage::ctrl_receive, error});
    state_ = rp::StreamState::failed;
    emit(rp::event::StateChanged{state_});
    emit(rp::event::Failed{rp::StreamFailure::negotiation_failed});
  }

  void lose(libchiaking::play::LinkFailure detail) {
    ending().record_failure(std::move(detail));
    state_ = rp::StreamState::failed;
    emit(rp::event::StateChanged{state_});
    emit(rp::event::Failed{rp::StreamFailure::pipeline_failed});
  }

 private:
  void emit(rp::StreamEvent event) {
    if (sink_) sink_->on_event(std::move(event));
  }

  Script& script_;
  libchiaking::play::LinkEventSink* sink_{};
  rp::StreamState state_{rp::StreamState::idle};
};

class Watcher final : public libchiaking::play::ReconnectingLink::Watcher {
 public:
  struct Try {
    std::uint32_t attempt;
    bool woke;
    bool reconnecting;
    std::chrono::milliseconds rest;
    bool offline;
  };

  auto wake_console() -> bool override {
    ++wakes_;
    return wakes_succeed_;
  }
  auto leaving() const noexcept -> bool override { return false; }
  void trying_again(std::uint32_t attempt, bool woke, bool reconnecting,
                    const libchiaking::play::StartPolicy::Failure& failure,
                    std::chrono::milliseconds rest) override {
    tries_.push_back(
        {attempt, woke, reconnecting, rest, failure.offline_for.has_value()});
  }

  void refuse_to_wake() { wakes_succeed_ = false; }
  auto wakes() const -> unsigned { return wakes_; }
  auto tries() const -> const std::vector<Try>& { return tries_; }

 private:
  unsigned wakes_{};
  bool wakes_succeed_{true};
  std::vector<Try> tries_;
};

class Sink final : public libchiaking::play::LinkEventSink {
 public:
  void on_event(rp::StreamEvent event) override {
    events_.push_back(std::move(event));
  }

  auto states() const -> std::vector<rp::StreamState> {
    std::vector<rp::StreamState> seen;
    for (const auto& event : events_)
      if (const auto* changed = std::get_if<rp::event::StateChanged>(&event))
        seen.push_back(changed->state);
    return seen;
  }

  auto failures() const -> std::size_t {
    return static_cast<std::size_t>(
        std::ranges::count_if(events_, [](const rp::StreamEvent& event) {
          return std::holds_alternative<rp::event::Failed>(event);
        }));
  }

  auto events() const -> std::size_t { return events_.size(); }

 private:
  std::vector<rp::StreamEvent> events_;
};

class Rig {
 public:
  explicit Rig(libchiaking::play::Where where = libchiaking::play::Where::home)
      : link_(
            where, [this] { return open(); }, stop_, clock_,
            LiveProviders::threading(), watcher_) {
    link_.set_event_sink(sink_);
  }

  auto script() -> Script& { return script_; }
  auto clock() -> libchiaking::testing::ManualClock& { return clock_; }
  auto stop() -> libchiaking::platform::StopSource& { return stop_; }
  auto watcher() -> Watcher& { return watcher_; }
  auto sink() -> Sink& { return sink_; }
  auto current() -> FakeLink& { return *current_; }
  auto link() -> libchiaking::play::ReconnectingLink& { return link_; }

 private:
  auto open() -> libchiaking::Expected<std::unique_ptr<libchiaking::play::Link>,
                                       rp::StreamError> {
    if (const auto refusal = script_.next_refusal())
      return libchiaking::unexpected(*refusal);
    auto next = std::make_unique<FakeLink>(script_);
    current_ = next.get();
    return std::unique_ptr<libchiaking::play::Link>{std::move(next)};
  }

  Script script_;
  libchiaking::testing::ManualClock clock_;
  libchiaking::platform::StopSource stop_;
  Watcher watcher_;
  Sink sink_;
  FakeLink* current_{};
  libchiaking::play::ReconnectingLink link_;
};

}  // namespace play_test::reconnecting_links
