#include "tests/unit/core/net/protocol/senkusha/client_adapter_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

namespace senkusha_test {
namespace {

class SenkushaClientStart : public SenkushaClientRig {
 protected:
  SenkushaHarness harness;
};

TEST_F(SenkushaClientStart, RefusesAnInvalidConfigurationBeforeAnyIo) {
  ClientConfig config;
  config.profile.bootstrap_version = 8;
  auto subject = client(harness, std::move(config));
  const auto started = subject->start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), Error::unsupported_version);
  EXPECT_TRUE(harness.state().sent.empty());
}

TEST_F(SenkushaClientStart, SendsInitAndReportsConnecting) {
  auto subject = client(harness);
  ASSERT_TRUE(subject->start());
  EXPECT_EQ(harness.state().sent.size(), 1U);
  const auto snapshot = subject->snapshot();
  EXPECT_EQ(snapshot.state, State::connecting_takion);
  EXPECT_EQ(snapshot.attempt_id, std::optional<std::uint64_t>{1});
  EXPECT_TRUE(snapshot.deadline_us.has_value());
  EXPECT_TRUE(subject->pump_once());
}

TEST_F(SenkushaClientStart, CancelsIdempotentlyAndClosesOnce) {
  auto subject = client(harness);
  ASSERT_TRUE(subject->start());
  EXPECT_TRUE(subject->cancel());
  EXPECT_TRUE(subject->cancel());
  EXPECT_EQ(harness.state().close_count, 1U);
  EXPECT_EQ(harness.events.failures(), 1U);
  EXPECT_EQ(subject->snapshot().state, State::failed);
  EXPECT_EQ(subject->snapshot().error, Error::cancelled);
}

TEST_F(SenkushaClientStart, ReportsATakionStartFailure) {
  harness.random.fail = true;
  auto subject = client(harness);
  const auto started = subject->start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), Error::takion_failure);
  EXPECT_EQ(harness.events.failures(), 1U);
  EXPECT_EQ(harness.state().close_count, 1U);
}

TEST_F(SenkushaClientStart, WaitsOutTheConnectTimeoutAfterAFailedFirstInit) {
  harness.state().fail_send = true;
  auto subject = client(harness);
  ASSERT_TRUE(subject->start());
  EXPECT_TRUE(harness.state().sent.empty());
  EXPECT_EQ(harness.state().close_count, 1U);
  EXPECT_TRUE(subject->pump_once());
  EXPECT_EQ(subject->snapshot().state, State::connecting_takion);
  EXPECT_EQ(harness.events.failures(), 0U);
  harness.clock.now_us = 1000 + 30'000'000 - 1;
  EXPECT_TRUE(subject->pump_once());
  EXPECT_EQ(harness.events.failures(), 0U);
  ++harness.clock.now_us;
  const auto pumped = subject->pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), Error::timeout);
  EXPECT_EQ(harness.events.failures(), 1U);
  EXPECT_EQ(harness.state().close_count, 1U);
}

TEST_F(SenkushaClientStart, WaitsOutTheConnectTimeoutWhenTakionIsRefused) {
  auto subject = client(harness);
  ASSERT_TRUE(subject->start());
  for (int attempt = 0; attempt < 3; ++attempt) {
    harness.state().failed_receives = 1;
    EXPECT_TRUE(subject->pump_once()) << "attempt " << attempt;
  }
  EXPECT_EQ(harness.state().sent.size(), 4U);
  EXPECT_EQ(subject->snapshot().state, State::connecting_takion);
  EXPECT_EQ(harness.events.failures(), 0U);
  harness.clock.now_us = 1000 + 30'000'000 - 1;
  EXPECT_TRUE(subject->pump_once());
  EXPECT_EQ(harness.events.failures(), 0U);
  EXPECT_GT(harness.state().sent.size(), 4U);
  ++harness.clock.now_us;
  const auto pumped = subject->pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), Error::timeout);
  EXPECT_EQ(harness.events.failures(), 1U);
}

TEST_F(SenkushaClientStart, FailsOnceWhenItsDeadlinePasses) {
  auto subject = client(harness);
  ASSERT_TRUE(subject->start());
  harness.clock.now_us = 31'000'001;
  const auto pumped = subject->pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), Error::timeout);
  EXPECT_EQ(subject->snapshot().state, State::failed);
  EXPECT_EQ(harness.events.failures(), 1U);
  EXPECT_EQ(harness.state().close_count, 1U);
}

}  // namespace
}  // namespace senkusha_test
