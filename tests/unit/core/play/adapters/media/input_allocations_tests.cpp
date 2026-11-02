#include "libchiaking/detail/diagnostics/log_queue.hpp"
#include "src/core/net/protocol/remote_play/stream/controller_delivery.hpp"
#include "src/core/play/adapters/links/link_driver.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/live_providers.hpp"
#include "tests/unit/core/play/adapters/media/counted_allocations.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace play_test {

using test_support::LiveProviders;
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;
using libchiaking::play::LinkDriver;

class CountingSink {
 public:
  auto post_controller_history_window(
      std::span<const takion::FeedbackHistoryEvent> events,
      std::span<const std::uint8_t> ids, std::size_t)
      -> libchiaking::Expected<void, int> {
    windows += events.size() == ids.size() ? 1U : 0U;
    return {};
  }

  auto post_controller_state(const remote_play::ControllerState&)
      -> libchiaking::Expected<void, int> {
    ++states;
    return {};
  }

  std::size_t windows{};
  std::size_t states{};
};

TEST(InputAllocations, ControllerDeliveryPostsEdgesWithoutAllocating) {
  CountingSink sink;
  remote_play::ControllerDelivery<CountingSink> delivery{sink, 0};
  remote_play::ControllerState state;
  const auto before = CountedAllocations::on_this_thread();
  for (std::uint64_t sample = 1; sample <= 2000; ++sample) {
    state.buttons = static_cast<std::uint32_t>(sample & 0x0fffU);
    state.l2 = static_cast<std::uint8_t>(sample);
    state.touches[0].id = sample % 3 == 0 ? std::int8_t{-1} : std::int8_t{1};
    state.touches[0].x = static_cast<std::uint16_t>(sample);
    state.left_x = static_cast<std::int16_t>(sample);
    ASSERT_TRUE(delivery.submit(state, sample * 1000));
    ASSERT_TRUE(delivery.refresh(sample * 1000 + 500));
  }
  EXPECT_EQ(CountedAllocations::on_this_thread(), before);
  EXPECT_GT(sink.windows, 2000U);
  EXPECT_GT(sink.states, 0U);
}

class CountingLink final : public libchiaking::play::Link {
 public:
  auto start() -> Result override {
    state_ = remote_play::StreamState::establishing_route;
    sink_->on_event(remote_play::event::StateChanged{state_});
    return {};
  }

  auto pump_once() -> Result override { return {}; }

  auto stop() -> Result override {
    state_ = remote_play::StreamState::stopped;
    return {};
  }

  auto state() const noexcept -> remote_play::StreamState override {
    return state_;
  }

  auto send_controller_state(const remote_play::ControllerState& state)
      -> Result override {
    in_order = in_order && state.buttons == states;
    ++states;
    return {};
  }

  auto send_microphone(std::span<const std::byte> frame) -> Result override {
    frames += frame.size() == LinkDriver::microphone_frame_size ? 1U : 0U;
    return {};
  }

  auto submit_pin(std::span<const std::byte>) -> Result override { return {}; }

  auto report_video_loss(std::uint16_t, std::uint16_t) -> Result override {
    return {};
  }

  void set_event_sink(
      libchiaking::play::LinkEventSink& sink) noexcept override {
    sink_ = &sink;
  }

  std::uint32_t states{};
  std::size_t frames{};
  bool in_order{true};

 private:
  libchiaking::play::LinkEventSink* sink_{};
  remote_play::StreamState state_{remote_play::StreamState::idle};
};

class QueueingSink final : public libchiaking::play::LinkDriverSink {
 public:
  static constexpr std::uint32_t samples = 1024;

  void on_event(remote_play::StreamEvent event) override {
    const auto* changed = std::get_if<remote_play::event::StateChanged>(&event);
    if (driver == nullptr || changed == nullptr ||
        changed->state != remote_play::StreamState::establishing_route) {
      return;
    }
    const std::array<std::byte, LinkDriver::microphone_frame_size> frame{};
    remote_play::ControllerState state;
    const auto before = CountedAllocations::on_this_thread();
    for (std::uint32_t sample = 0; sample < samples; ++sample) {
      state.buttons = sample;
      queued += driver->send_controller_state(state) ? 1U : 0U;
      queued += driver->send_microphone(frame) ? 1U : 0U;
    }
    allocations = CountedAllocations::on_this_thread() - before;
  }

  void on_video_frame(libchiaking::media::VideoFrame) override {}

  LinkDriver* driver{};
  std::size_t queued{};
  std::size_t allocations{};
};

TEST(InputAllocations, LinkDriverQueuesAndDrainsInputsWithoutAllocating) {
  auto* link = new CountingLink;
  libchiaking::platform::StopSource stop;
  QueueingSink sink;
  libchiaking::diagnostics::LogQueue log{LiveProviders::steady()};
  LinkDriver driver{std::unique_ptr<libchiaking::play::Link>{link}, stop,
                    LiveProviders::threading(), sink, log};
  sink.driver = &driver;
  ASSERT_TRUE(driver.start());
  EXPECT_EQ(sink.queued, 2U * QueueingSink::samples);
  EXPECT_EQ(sink.allocations, 0U);
  EXPECT_EQ(link->states, 0U);
  const auto before = CountedAllocations::on_this_thread();
  ASSERT_TRUE(driver.pump_once());
  EXPECT_EQ(CountedAllocations::on_this_thread(), before);
  EXPECT_EQ(link->states, QueueingSink::samples);
  EXPECT_EQ(link->frames, QueueingSink::samples);
  EXPECT_TRUE(link->in_order);
  remote_play::ControllerState direct;
  direct.buttons = QueueingSink::samples;
  const auto inline_before = CountedAllocations::on_this_thread();
  ASSERT_TRUE(driver.send_controller_state(direct));
  EXPECT_EQ(CountedAllocations::on_this_thread(), inline_before);
  EXPECT_EQ(link->states, QueueingSink::samples + 1);
  EXPECT_TRUE(link->in_order);
  sink.driver = nullptr;
}

}  // namespace
}  // namespace play_test
