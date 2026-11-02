#include "src/core/users/server_time.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <chrono>

namespace users_test {
namespace {

using libchiaking::users::ServerTime;
using libchiaking::users::SignedInUsers;

TEST(ServerTime, ReadsTheServerClock) {
  test_support::FakeClock monotonic;
  ServerTime trusted{monotonic};
  EXPECT_FALSE(trusted.now());
  trusted.server_clock().record(1'791'273'600'000);
  EXPECT_EQ(trusted.now(),
            ServerTime::Time{std::chrono::milliseconds{1'791'273'600'000}});
  monotonic.advance_ms(3'600'000);
  EXPECT_EQ(trusted.now(),
            ServerTime::Time{std::chrono::milliseconds{1'791'277'200'000}});
}

TEST(ServerTime, IsSharedByTheUsersOfOneServiceOnly) {
  SignedInRig rig;
  const auto first = rig.signed_in.as(rig.saved(7, "NPSSO-A", "app-a"));
  const auto second = rig.signed_in.as(rig.saved(8, "NPSSO-B", "app-b"));
  EXPECT_EQ(&ServerTime::of(*first), &ServerTime::of(*second));
  SignedInRig other;
  const auto elsewhere = other.signed_in.as(other.saved(7, "NPSSO-A", "app-a"));
  EXPECT_NE(&ServerTime::of(*first), &ServerTime::of(*elsewhere));

  EXPECT_FALSE(rig.signed_in.users.server_time());
  EXPECT_FALSE(other.signed_in.users.server_time());
  ServerTime::of(*first).server_clock().record(1'791'273'600'000);
  EXPECT_EQ(rig.signed_in.users.server_time(),
            SignedInUsers::ServerInstant{
                std::chrono::milliseconds{1'791'273'600'000}});
  EXPECT_FALSE(other.signed_in.users.server_time());
}

}  // namespace
}  // namespace users_test
