#include "src/core/kakusan/adapters/controller_map.hpp"
#include "src/core/net/kakusan/controller_frame.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/controller_map_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace kakusan_test {
namespace {

using namespace libchiaking::kakusan;
using namespace libchiaking::net::kakusan;
using libchiaking::controllers::ControllerButton;
using libchiaking::net::kakusan::Button;

class ControllerMapFeedback : public ControllerMapTest {};

TEST_F(ControllerMapFeedback, MapsRumbleToItsPad) {
  for (std::uint8_t pad = 0; pad < 4; ++pad) {
    const auto mapped = rumble_from_event(rumble(pad, 255, 0));
    ASSERT_TRUE(mapped);
    EXPECT_EQ(*mapped, (SourceRumble{seat(pad), 65535, 0}));
  }
  auto mapped = rumble_from_event(rumble(1, 0, 255));
  ASSERT_TRUE(mapped);
  EXPECT_EQ(*mapped, (SourceRumble{seat(1), 0, 65535}));
  mapped = rumble_from_event(rumble(2, 1, 128));
  ASSERT_TRUE(mapped);
  EXPECT_EQ(*mapped, (SourceRumble{seat(2), 257, 32896}));
  mapped = rumble_from_event(rumble(3, 0, 0));
  ASSERT_TRUE(mapped);
  EXPECT_EQ(*mapped, (SourceRumble{seat(3), 0, 0}));

  for (unsigned value = 0; value <= 255; ++value) {
    mapped = rumble_from_event(rumble(0, static_cast<std::uint8_t>(value),
                                      static_cast<std::uint8_t>(255U - value)));
    ASSERT_TRUE(mapped);
    EXPECT_EQ(mapped->low_frequency, value * 257U);
    EXPECT_EQ(mapped->high_frequency, (255U - value) * 257U);
  }
}

TEST_F(ControllerMapFeedback, FollowsTheConsolesHapticStrength) {
  const SourceRumble full{seat(1), 65535, 32896};
  EXPECT_EQ(ControllerMap::scaled(
                full, libchiaking::controllers::FeedbackIntensity::strong),
            full);
  EXPECT_EQ(ControllerMap::scaled(
                full, libchiaking::controllers::FeedbackIntensity::medium),
            (SourceRumble{seat(1), 32768, 16448}));
  EXPECT_EQ(ControllerMap::scaled(
                full, libchiaking::controllers::FeedbackIntensity::weak),
            (SourceRumble{seat(1), 21627, 10856}));
  EXPECT_EQ(ControllerMap::scaled(
                full, libchiaking::controllers::FeedbackIntensity::off),
            (SourceRumble{seat(1), 0, 0}));
}

TEST_F(ControllerMapFeedback, DropsFeedbackThatIsNotRumble) {
  auto media = rumble(0, 1, 1);
  media.fact = libchiaking::controllers::HapticAudio{};
  EXPECT_FALSE(rumble_from_event(media));
  media.fact = libchiaking::controllers::TriggerEffects{};
  EXPECT_FALSE(rumble_from_event(media));
  media.fact = libchiaking::controllers::Lightbar{};
  EXPECT_FALSE(rumble_from_event(media));
  media.fact = libchiaking::controllers::Rumble{1, 1};
  EXPECT_TRUE(rumble_from_event(media));
}

}  // namespace
}  // namespace kakusan_test
