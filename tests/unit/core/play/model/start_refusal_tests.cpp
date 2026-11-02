#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

namespace play_test {
namespace {

namespace consoles = libchiaking::consoles;
using libchiaking::play::LaunchRefused;
using libchiaking::play::StartRefusal;
using Kind = StartRefusal::Kind;

TEST(StartRefusal, NamesTheRefusalsWithoutPayload) {
  EXPECT_EQ(StartRefusal::busy().kind(), Kind::busy);
  EXPECT_FALSE(StartRefusal::busy().launch());
  EXPECT_EQ(StartRefusal::not_paired().kind(), Kind::not_paired);
  EXPECT_EQ(StartRefusal::remote_play_disabled().kind(),
            Kind::remote_play_disabled);
  EXPECT_EQ(StartRefusal::sign_in_expired().kind(), Kind::sign_in_expired);
  EXPECT_EQ(StartRefusal::needs_psn_sign_in().kind(), Kind::needs_psn_sign_in);
  EXPECT_EQ(StartRefusal::invalid_preferences().kind(),
            Kind::invalid_preferences);
}

TEST(StartRefusal, SaysWhyTheConsoleWasUnreachable) {
  const auto unreachable =
      StartRefusal::console_unreachable(consoles::Unreachable::timed_out);
  EXPECT_EQ(unreachable.kind(), Kind::console_unreachable);
  EXPECT_EQ(unreachable.unreachable(), consoles::Unreachable::timed_out);
  EXPECT_FALSE(unreachable.launch());
}

TEST(StartRefusal, CarriesTheRefusedLaunch) {
  const auto refused =
      StartRefusal::launch_refused(LaunchRefused::title_not_found());
  EXPECT_EQ(refused.kind(), Kind::launch_refused);
  ASSERT_TRUE(refused.launch());
  EXPECT_EQ(refused.launch()->kind(), LaunchRefused::Kind::title_not_found);
  EXPECT_FALSE(refused.unreachable());
}

}  // namespace
}  // namespace play_test
