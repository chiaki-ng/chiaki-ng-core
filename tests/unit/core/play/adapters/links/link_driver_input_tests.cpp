#include "tests/support/enum_printing.hpp"
#include "tests/support/live_providers.hpp"
#include "tests/unit/core/play/adapters/links/link_driver_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace play_test {

using test_support::LiveProviders;
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::Expected;
using libchiaking::unexpected;
using libchiaking::play::LinkDriver;
using link_drivers::Rig;
using remote_play::StreamError;
using remote_play::StreamState;

class LinkDriverInput : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(rig_.driver().start()); }

  auto rig() -> Rig& { return rig_; }

 private:
  Rig rig_;
};

TEST_F(LinkDriverInput, SendsControllerStateInlineWhenTheSessionIsFree) {
  remote_play::ControllerState sample;
  sample.buttons = 7;
  EXPECT_TRUE(rig().driver().send_controller_state(sample));
  EXPECT_EQ(rig().session().buttons, std::vector<std::uint32_t>{7});
  rig().session().controller_result =
      unexpected(StreamError::controller_rejected);
  const auto rejected = rig().driver().send_controller_state(sample);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), StreamError::controller_rejected);
}

TEST_F(LinkDriverInput, SubmitsAPinInline) {
  const std::array pin{std::byte{'1'}, std::byte{'2'}, std::byte{'3'},
                       std::byte{'4'}};
  EXPECT_TRUE(rig().driver().submit_pin(pin));
  EXPECT_TRUE(std::ranges::equal(rig().session().pin, pin));
  EXPECT_TRUE(rig().driver().submit_pin({}));
  EXPECT_TRUE(rig().session().pin.empty());
}

TEST_F(LinkDriverInput, ReportsVideoLossInline) {
  EXPECT_TRUE(rig().driver().report_video_loss(3, 9));
  EXPECT_EQ(rig().session().loss.first, 3);
  EXPECT_EQ(rig().session().loss.second, 9);
}

TEST_F(LinkDriverInput, DrivesTheMicrophoneInline) {
  const std::array<std::byte, LinkDriver::microphone_frame_size> frame{};
  EXPECT_TRUE(rig().driver().send_microphone(frame));
  EXPECT_EQ(rig().session().microphone_bytes, 40U);
  EXPECT_TRUE(rig().driver().connect_microphone());
  EXPECT_EQ(rig().session().microphone_connects, 1U);
  EXPECT_TRUE(rig().driver().set_microphone_muted(true));
  EXPECT_TRUE(rig().session().muted);
}

TEST_F(LinkDriverInput, RequestsAKeyframeAndSendsAudioStateInline) {
  EXPECT_TRUE(rig().driver().request_idr());
  EXPECT_EQ(rig().session().keyframe_requests, 1U);
  const std::array body{std::byte{9}, std::byte{8}};
  EXPECT_TRUE(rig().driver().send_audio_state(
      remote_play::AudioStateKind::channel_count, body));
  EXPECT_TRUE(std::ranges::equal(rig().session().audio_state, body));
}

TEST_F(LinkDriverInput, ExecutesCommandsInline) {
  EXPECT_TRUE(rig().driver().execute(remote_play::command::GoHome{}));
  EXPECT_TRUE(rig().driver().execute(remote_play::command::SetPadCount{2}));
  ASSERT_EQ(rig().session().commands.size(), 2U);
  EXPECT_EQ(rig().session().commands[0],
            remote_play::StreamCommand{remote_play::command::GoHome{}}.index());
}

TEST_F(LinkDriverInput, PassesOnTheSessionVersionAndNoFailure) {
  EXPECT_EQ(rig().driver().takion_version(), 12U);
  EXPECT_FALSE(rig().driver().failure());
  EXPECT_FALSE(rig().driver().cleanup_failure());
  EXPECT_EQ(rig().driver().quit().reason, remote_play::QuitReason::none);
}

