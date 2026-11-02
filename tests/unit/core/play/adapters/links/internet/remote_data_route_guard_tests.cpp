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

class RemoteDataRouteGuard : public remote_route_rig::RouteRig {
 protected:
  void SetUp() override { ASSERT_NO_FATAL_FAILURE(open()); }

  auto establish(platform::PlatformFactory& factory, std::uint64_t offer_req_id,
                 std::uint64_t accept_req_id, platform::StopToken stop,
                 InternetStart::Deadline deadline) {
    return InternetStart::establish_data_route(
        factory, session, push, *started, secrets, offer_req_id, accept_req_id,
        std::move(stop), deadline, false);
  }
};

TEST_F(RemoteDataRouteGuard, RefusesAZeroOfferRequestId) {
  auto invalid = establish(without_http, 0, 4, {}, 10000);
  ASSERT_FALSE(invalid);
  EXPECT_TRUE(invalid.error().is<RouteFailure::InvalidState>());
}

TEST_F(RemoteDataRouteGuard, RefusesTheSameIdForOfferAndAccept) {
  auto duplicate_ids = establish(without_http, 3, 3, {}, 10000);
  ASSERT_FALSE(duplicate_ids);
  EXPECT_TRUE(duplicate_ids.error().is<RouteFailure::InvalidState>());
}

TEST_F(RemoteDataRouteGuard, NeedsAnHttpProvider) {
  auto missing = establish(without_http, 3, 4, {}, 10000);
  ASSERT_FALSE(missing);
  EXPECT_TRUE(missing.error().is<RouteFailure::NoHttpProvider>());
}

TEST_F(RemoteDataRouteGuard, StopsWhenTheStartIsAlreadyCancelled) {
  auto cancelled = establish(
      with_http, 3, 4,
      platform::StopToken{std::make_shared<test_support::Stopped>()}, 10000);
  ASSERT_FALSE(cancelled);
  EXPECT_TRUE(cancelled.error().is<RouteFailure::Cancelled>());
}

TEST_F(RemoteDataRouteGuard, TimesOutWhenTheDeadlineHasPassed) {
  clock.now_us = 10000;
  auto expired = establish(with_http, 3, 4, {}, 10000);
  ASSERT_FALSE(expired);
  EXPECT_TRUE(expired.error().is<RouteFailure::TimedOut>());
}

TEST_F(RemoteDataRouteGuard, RefusesASessionWhoseReleaseWasAttempted) {
  clock.now_us = 10000;
  static_cast<void>(started->opened.lease().release({}));
  EXPECT_TRUE(started->opened.lease().release_attempted());
  auto after_release = establish(with_http, 3, 4, {}, 20000);
  ASSERT_FALSE(after_release);
  EXPECT_TRUE(after_release.error().is<RouteFailure::InvalidState>());
}

}  // namespace
}  // namespace play_test
