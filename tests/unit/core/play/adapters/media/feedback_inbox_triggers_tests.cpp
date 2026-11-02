#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/media/feedback_rig.hpp"

#include <gtest/gtest.h>

namespace play_test {

using test_support::LiveProviders;
namespace {

namespace controllers = libchiaking::controllers;
using controllers::FeedbackIntensity;

class FeedbackInboxTriggers : public FeedbackRig {};

class FeedbackInboxPending : public FeedbackRig {
 protected:
  using Pending = Inbox::Pending;
};

TEST_F(FeedbackInboxTriggers, HoldsEffectsPerSeatWithTheConsoleIntensity) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  EXPECT_EQ(inbox.trigger_intensity(), FeedbackIntensity::strong);
  EXPECT_EQ(inbox.accept({seat(2), triggers()}, start), Admission::held);
  EXPECT_FALSE(inbox.take_triggers(seat(0)));
  EXPECT_FALSE(inbox.take_triggers(seat(1)));
  const auto effects = inbox.take_triggers(seat(2));
  ASSERT_TRUE(effects);
  EXPECT_EQ(effects->intensity, FeedbackIntensity::strong);
  EXPECT_EQ(effects->effects, triggers());
  const auto packed = effects->effects.packed();
  ASSERT_EQ(packed.size(), 22);
  EXPECT_EQ(packed[0], 0x21);
  EXPECT_EQ(packed[1], 0xaa);
  EXPECT_EQ(packed[11], 0x26);
  EXPECT_EQ(packed[12], 0xbb);
  EXPECT_EQ(packed[21], 0x08);
  EXPECT_FALSE(inbox.take_triggers(seat(2)));
}

TEST_F(FeedbackInboxTriggers, ReissuesEffectsWhenTheConsoleIntensityChanges) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  EXPECT_EQ(inbox.accept({seat(2), triggers()}, start), Admission::held);
  EXPECT_TRUE(inbox.take_triggers(seat(2)));
  EXPECT_EQ(
      inbox.accept(
          {seat(0), controllers::TriggerIntensity{FeedbackIntensity::weak}},
          start),
      Admission::held);
  EXPECT_EQ(inbox.trigger_intensity(), FeedbackIntensity::weak);
  EXPECT_FALSE(inbox.take_triggers(seat(1)));
  const auto weaker = inbox.take_triggers(seat(2));
  ASSERT_TRUE(weaker);
  EXPECT_EQ(weaker->intensity, FeedbackIntensity::weak);
  EXPECT_EQ(weaker->effects.left.mode, 0x21);
  const auto primary = inbox.take_triggers(seat(0));
  ASSERT_TRUE(primary);
  EXPECT_EQ(primary->intensity, FeedbackIntensity::weak);
  EXPECT_EQ(primary->effects.left.mode, 0);
  EXPECT_EQ(primary->effects.right.mode, 0);

  EXPECT_EQ(inbox.accept({seat(0), triggers()}, start), Admission::held);
  const auto replaced = inbox.take_triggers(seat(0));
  ASSERT_TRUE(replaced);
  EXPECT_EQ(replaced->effects.left.mode, 0x21);
}

TEST_F(FeedbackInboxPending, StartsWithNothingPending) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  EXPECT_EQ(inbox.pending(seat(1)), Pending{});
  EXPECT_FALSE(inbox.pending(seat(1)).any());
}

TEST_F(FeedbackInboxPending, CountsBlocksTriggersAndRumblePerSeat) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  EXPECT_EQ(haptics(inbox, 512, 0x11, 1), Admission::held);
  EXPECT_EQ(haptics(inbox, 100, 0x11, 2), Admission::held);
  EXPECT_EQ(inbox.accept({seat(1), triggers()}, start), Admission::held);
  EXPECT_EQ(inbox.accept({seat(1), controllers::Rumble{9, 8}}, start),
            Admission::held);
  EXPECT_EQ(inbox.pending(seat(1)), (Pending{2, true, true}));
  EXPECT_TRUE(inbox.pending(seat(1)).any());
  EXPECT_EQ(inbox.pending(seat(2)), (Pending{0, false, false}));
  EXPECT_EQ(inbox.statistics().haptic_bytes_buffered, 612);
  EXPECT_EQ(inbox.statistics().rumble_packets, 1);
  EXPECT_EQ(inbox.statistics().trigger_packets, 1);
}

TEST_F(FeedbackInboxPending, ClearingHapticsKeepsTheTriggersAndRumble) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  EXPECT_EQ(haptics(inbox, 512, 0x11, 1), Admission::held);
  EXPECT_EQ(haptics(inbox, 100, 0x11, 2), Admission::held);
  EXPECT_EQ(inbox.accept({seat(1), triggers()}, start), Admission::held);
  EXPECT_EQ(inbox.accept({seat(1), controllers::Rumble{9, 8}}, start),
            Admission::held);

  inbox.clear_haptics(seat(1));
  EXPECT_EQ(inbox.pending(seat(1)), (Pending{0, true, true}));
  EXPECT_EQ(inbox.statistics().haptic_bytes_buffered, 100);
  EXPECT_FALSE(inbox.take_haptic_block(seat(1), start));
  const auto effects = inbox.take_triggers(seat(1));
  ASSERT_TRUE(effects);
  EXPECT_EQ(effects->effects.left.mode, 0x21);
  EXPECT_EQ(inbox.take_rumble(seat(1)), (controllers::Rumble{9, 8}));
  EXPECT_FALSE(inbox.take_rumble(seat(1)));
  EXPECT_FALSE(inbox.pending(seat(1)).any());
}

TEST_F(FeedbackInboxPending, KeepsTakenTriggersForAnIntensityChange) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  EXPECT_EQ(inbox.accept({seat(1), triggers()}, start), Admission::held);
  EXPECT_TRUE(inbox.take_triggers(seat(1)));
  EXPECT_EQ(
      inbox.accept(
          {seat(0), controllers::TriggerIntensity{FeedbackIntensity::medium}},
          start),
      Admission::held);
  const auto again = inbox.take_triggers(seat(1));
  ASSERT_TRUE(again);
  EXPECT_EQ(again->effects.left.mode, 0x21);
}

TEST_F(FeedbackInboxTriggers, DoesNotHoldFactsWithNothingToWaitFor) {
  Inbox inbox{LiveProviders::threading(), LiveProviders::steady()};
  EXPECT_EQ(inbox.accept({seat(1), controllers::MotionReset{}}, start),
            Admission::not_held);
  EXPECT_EQ(inbox.accept({seat(1), controllers::Lightbar{{1, 2, 3}}}, start),
            Admission::not_held);
  EXPECT_EQ(inbox.accept({seat(1), controllers::PlayerLeds{2}}, start),
            Admission::not_held);
  EXPECT_EQ(
      inbox.accept({seat(1), controllers::SeatConfirmed{{1, 2, 3}}}, start),
      Admission::not_held);
  EXPECT_FALSE(inbox.pending(seat(1)).any());
  EXPECT_EQ(inbox.statistics(), Inbox::Statistics{});
}

}  // namespace
}  // namespace play_test
