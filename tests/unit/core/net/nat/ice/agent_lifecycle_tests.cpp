#include "src/core/net/nat/ice/agent_lifecycle.hpp"

#include <gtest/gtest.h>

#include <variant>

namespace {

using namespace libchiaking::net::nat::ice;

TEST(IceAgentLifecycle, ConnectsOnlyOnceAfterGathering) {
  AgentLifecycle lifecycle;
  EXPECT_FALSE(lifecycle.connect());
  lifecycle.finish_gathering();
  EXPECT_TRUE(lifecycle.gathered());
  EXPECT_TRUE(lifecycle.connect());
  EXPECT_TRUE(lifecycle.connected());
  EXPECT_TRUE(lifecycle.gathered());
  EXPECT_FALSE(lifecycle.connect());
  lifecycle.finish_gathering();
}

TEST(IceAgentLifecycle, FailsOnceAndReportsIt) {
  AgentLifecycle lifecycle;
  lifecycle.fail(Error::no_path);
  lifecycle.fail(Error::internal);
  EXPECT_TRUE(lifecycle.failed());
  const auto first = lifecycle.take();
  ASSERT_TRUE(first);
  EXPECT_EQ(std::get<event::Failed>(*first).error, Error::no_path);
  EXPECT_FALSE(lifecycle.take());
}

TEST(IceAgentLifecycle, ClosingDropsWhatWasNotTaken) {
  AgentLifecycle lifecycle;
  lifecycle.emit(event::Nominated{});
  lifecycle.close();
  lifecycle.close();
  EXPECT_TRUE(lifecycle.closed());
  EXPECT_FALSE(lifecycle.take());
}

TEST(IceAgentLifecycle, HoldsUntilReleasedAndLosesConsentOnce) {
  AgentLifecycle lifecycle;
  lifecycle.release();
  lifecycle.hold();
  EXPECT_TRUE(lifecycle.holding());
  lifecycle.hold();
  lifecycle.release();
  EXPECT_FALSE(lifecycle.holding());
  lifecycle.expire_consent();
  lifecycle.expire_consent();
  EXPECT_TRUE(lifecycle.consent_expired());
}

}  // namespace
