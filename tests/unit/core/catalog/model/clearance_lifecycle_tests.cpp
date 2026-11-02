#include "src/core/catalog/model/cloud_stream/clearance_lifecycle.hpp"

#include <gtest/gtest.h>

namespace {

using libchiaking::catalog::cloud_stream::ClearanceLifecycle;
using libchiaking::catalog::cloud_stream::ClearanceStage;

TEST(ClearanceLifecycleTable, RunsTheChecksInOrder) {
  ClearanceLifecycle lifecycle;
  EXPECT_EQ(lifecycle.stage(), ClearanceStage::standing);
  lifecycle.pass();
  EXPECT_EQ(lifecycle.stage(), ClearanceStage::cloud_streaming);
  lifecycle.pass();
  EXPECT_EQ(lifecycle.stage(), ClearanceStage::access_route);
  lifecycle.pass();
  EXPECT_EQ(lifecycle.stage(), ClearanceStage::entitlement);
  lifecycle.pass();
  EXPECT_EQ(lifecycle.stage(), ClearanceStage::selected_title);
  lifecycle.pass();
  EXPECT_EQ(lifecycle.stage(), ClearanceStage::exclusions);
  lifecycle.pass();
  EXPECT_EQ(lifecycle.stage(), ClearanceStage::release);
  lifecycle.pass();
  EXPECT_EQ(lifecycle.stage(), ClearanceStage::release);
}

TEST(ClearanceLifecycleTable, RechecksOwnershipOnlyFromTheAccessRoute) {
  ClearanceLifecycle lifecycle;
  lifecycle.need_ownership();
  EXPECT_EQ(lifecycle.stage(), ClearanceStage::standing);
  lifecycle.pass();
  lifecycle.pass();
  lifecycle.need_ownership();
  EXPECT_EQ(lifecycle.stage(), ClearanceStage::ownership);
  lifecycle.pass();
  EXPECT_EQ(lifecycle.stage(), ClearanceStage::entitlement);
}

}  // namespace
