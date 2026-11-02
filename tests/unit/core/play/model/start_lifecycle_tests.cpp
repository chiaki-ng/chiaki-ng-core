#include "src/core/play/model/start_lifecycle.hpp"

#include <gtest/gtest.h>

namespace {

using libchiaking::play::StartLifecycle;

TEST(StartLifecycleTable, StartsWithoutAWakeAndReconnectsUntilItGivesUp) {
  StartLifecycle lifecycle;
  EXPECT_FALSE(lifecycle.reconnecting());
  EXPECT_FALSE(lifecycle.finished());
  EXPECT_FALSE(lifecycle.woken());
  lifecycle.reconnect();
  lifecycle.reconnect();
  EXPECT_TRUE(lifecycle.reconnecting());
  lifecycle.wake();
  lifecycle.wake();
  EXPECT_TRUE(lifecycle.woken());
  lifecycle.give_up();
  EXPECT_TRUE(lifecycle.finished());
  EXPECT_FALSE(lifecycle.reconnecting());
  lifecycle.reconnect();
  EXPECT_TRUE(lifecycle.finished());
}

}  // namespace
