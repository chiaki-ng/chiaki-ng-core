#include "src/core/net/kakusan/link_lifecycle.hpp"

#include <gtest/gtest.h>

#include <variant>

namespace {

using namespace libchiaking::net::kakusan;

TEST(LinkLifecycleTable, StopsOnceAndReportsOnlyTheFirstFailure) {
  LinkLifecycle lifecycle;
  EXPECT_TRUE(lifecycle.running());
  lifecycle.stop(Error::timeout);
  lifecycle.stop(Error::no_path);
  EXPECT_FALSE(lifecycle.running());
  EXPECT_FALSE(lifecycle.closed());
  const auto events = lifecycle.events();
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(std::get<link_event::Failed>(events[0]).error, Error::timeout);
  lifecycle.begin_pump();
  EXPECT_TRUE(lifecycle.events().empty());
}

TEST(LinkLifecycleTable, ClosesFromAnyRunStateAndStaysClosed) {
  LinkLifecycle lifecycle;
  lifecycle.stop(Error::timeout);
  lifecycle.close();
  lifecycle.close();
  EXPECT_TRUE(lifecycle.closed());
  EXPECT_FALSE(lifecycle.running());
  lifecycle.begin_pump();
  lifecycle.stop(Error::timeout);
  EXPECT_TRUE(lifecycle.events().empty());
}

TEST(LinkLifecycleTable, DeafensOnceWhileStillRunning) {
  LinkLifecycle lifecycle;
  lifecycle.emit(link_event::Nominated{});
  lifecycle.deafen(Error::codec);
  lifecycle.deafen(Error::protocol);
  EXPECT_TRUE(lifecycle.deaf());
  EXPECT_TRUE(lifecycle.running());
  const auto events = lifecycle.events();
  ASSERT_EQ(events.size(), 2U);
  EXPECT_TRUE(std::holds_alternative<link_event::Nominated>(events[0]));
  EXPECT_EQ(std::get<link_event::Malformed>(events[1]).error, Error::codec);
}

}  // namespace
