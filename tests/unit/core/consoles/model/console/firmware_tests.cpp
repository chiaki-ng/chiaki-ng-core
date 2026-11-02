#include "src/core/net/protocol/remote_play/registered_console.hpp"
#include "tests/support/enum_printing.hpp"

#include <libchiaking/consoles_types.hpp>

#include <gtest/gtest.h>

namespace consoles_test {
namespace {

using libchiaking::consoles::Firmware;
using libchiaking::consoles::Generation;
using libchiaking::net::protocol::remote_play::RegistrationTarget;
using Family = Firmware::Family;

TEST(Firmware, NewestFollowsTheGeneration) {
  EXPECT_EQ(Firmware::newest(Generation::ps5).family(), Family::ps5_1);
  EXPECT_EQ(Firmware::newest(Generation::ps4).family(), Family::ps4_10);
}

TEST(Firmware, EachFamilyBelongsToOneGeneration) {
  EXPECT_EQ(Firmware{Family::ps5_1}.generation(), Generation::ps5);
  EXPECT_EQ(Firmware{Family::ps4_8}.generation(), Generation::ps4);
  EXPECT_EQ(Firmware{Family::ps4_9}.generation(), Generation::ps4);
  EXPECT_EQ(Firmware{Family::ps4_10}.generation(), Generation::ps4);
}

TEST(Firmware, FollowsThePs4SystemVersion) {
  EXPECT_EQ(Firmware::from_system_version(Generation::ps4, "06720001").family(),
            Family::ps4_8);
  EXPECT_EQ(Firmware::from_system_version(Generation::ps4, "07020001").family(),
            Family::ps4_9);
  EXPECT_EQ(Firmware::from_system_version(Generation::ps4, "08000000").family(),
            Family::ps4_10);
  EXPECT_EQ(Firmware::from_system_version(Generation::ps4, "12000000").family(),
            Family::ps4_10);
}

TEST(Firmware, TakesTheNewestPs4ForAMissingOrUnreadableVersion) {
  EXPECT_EQ(Firmware::from_system_version(Generation::ps4, "").family(),
            Family::ps4_10);
  EXPECT_EQ(Firmware::from_system_version(Generation::ps4, "junk").family(),
            Family::ps4_10);
}

TEST(Firmware, IsAlwaysTheFirstPs5Family) {
  EXPECT_EQ(Firmware::from_system_version(Generation::ps5, "04000000").family(),
            Family::ps5_1);
  EXPECT_EQ(Firmware::from_system_version(Generation::ps5, "").family(),
            Family::ps5_1);
}

TEST(Firmware, RoundTripsEachKnownRegistrationTarget) {
  for (const auto target :
       {RegistrationTarget::ps4_8, RegistrationTarget::ps4_9,
        RegistrationTarget::ps4_10, RegistrationTarget::ps5_1}) {
    const auto firmware = Firmware::from(target);
    ASSERT_TRUE(firmware);
    EXPECT_EQ(firmware->target(), target);
  }
}

TEST(Firmware, HasNoFamilyForAnUnknownPs4) {
  EXPECT_FALSE(Firmware::from(RegistrationTarget::ps4_unknown));
}

}  // namespace
}  // namespace consoles_test
