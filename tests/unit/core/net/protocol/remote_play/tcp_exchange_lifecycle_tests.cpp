#include "src/core/net/protocol/remote_play/tcp_exchange.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <variant>

namespace remote_play_test {
namespace {

namespace lifecycle =
    libchiaking::net::protocol::remote_play::tcp_exchange_lifecycle;
using State = lifecycle::State;

TEST(TcpExchangeLifecycle, ReopensOnceAfterAHeaderTimeout) {
  lifecycle::Lifecycle retrying{true};
  EXPECT_FALSE(retrying.process(lifecycle::Connected{0}));
  EXPECT_TRUE(retrying.process(lifecycle::Open{}));
  EXPECT_TRUE(retrying.process(lifecycle::Opened{100, true}));
  EXPECT_EQ(retrying.state(), State::sending);
  EXPECT_TRUE(retrying.process(lifecycle::RequestDelivered{1'000}));
  EXPECT_FALSE(retrying.overdue(5'000'999));
  EXPECT_TRUE(retrying.overdue(5'001'000));
  EXPECT_TRUE(retrying.process(lifecycle::DeadlinePassed{}));
  const auto reopen = retrying.take_effect();
  ASSERT_TRUE(reopen);
  EXPECT_TRUE(std::holds_alternative<lifecycle::Reopen>(*reopen));
  EXPECT_EQ(retrying.state(), State::opening);
  EXPECT_TRUE(retrying.header_retried());
}

TEST(TcpExchangeLifecycle, ADroppedRetryFallsBackToTheConnectDeadline) {
  lifecycle::Lifecycle retrying{true};
  ASSERT_TRUE(retrying.process(lifecycle::Open{}));
  ASSERT_TRUE(retrying.process(lifecycle::Opened{100, true}));
  ASSERT_TRUE(retrying.process(lifecycle::RequestDelivered{1'000}));
  ASSERT_TRUE(retrying.process(lifecycle::DeadlinePassed{}));
  ASSERT_TRUE(retrying.take_effect());
  EXPECT_TRUE(retrying.process(lifecycle::Opened{2'000, false}));
  EXPECT_TRUE(retrying.process(lifecycle::Connected{3'000}));
  EXPECT_TRUE(retrying.process(lifecycle::ConnectionDropped{}));
  EXPECT_EQ(retrying.state(), State::connecting);
  EXPECT_FALSE(retrying.overdue(5'001'999));
  EXPECT_TRUE(retrying.overdue(5'002'000));
  EXPECT_TRUE(retrying.process(lifecycle::Failed{}));
  EXPECT_EQ(retrying.state(), State::finished);
  EXPECT_TRUE(retrying.failed_while_connecting());
  EXPECT_TRUE(retrying.process(lifecycle::Cancel{}));
}

TEST(TcpExchangeLifecycle, GivesUpAtTheHeaderTimeoutWhenItMayNotRetry) {
  lifecycle::Lifecycle once{false};
  EXPECT_TRUE(once.process(lifecycle::Open{}));
  EXPECT_TRUE(once.process(lifecycle::Opened{0, true}));
  EXPECT_TRUE(once.process(lifecycle::RequestDelivered{0}));
  EXPECT_TRUE(once.process(lifecycle::DeadlinePassed{}));
  const auto give_up = once.take_effect();
  ASSERT_TRUE(give_up);
  EXPECT_TRUE(std::holds_alternative<lifecycle::GiveUp>(*give_up));
  EXPECT_EQ(once.state(), State::receiving);
  EXPECT_FALSE(once.header_retried());
}

}  // namespace
}  // namespace remote_play_test