TEST_F(LinkDriverInput, QueuesInputDuringAWaitWithoutBlocking) {
  std::atomic<bool> sent{false};
  Expected<void, StreamError> submitted =
      unexpected(StreamError::pipeline_failed);
  auto input = LiveProviders::spawn("test", [&] {
    while (!rig().session().waiting.load())
      LiveProviders::nap(std::chrono::milliseconds{1});
    remote_play::ControllerState sample;
    sample.buttons = 5;
    submitted = rig().driver().send_controller_state(sample);
    sent = true;
  });
  const auto began = LiveProviders::steady_us();
  EXPECT_TRUE(rig().driver().wait(std::chrono::milliseconds{3000}));
  const auto waited = Rig::since(began);
  static_cast<void>(input.join());
  EXPECT_TRUE(sent.load());
  EXPECT_TRUE(submitted);
  EXPECT_LT(waited, std::chrono::milliseconds{2000});
  EXPECT_TRUE(rig().session().buttons.empty());
  const auto again = LiveProviders::steady_us();
  EXPECT_TRUE(rig().driver().wait(std::chrono::milliseconds{3000}));
  EXPECT_LT(Rig::since(again), std::chrono::milliseconds{2000});
  EXPECT_TRUE(rig().driver().pump_once());
  EXPECT_EQ(rig().session().buttons, std::vector<std::uint32_t>{5});
  EXPECT_TRUE(rig().driver().stop());
}

TEST(LinkDriverHostInput, ArrivesInOrderFromAnotherThread) {
  Rig rig;
  std::atomic<bool> done{false};
  std::atomic<unsigned> rounds{0};
  auto input = LiveProviders::spawn("test", [&] {
    remote_play::ControllerState sample;
    std::uint32_t buttons = 0;
    while (!done.load()) {
      sample.buttons = buttons++;
      const auto sent = rig.driver().send_controller_state(sample);
      EXPECT_TRUE(sent || sent.error() == StreamError::invalid_state);
      const auto idr = rig.driver().request_idr();
      EXPECT_TRUE(idr || idr.error() == StreamError::invalid_state);
      rounds.fetch_add(1);
    }
  });
  while (rounds.load() < 4) LiveProviders::nap(std::chrono::milliseconds{1});
  EXPECT_TRUE(rig.driver().start());
  const auto started = rounds.load();
  while (rounds.load() < started + 64) {
    EXPECT_TRUE(rig.driver().pump_once());
    EXPECT_TRUE(rig.driver().wait(std::chrono::milliseconds{1}));
  }
  EXPECT_TRUE(rig.driver().stop());
  const auto stopped = rounds.load();
  while (rounds.load() < stopped + 4)
    LiveProviders::nap(std::chrono::milliseconds{1});
  done = true;
  static_cast<void>(input.join());
  EXPECT_TRUE(std::ranges::is_sorted(rig.session().buttons));
  EXPECT_GE(rig.session().buttons.size(), 4U);
}

TEST(LinkDriverPendingInput, IsBoundedAndDrainedByTheNextPump) {
  Rig rig;
  std::size_t accepted = 0;
  Expected<void, StreamError> overflow{};
  rig.sink().on_state = [&](StreamState state) {
    if (state != StreamState::establishing_route) return;
    for (std::size_t index = 0; index < LinkDriver::max_pending_inputs; ++index)
      accepted += rig.driver().request_idr() ? 1U : 0U;
    overflow = rig.driver().request_idr();
  };
  ASSERT_TRUE(rig.driver().start());
  EXPECT_EQ(accepted, 4096U);
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error(), StreamError::invalid_state);
  EXPECT_EQ(rig.session().keyframe_requests, 0U);
  EXPECT_EQ(rig.session().wakes.load(), 4096U);
  EXPECT_TRUE(rig.driver().wait(std::chrono::milliseconds{3000}));
  EXPECT_TRUE(rig.driver().pump_once());
  EXPECT_EQ(rig.session().keyframe_requests, 4096U);
}

TEST(LinkDriverPendingInput, RefusesControllerStatesOnceTheQueueIsFull) {
  Rig rig;
  std::size_t accepted = 0;
  Expected<void, StreamError> overflow{};
  rig.sink().on_state = [&](StreamState state) {
    if (state != StreamState::establishing_route) return;
    remote_play::ControllerState sample;
    for (std::size_t index = 0; index < LinkDriver::max_pending_inputs;
         ++index) {
      sample.buttons = static_cast<std::uint32_t>(index);
      accepted += rig.driver().send_controller_state(sample) ? 1U : 0U;
    }
    overflow = rig.driver().send_controller_state(sample);
  };
  ASSERT_TRUE(rig.driver().start());
  EXPECT_EQ(accepted, LinkDriver::max_pending_inputs);
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error(), StreamError::invalid_state);
  EXPECT_TRUE(rig.session().buttons.empty());
  EXPECT_TRUE(rig.driver().pump_once());
  ASSERT_EQ(rig.session().buttons.size(), LinkDriver::max_pending_inputs);
  EXPECT_TRUE(std::ranges::is_sorted(rig.session().buttons));
}

