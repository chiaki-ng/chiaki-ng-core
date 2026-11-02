#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/controllers_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace play_test {
namespace {

using libchiaking::unexpected;
using libchiaking::net::protocol::remote_play::StreamError;
using libchiaking::play::InputRefusal;
using Core = ControllersRig::Core;
using Rig = ControllersRig;

TEST(ControllersQueue, HoldsThirtyTwoSamplesAndRetriesTwice) {
  EXPECT_EQ(Core::queue_depth, 32U);
  EXPECT_EQ(Core::send_retries, 2U);
}

TEST(ControllersQueue, RefusesInputBeforeTheSessionRuns) {
  Rig rig;
  const auto refused = rig.queue().send(Rig::host, Rig::sample(0));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), InputRefusal::not_running);
}

TEST(ControllersQueue, CoalescesMovesAndKeepsEveryEdge) {
  Rig rig;
  rig.start();
  auto& queue = rig.queue();
  auto pressed = Rig::sample(2);
  pressed.l2 = 1;
  pressed.left_x = 7;
  auto released = Rig::sample(0);
  released.left_x = 7;
  auto moved = Rig::sample(0);
  moved.l2 = 200;
  moved.r2 = 100;
  moved.left_x = -1000;
  moved.left_y = 1000;
  moved.right_x = -500;
  moved.right_y = 500;
  EXPECT_TRUE(queue.send(Rig::host, Rig::sample(0)));
  EXPECT_TRUE(queue.send(Rig::host, pressed));
  EXPECT_TRUE(queue.send(Rig::host, pressed));
  EXPECT_TRUE(queue.send(Rig::host, released));
  EXPECT_TRUE(queue.send(Rig::host, moved));
  EXPECT_EQ(queue.pending(Rig::host), 3U);
  EXPECT_EQ(queue.pending(), 3U);
  EXPECT_EQ(rig.session().wakes, 5);

  EXPECT_TRUE(queue.flush());
  const auto& sent = rig.session().controllers;
  ASSERT_EQ(sent.size(), 3U);
  EXPECT_EQ(queue.pending(), 0U);
  EXPECT_EQ(sent[0].buttons, 0U);
  EXPECT_EQ(sent[1].buttons, 2U);
  EXPECT_EQ(sent[2].buttons, 0U);
  EXPECT_EQ(sent[2].l2, 200);
  EXPECT_EQ(sent[2].r2, 100);
  EXPECT_EQ(sent[2].left_x, -1000);
  EXPECT_EQ(sent[2].right_y, 500);
  EXPECT_EQ(sent[2].touches[0].id, -1);
  EXPECT_EQ(sent[2].accel_y, 1.0F);
  EXPECT_EQ(sent[2].orient_w, 1.0F);
  EXPECT_TRUE(queue.flush());
  EXPECT_EQ(sent.size(), 3U);
  EXPECT_EQ(rig.logged("pad 0 input accepted by the session"), 1U);
}

TEST(ControllersQueue, KeepsNoMoreThanItsDepthOfEdges) {
  Rig rig;
  rig.start();
  for (std::uint32_t index = 0; index < 40; ++index) {
    SCOPED_TRACE(index);
    EXPECT_TRUE(
        rig.queue().send(Rig::host, Rig::sample((index & 1U) != 0 ? 4U : 0U)));
  }
  EXPECT_EQ(rig.queue().pending(), Core::queue_depth);
}

TEST(ControllersQueue, LetsAnUnchangedSampleRideTheNextPassWithoutAWake) {
  Rig rig{Core::Waking::changed_samples};
  rig.start();
  auto& queue = rig.queue();
  auto held = Rig::sample(2);
  held.left_x = 300;
  EXPECT_TRUE(queue.send(Rig::host, held));
  EXPECT_EQ(rig.session().wakes, 1);
  EXPECT_TRUE(queue.flush());
  EXPECT_EQ(queue.pending(), 0U);

  for (int count = 0; count < 120; ++count) {
    SCOPED_TRACE(count);
    EXPECT_TRUE(queue.send(Rig::host, held));
  }
  EXPECT_EQ(rig.session().wakes, 1);
  EXPECT_EQ(queue.pending(Rig::host), 1U);
  EXPECT_TRUE(queue.flush());
  ASSERT_EQ(rig.session().controllers.size(), 2U);
  EXPECT_EQ(rig.session().controllers.back().left_x, 300);
}

TEST(ControllersQueue, WakesForAChangedSampleButNotForMotionAlone) {
  Rig rig{Core::Waking::changed_samples};
  rig.start();
  auto& queue = rig.queue();
  auto held = Rig::sample(2);
  held.left_x = 300;
  EXPECT_TRUE(queue.send(Rig::host, held));
  EXPECT_TRUE(queue.flush());

  auto tilted = held;
  tilted.gyro_x = 0.01F;
  EXPECT_TRUE(queue.send(Rig::host, tilted));
  EXPECT_EQ(rig.session().wakes, 1);
  EXPECT_EQ(queue.pending(Rig::host), 1U);
  auto moved = tilted;
  moved.left_x = 301;
  EXPECT_TRUE(queue.send(Rig::host, moved));
  EXPECT_EQ(rig.session().wakes, 2);
  EXPECT_TRUE(queue.send(Rig::host, moved));
  EXPECT_EQ(rig.session().wakes, 2);
  queue.discard_pending(Rig::host);
  EXPECT_TRUE(queue.send(Rig::host, moved));
  EXPECT_EQ(rig.session().wakes, 3);
}

