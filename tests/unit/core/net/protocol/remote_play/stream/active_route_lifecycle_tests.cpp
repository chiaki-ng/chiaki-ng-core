#include "src/core/net/protocol/remote_play/stream/active_route_lifecycle.hpp"

#include <gtest/gtest.h>

namespace {

using libchiaking::net::protocol::remote_play::ActiveRouteLifecycle;

TEST(ActiveRouteLifecycleTable, StartsLiveWithTakionAndDroppingMedia) {
  const ActiveRouteLifecycle route;
  EXPECT_FALSE(route.stopped());
  EXPECT_FALSE(route.takion_lost());
  EXPECT_FALSE(route.keeps_media());
}

TEST(ActiveRouteLifecycleTable, EachRegionMovesOnceAndIgnoresRepeats) {
  ActiveRouteLifecycle route;
  route.lose_takion();
  route.lose_takion();
  EXPECT_TRUE(route.takion_lost());
  EXPECT_FALSE(route.stopped());
  route.keep_media();
  route.keep_media();
  EXPECT_TRUE(route.keeps_media());
  route.halt();
  route.halt();
  EXPECT_TRUE(route.stopped());
  EXPECT_TRUE(route.takion_lost());
  EXPECT_TRUE(route.keeps_media());
}

}  // namespace