TEST(LinkDriverPendingInput, RunsQueuedInputsBeforeADirectOne) {
  Rig rig;
  rig.sink().on_state = [&](StreamState state) {
    if (state != StreamState::establishing_route) return;
    remote_play::ControllerState sample;
    sample.buttons = 1;
    EXPECT_TRUE(rig.driver().send_controller_state(sample));
    sample.buttons = 2;
    EXPECT_TRUE(rig.driver().send_controller_state(sample));
  };
  ASSERT_TRUE(rig.driver().start());
  EXPECT_TRUE(rig.session().buttons.empty());
  remote_play::ControllerState direct;
  direct.buttons = 3;
  EXPECT_TRUE(rig.driver().send_controller_state(direct));
  EXPECT_EQ(rig.session().buttons, (std::vector<std::uint32_t>{1, 2, 3}));
}

TEST(LinkDriverHostInput, KeepsEachProducersOrderUnderContention) {
  Rig rig;
  ASSERT_TRUE(rig.driver().start());
  constexpr std::uint32_t per_producer = 2000;
  constexpr std::uint32_t producer_bit = 1U << 20;
  std::atomic<bool> done{false};
  std::array<std::atomic<std::uint32_t>, 2> accepted{};
  const auto produce = [&](std::size_t producer) {
    remote_play::ControllerState sample;
    for (std::uint32_t index = 0; index < per_producer; ++index) {
      sample.buttons = (producer == 0 ? 0U : producer_bit) | index;
      if (rig.driver().send_controller_state(sample))
        accepted[producer].fetch_add(1);
    }
  };
  auto first = LiveProviders::spawn("first", [&] { produce(0); });
  auto second = LiveProviders::spawn("second", [&] { produce(1); });
  auto pump = LiveProviders::spawn("pump", [&] {
    while (!done.load()) {
      EXPECT_TRUE(rig.driver().pump_once());
      EXPECT_TRUE(rig.driver().wait(std::chrono::milliseconds{1}));
    }
  });
  static_cast<void>(first.join());
  static_cast<void>(second.join());
  done = true;
  static_cast<void>(pump.join());
  EXPECT_TRUE(rig.driver().pump_once());
  std::array<std::vector<std::uint32_t>, 2> received;
  for (const auto buttons : rig.session().buttons)
    received[(buttons & producer_bit) != 0 ? 1 : 0].push_back(buttons &
                                                              ~producer_bit);
  for (std::size_t producer = 0; producer < received.size(); ++producer) {
    SCOPED_TRACE(producer);
    EXPECT_EQ(received[producer].size(), accepted[producer].load());
    EXPECT_TRUE(std::ranges::is_sorted(received[producer]));
    EXPECT_EQ(std::ranges::adjacent_find(received[producer]),
              received[producer].end());
  }
  EXPECT_TRUE(rig.driver().stop());
}

TEST(LinkDriverPendingInput, LogsQueuedInputTheSessionRejects) {
  Rig rig;
  rig.session().controller_result =
      unexpected(StreamError::controller_rejected);
  rig.sink().on_state = [&](StreamState state) {
    if (state == StreamState::establishing_route) {
      EXPECT_TRUE(rig.driver().send_controller_state({}));
    }
  };
  ASSERT_TRUE(rig.driver().start());
  EXPECT_FALSE(rig.logged("queued host input was rejected by the session"));
  EXPECT_TRUE(rig.driver().pump_once());
  EXPECT_EQ(rig.session().buttons.size(), 1U);
  EXPECT_TRUE(rig.logged("queued host input was rejected by the session"));
}

TEST(LinkDriverPendingInput, IsDiscardedByAStop) {
  Rig discarding;
  discarding.sink().on_state = [&](StreamState state) {
    if (state != StreamState::establishing_route) return;
    EXPECT_TRUE(discarding.driver().request_idr());
    EXPECT_TRUE(discarding.driver().stop());
  };
  const auto started = discarding.driver().start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), StreamError::invalid_state);
  EXPECT_EQ(discarding.session().keyframe_requests, 0U);
  EXPECT_TRUE(discarding.driver().request_idr());
  EXPECT_EQ(discarding.session().keyframe_requests, 1U);
}

}  // namespace
}  // namespace play_test
