#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/play/adapters/links/internet/remote_control_route_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace play_test {
namespace {

namespace platform = libchiaking::platform;
using libchiaking::play::RouteFailure;
using libchiaking::psn::rest::ConsoleFamily;

class RemoteControlRouteGuard : public remote_route_rig::RouteRig {
 protected:
  void SetUp() override { ASSERT_NO_FATAL_FAILURE(open()); }

  auto establish(platform::PlatformFactory& factory, std::uint64_t offer_req_id,
                 platform::StopToken stop, InternetStart::Deadline deadline,
                 ConsoleFamily family = ConsoleFamily::ps5) {
    return InternetStart::establish_control_route(
        factory, session, push, *started, secrets, offer_req_id, 2,
        std::move(stop), deadline, false, family);
  }
};

TEST_F(RemoteControlRouteGuard, RefusesAZeroOfferRequestId) {
  auto invalid = establish(without_http, 0, {}, 10000);
  ASSERT_FALSE(invalid);
  EXPECT_TRUE(invalid.error().is<RouteFailure::InvalidState>());
}

TEST_F(RemoteControlRouteGuard, NeedsAnHttpProviderForAPs5) {
  auto missing = establish(without_http, 1, {}, 10000);
  ASSERT_FALSE(missing);
  EXPECT_TRUE(missing.error().is<RouteFailure::NoHttpProvider>());
}

TEST_F(RemoteControlRouteGuard, NeedsAnHttpProviderForAPs4) {
  auto ps4_missing = establish(without_http, 1, {}, 10000, ConsoleFamily::ps4);
  ASSERT_FALSE(ps4_missing);
  EXPECT_TRUE(ps4_missing.error().is<RouteFailure::NoHttpProvider>());
}

TEST_F(RemoteControlRouteGuard, RefusesAnUnknownConsoleFamily) {
  auto invalid_family =
      establish(without_http, 1, {}, 10000, static_cast<ConsoleFamily>(0xff));
  ASSERT_FALSE(invalid_family);
  EXPECT_TRUE(invalid_family.error().is<RouteFailure::InvalidState>());
}

TEST_F(RemoteControlRouteGuard, StopsWhenTheStartIsAlreadyCancelled) {
  auto cancelled = establish(
      with_http, 1,
      platform::StopToken{std::make_shared<test_support::Stopped>()}, 10000);
  ASSERT_FALSE(cancelled);
  EXPECT_TRUE(cancelled.error().is<RouteFailure::Cancelled>());
}

TEST_F(RemoteControlRouteGuard, TimesOutWhenTheDeadlineHasPassed) {
  clock.now_us = 10000;
  auto expired = establish(with_http, 1, {}, 10000);
  ASSERT_FALSE(expired);
  EXPECT_TRUE(expired.error().is<RouteFailure::TimedOut>());
}

TEST_F(RemoteControlRouteGuard, RefusesASessionWhoseReleaseWasAttempted) {
  clock.now_us = 10000;
  static_cast<void>(started->opened.lease().release({}));
  EXPECT_TRUE(started->opened.lease().release_attempted());
  auto after_release = establish(with_http, 1, {}, 20000);
  ASSERT_FALSE(after_release);
  EXPECT_TRUE(after_release.error().is<RouteFailure::InvalidState>());
}

}  // namespace
}  // namespace play_test