TEST(ControllersQueue, WakesForEverySampleByDefault) {
  Rig rig;
  rig.start();
  auto held = Rig::sample(2);
  held.left_x = 300;
  for (int count = 0; count < 3; ++count) {
    SCOPED_TRACE(count);
    EXPECT_TRUE(rig.queue().send(Rig::host, held));
  }
  EXPECT_EQ(rig.session().wakes, 3);
}

TEST(ControllersQueue, RetriesARefusedSampleTwiceThenDropsIt) {
  Rig rig;
  rig.start();
  auto& queue = rig.queue();
  const api_test::ScriptedSession::Result rejected =
      unexpected(StreamError::controller_rejected);

  rig.session().controller_results = {rejected};
  EXPECT_TRUE(queue.send(Rig::host, Rig::sample(2)));
  const auto first = queue.flush();
  ASSERT_FALSE(first);
  EXPECT_EQ(first.error(), StreamError::controller_rejected);
  EXPECT_EQ(queue.pending(), 1U);
  EXPECT_TRUE(queue.flush());
  EXPECT_EQ(queue.pending(), 0U);
  ASSERT_EQ(rig.session().controllers.size(), 2U);
  EXPECT_EQ(rig.session().controllers[1].buttons, 2U);
  EXPECT_EQ(rig.logged("pad 0 input was refused: result=controller_rejected"),
            1U);

  rig.session().controller_results = {rejected, rejected, rejected};
  EXPECT_TRUE(queue.send(Rig::host, Rig::sample(8)));
  EXPECT_FALSE(queue.flush());
  EXPECT_EQ(queue.pending(), 1U);
  EXPECT_FALSE(queue.flush());
  EXPECT_EQ(queue.pending(), 1U);
  EXPECT_FALSE(queue.flush());
  EXPECT_EQ(queue.pending(), 0U);
  EXPECT_EQ(rig.session().controllers.size(), 5U);
}

TEST(ControllersQueue, CoalescesMotionChangesLikeMoves) {
  Rig rig;
  rig.start();
  auto& queue = rig.queue();
  auto tilted = Rig::sample(0);
  tilted.gyro_x = 0.5F;
  tilted.gyro_y = -0.25F;
  tilted.accel_y = 0.9F;
  tilted.orient_x = 0.2F;
  tilted.orient_w = 0.98F;
  auto turned = tilted;
  turned.gyro_x = 2.0F;
  EXPECT_TRUE(queue.send(Rig::host, Rig::sample(0)));
  EXPECT_TRUE(queue.send(Rig::host, tilted));
  EXPECT_TRUE(queue.send(Rig::host, tilted));
  EXPECT_TRUE(queue.send(Rig::host, turned));
  EXPECT_EQ(queue.pending(), 2U);
  EXPECT_TRUE(queue.flush());
  const auto& sent = rig.session().controllers;
  ASSERT_EQ(sent.size(), 2U);
  EXPECT_EQ(sent[1].gyro_x, 2.0F);
  EXPECT_EQ(sent[1].gyro_y, -0.25F);
  EXPECT_EQ(sent[1].accel_y, 0.9F);
  EXPECT_EQ(sent[1].orient_w, 0.98F);
}

TEST(ControllersQueue, KeepsATouchStartClickAndLift) {
  Rig rig;
  rig.start();
  auto& queue = rig.queue();
  const std::uint32_t click = 1U << 14U;
  EXPECT_TRUE(queue.send(Rig::host, Rig::sample(0)));
  EXPECT_TRUE(queue.send(Rig::host, Rig::touched(5, 300, 400)));
  EXPECT_TRUE(queue.send(Rig::host, Rig::touched(5, 300, 400)));
  EXPECT_TRUE(queue.send(Rig::host, Rig::touched(5, 900, 500)));
  EXPECT_TRUE(queue.send(Rig::host, Rig::touched(5, 900, 500, click)));
  EXPECT_TRUE(queue.send(Rig::host, Rig::sample(0)));
  EXPECT_EQ(queue.pending(), 4U);
  EXPECT_TRUE(queue.flush());
  const auto& sent = rig.session().controllers;
  ASSERT_EQ(sent.size(), 4U);
  EXPECT_EQ(sent[0].touches[0].id, -1);
  EXPECT_EQ(sent[1].touches[0].id, 5);
  EXPECT_EQ(sent[1].buttons, 0U);
  EXPECT_EQ(sent[1].touches[0].x, 900);
  EXPECT_EQ(sent[2].touches[0].id, 5);
  EXPECT_EQ(sent[2].buttons, click);
  EXPECT_EQ(sent[3].touches[0].id, -1);
  EXPECT_EQ(sent[3].buttons, 0U);
}

TEST(ControllersQueue, CoalescesATouchThatOnlyMoves) {
  Rig rig;
  rig.start();
  auto& queue = rig.queue();
  EXPECT_TRUE(queue.send(Rig::host, Rig::touched(6, 100, 100)));
  EXPECT_TRUE(queue.send(Rig::host, Rig::touched(6, 200, 100)));
  EXPECT_TRUE(queue.send(Rig::host, Rig::touched(6, 300, 100)));
  EXPECT_TRUE(queue.send(Rig::host, Rig::sample(0)));
  EXPECT_EQ(queue.pending(), 3U);
  EXPECT_TRUE(queue.flush());
  const auto& sent = rig.session().controllers;
  ASSERT_EQ(sent.size(), 3U);
  EXPECT_EQ(sent[0].touches[0].id, 6);
  EXPECT_EQ(sent[1].touches[0].x, 300);
  EXPECT_EQ(sent[2].touches[0].id, -1);
}

}  // namespace
}  // namespace play_test
