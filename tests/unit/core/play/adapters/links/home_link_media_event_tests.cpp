#include "src/core/net/protocol/remote_play/stream/public_media_event.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/home_link_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <utility>
#include <vector>

namespace play_test {
namespace {

using namespace home_links;

using remote_play::EncodedMediaKind;
using remote_play::PublicMediaEvent;
namespace feedback_event = remote_play::feedback_event;

class HomeLinkMediaEvents : public HomeLinkTest {};

TEST_F(HomeLinkMediaEvents, CopiesRumbleBytes) {
  const auto rumble =
      PublicMediaEvent::from(takion::client_event::Rumble{3, 0xab, 0xcd, 0x47});
  EXPECT_EQ(rumble.kind, EncodedMediaKind::rumble);
  EXPECT_EQ(rumble.bytes,
            (std::vector<std::byte>{std::byte{3}, std::byte{0xab},
                                    std::byte{0xcd}, std::byte{0x47}}));
}

TEST_F(HomeLinkMediaEvents, OwnsTheTriggerEffectBytes) {
  takion::client_event::TriggerEffects input;
  input.player_index = 2;
  input.type_left = 5;
  input.type_right = 6;
  input.left.fill(1);
  input.right.fill(2);
  const auto triggers = PublicMediaEvent::from(input);
  EXPECT_EQ(triggers.kind, EncodedMediaKind::trigger_effects);
  ASSERT_EQ(triggers.bytes.size(), 25U);
  EXPECT_EQ(triggers.bytes[0], std::byte{2});
  EXPECT_EQ(triggers.bytes[1], std::byte{5});
  EXPECT_EQ(triggers.bytes[2], std::byte{6});
  EXPECT_EQ(triggers.bytes[3], std::byte{0});
  EXPECT_EQ(triggers.bytes[4], std::byte{0});
  for (std::size_t index = 5; index < 15; ++index) {
    SCOPED_TRACE(index);
    EXPECT_EQ(triggers.bytes[index], std::byte{1});
  }
  for (std::size_t index = 15; index < 25; ++index) {
    SCOPED_TRACE(index);
    EXPECT_EQ(triggers.bytes[index], std::byte{2});
  }
  input.left.fill(9);
  EXPECT_EQ(triggers.bytes[5], std::byte{1});
}

TEST_F(HomeLinkMediaEvents, CarriesPadAndLightFeedback) {
  const auto pad = PublicMediaEvent::from_feedback(
      feedback_event::PadConfirmed{2, {4, 5, 6}});
  EXPECT_EQ(pad.kind, EncodedMediaKind::pad_confirmed);
  EXPECT_EQ(pad.bytes, (std::vector<std::byte>{std::byte{2}, std::byte{4},
                                               std::byte{5}, std::byte{6}}));
  const auto led =
      PublicMediaEvent::from_feedback(feedback_event::LedColor{{1, 2, 3}});
  EXPECT_EQ(led.kind, EncodedMediaKind::led_color);
  EXPECT_EQ(led.bytes.size(), 3U);
  const auto reset =
      PublicMediaEvent::from_feedback(feedback_event::MotionReset{});
  EXPECT_EQ(reset.kind, EncodedMediaKind::motion_reset);
  EXPECT_TRUE(reset.bytes.empty());
  const auto haptic =
      PublicMediaEvent::from_feedback(feedback_event::HapticIntensity{2});
  EXPECT_EQ(haptic.kind, EncodedMediaKind::haptic_intensity);
  EXPECT_EQ(haptic.bytes, std::vector<std::byte>{std::byte{2}});
  const auto trigger =
      PublicMediaEvent::from_feedback(feedback_event::TriggerIntensity{3});
  EXPECT_EQ(trigger.kind, EncodedMediaKind::trigger_intensity);
  EXPECT_EQ(trigger.bytes, std::vector<std::byte>{std::byte{3}});
  const auto player =
      PublicMediaEvent::from_feedback(feedback_event::PlayerIndex{1});
  EXPECT_EQ(player.kind, EncodedMediaKind::player_index);
  EXPECT_EQ(player.bytes, std::vector<std::byte>{std::byte{1}});
}

TEST_F(HomeLinkMediaEvents, ReportsVideoFecFailures) {
  const auto fec = PublicMediaEvent::from_feedback(
      feedback_event::VideoFecFailure{0x1234, true});
  EXPECT_EQ(fec.kind, EncodedMediaKind::video_fec_failure);
  EXPECT_EQ(fec.frame_index, 0x1234);
  EXPECT_EQ(fec.bytes, std::vector<std::byte>{std::byte{1}});
  EXPECT_FALSE(fec.frame_recovered);
  EXPECT_EQ(fec.frames_lost, 0);
  EXPECT_FALSE(fec.profile_header);
  EXPECT_EQ(fec.pad_index, 0);
  const auto fec_silent = PublicMediaEvent::from_feedback(
      feedback_event::VideoFecFailure{0xffff, false});
  EXPECT_EQ(fec_silent.kind, EncodedMediaKind::video_fec_failure);
  EXPECT_EQ(fec_silent.frame_index, 0xffff);
  EXPECT_EQ(fec_silent.bytes, std::vector<std::byte>{std::byte{0}});
}

TEST_F(HomeLinkMediaEvents, DispatchesQueuedFeedback) {
  remote_play::ActiveFeedbackEvent queued_fec =
      feedback_event::VideoFecFailure{7, true};
  const auto dispatched_fec =
      PublicMediaEvent::from_feedback(std::move(queued_fec));
  EXPECT_EQ(dispatched_fec.kind, EncodedMediaKind::video_fec_failure);
  EXPECT_EQ(dispatched_fec.frame_index, 7);
  EXPECT_EQ(dispatched_fec.bytes, std::vector<std::byte>{std::byte{1}});
  remote_play::ActiveFeedbackEvent queued =
      takion::client_event::Rumble{3, 7, 8};
  const auto dispatched = PublicMediaEvent::from_feedback(std::move(queued));
  EXPECT_EQ(dispatched.kind, EncodedMediaKind::rumble);
  EXPECT_EQ(dispatched.bytes,
            (std::vector<std::byte>{std::byte{3}, std::byte{7}, std::byte{8},
                                    std::byte{0}}));
}

}  // namespace
}  // namespace play_test
