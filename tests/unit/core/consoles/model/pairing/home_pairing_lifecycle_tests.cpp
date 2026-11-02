#include "src/core/consoles/model/pairing/home_pairing_lifecycle.hpp"
#include "tests/support/enum_printing.hpp"

#include <boost/sml.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <variant>

namespace consoles_test {
namespace {

using libchiaking::consoles::home_pairing_lifecycle::BeginConnect;
using libchiaking::consoles::home_pairing_lifecycle::BeginKnock;
using libchiaking::consoles::home_pairing_lifecycle::Cancel;
using libchiaking::consoles::home_pairing_lifecycle::Cleanup;
using libchiaking::consoles::home_pairing_lifecycle::Closed;
using libchiaking::consoles::home_pairing_lifecycle::Connected;
using libchiaking::consoles::home_pairing_lifecycle::ConnectionPending;
using libchiaking::consoles::home_pairing_lifecycle::DelayElapsed;
using libchiaking::consoles::home_pairing_lifecycle::Error;
using libchiaking::consoles::home_pairing_lifecycle::Exchange;
using libchiaking::consoles::home_pairing_lifecycle::Failed;
using libchiaking::consoles::home_pairing_lifecycle::HeaderSent;
using libchiaking::consoles::home_pairing_lifecycle::KnockAnswered;
using libchiaking::consoles::home_pairing_lifecycle::Lifecycle;
using libchiaking::consoles::home_pairing_lifecycle::LifecycleContext;
using libchiaking::consoles::home_pairing_lifecycle::PublishCancelled;
using libchiaking::consoles::home_pairing_lifecycle::PublishFailed;
using libchiaking::consoles::home_pairing_lifecycle::PublishPaired;
using libchiaking::consoles::home_pairing_lifecycle::ReplyDecoded;
using libchiaking::consoles::home_pairing_lifecycle::ReplyHeaderAccepted;
using libchiaking::consoles::home_pairing_lifecycle::RequestSent;
using libchiaking::consoles::home_pairing_lifecycle::Start;
using libchiaking::consoles::home_pairing_lifecycle::Step;
using libchiaking::consoles::home_pairing_lifecycle::WaitAfterKnock;

class HomePairingLifecycle : public ::testing::Test {
 protected:
  static constexpr std::uint64_t attempt{7};
  static constexpr std::uint64_t stale_id{99};

  void advance(int phase) {
    ASSERT_TRUE(machine.process(Start{attempt}));
    if (phase >= 2) {
      ASSERT_TRUE(machine.process(KnockAnswered{attempt}));
    }
    if (phase >= 3) {
      ASSERT_TRUE(machine.process(DelayElapsed{attempt}));
    }
    if (phase >= 4) {
      ASSERT_TRUE(machine.process(Connected{attempt}));
    }
    if (phase >= 5) {
      ASSERT_TRUE(machine.process(HeaderSent{attempt}));
      ASSERT_TRUE(machine.process(RequestSent{attempt}));
    }
  }

  void advance_full() {
    ASSERT_NO_FATAL_FAILURE(advance(5));
    ASSERT_TRUE(machine.process(ReplyDecoded{attempt}));
  }

  template <class Effect>
  auto count() const -> std::size_t {
    std::size_t found = 0;
    for (const auto& effect : context.effects)
      if (std::holds_alternative<Effect>(effect)) ++found;
    return found;
  }

  template <class Effect>
  auto last() const -> const Effect* {
    if (context.effects.empty()) return nullptr;
    return std::get_if<Effect>(&context.effects.back());
  }

  LifecycleContext context;
  Lifecycle machine{context};
};

class HomePairingLifecyclePhase : public HomePairingLifecycle,
                                  public ::testing::WithParamInterface<int> {};

TEST_F(HomePairingLifecycle, StartsIdle) {
  EXPECT_EQ(machine.state(), Step::idle);
}

TEST_F(HomePairingLifecycle, WalksEveryStepOfAnExchange) {
  ASSERT_TRUE(machine.process(Start{attempt}));
  EXPECT_EQ(machine.state(), Step::knocking);
  ASSERT_TRUE(machine.process(KnockAnswered{attempt}));
  EXPECT_EQ(machine.state(), Step::settling);
  ASSERT_TRUE(machine.process(DelayElapsed{attempt}));
  EXPECT_EQ(machine.state(), Step::connecting);
  ASSERT_TRUE(machine.process(Connected{attempt}));
  EXPECT_EQ(machine.state(), Step::sending);
  EXPECT_EQ(machine.exchange(), Exchange::sending_header);
  ASSERT_TRUE(machine.process(ConnectionPending{attempt}));
  EXPECT_EQ(machine.state(), Step::connecting);
  EXPECT_EQ(machine.exchange(), Exchange::connecting);
  ASSERT_TRUE(machine.process(Connected{attempt}));
  EXPECT_FALSE(machine.process(RequestSent{attempt}));
  ASSERT_TRUE(machine.process(HeaderSent{attempt}));
  EXPECT_EQ(machine.state(), Step::sending);
  EXPECT_EQ(machine.exchange(), Exchange::sending_payload);
  ASSERT_TRUE(machine.process(RequestSent{attempt}));
  EXPECT_EQ(machine.state(), Step::receiving);
  EXPECT_EQ(machine.exchange(), Exchange::awaiting_reply_header);
  ASSERT_TRUE(machine.process(ReplyHeaderAccepted{attempt}));
  EXPECT_EQ(machine.state(), Step::receiving);
  EXPECT_EQ(machine.exchange(), Exchange::reading_reply_body);
  EXPECT_EQ(context.unexpected_events, 1U);
  ASSERT_EQ(context.effects.size(), 3U);
  EXPECT_TRUE(std::holds_alternative<BeginKnock>(context.effects[0]));
  EXPECT_TRUE(std::holds_alternative<WaitAfterKnock>(context.effects[1]));
  EXPECT_TRUE(std::holds_alternative<BeginConnect>(context.effects[2]));
  const auto* knock = std::get_if<BeginKnock>(&context.effects[0]);
  ASSERT_NE(knock, nullptr);
  EXPECT_EQ(knock->attempt_id, attempt);
  EXPECT_EQ(count<PublishPaired>(), 0U);
  EXPECT_EQ(count<PublishCancelled>(), 0U);
  EXPECT_EQ(count<PublishFailed>(), 0U);
}

TEST_P(HomePairingLifecyclePhase, CancelsThroughACleanup) {
  ASSERT_NO_FATAL_FAILURE(advance(GetParam()));
  const auto pre = context.effects.size();
  ASSERT_TRUE(machine.process(Cancel{attempt}));
  EXPECT_EQ(machine.state(), Step::closing);
  EXPECT_EQ(context.effects.size(), pre + 1);
  EXPECT_NE(last<Cleanup>(), nullptr);
  EXPECT_EQ(count<PublishCancelled>(), 0U);
  ASSERT_TRUE(machine.process(Closed{attempt, true}));
  EXPECT_EQ(machine.state(), Step::cancelled);
  EXPECT_EQ(count<Cleanup>(), 1U);
  EXPECT_EQ(count<PublishCancelled>(), 1U);
  const auto* cancelled = last<PublishCancelled>();
  ASSERT_NE(cancelled, nullptr);
  EXPECT_EQ(cancelled->attempt_id, attempt);
  EXPECT_EQ(count<PublishPaired>(), 0U);
  EXPECT_EQ(count<PublishFailed>(), 0U);
}

TEST_P(HomePairingLifecyclePhase, FailsThroughACleanup) {
  ASSERT_NO_FATAL_FAILURE(advance(GetParam()));
  const auto pre = context.effects.size();
  ASSERT_TRUE(machine.process(Failed{attempt, Error::refused}));
  EXPECT_EQ(machine.state(), Step::closing);
  EXPECT_EQ(context.effects.size(), pre + 1);
  EXPECT_NE(last<Cleanup>(), nullptr);
  ASSERT_TRUE(machine.process(Closed{attempt, true}));
  EXPECT_EQ(machine.state(), Step::failed);
  EXPECT_EQ(count<PublishFailed>(), 1U);
  const auto* failed = last<PublishFailed>();
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->error, Error::refused);
  EXPECT_EQ(count<PublishPaired>(), 0U);
  EXPECT_EQ(count<PublishCancelled>(), 0U);
}

TEST_P(HomePairingLifecyclePhase, IgnoresStaleEvents) {
  ASSERT_NO_FATAL_FAILURE(advance(GetParam()));
  const auto before = machine.state();
  const auto size = context.effects.size();
  EXPECT_FALSE(machine.process(Cancel{stale_id}));
  EXPECT_FALSE(machine.process(Failed{stale_id, Error::timeout}));
  EXPECT_FALSE(machine.process(Closed{stale_id, true}));
  EXPECT_EQ(machine.state(), before);
  EXPECT_EQ(context.effects.size(), size);
  EXPECT_EQ(context.stale_events, 3U);
}

INSTANTIATE_TEST_SUITE_P(KnockingToReceiving, HomePairingLifecyclePhase,
                         ::testing::Range(1, 6));

TEST_F(HomePairingLifecycle, PublishesAPairingOnceClosed) {
  ASSERT_NO_FATAL_FAILURE(advance_full());
  ASSERT_TRUE(machine.process(Closed{attempt, true}));
  EXPECT_EQ(machine.state(), Step::paired);
  EXPECT_EQ(count<PublishPaired>(), 1U);
  EXPECT_EQ(count<PublishCancelled>(), 0U);
  EXPECT_EQ(count<PublishFailed>(), 0U);
}

TEST_F(HomePairingLifecycle, TurnsAFailedCleanupAfterACancelIntoAFailure) {
  ASSERT_NO_FATAL_FAILURE(advance(3));
  ASSERT_TRUE(machine.process(Cancel{attempt}));
  ASSERT_TRUE(machine.process(Closed{attempt, false}));
  EXPECT_EQ(machine.state(), Step::failed);
  EXPECT_EQ(count<PublishCancelled>(), 0U);
  EXPECT_EQ(count<PublishFailed>(), 1U);
  const auto* failed = last<PublishFailed>();
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->error, Error::cleanup_failure);
}

TEST_F(HomePairingLifecycle, KeepsTheFirstFailureThroughAFailedCleanup) {
  ASSERT_NO_FATAL_FAILURE(advance(2));
  ASSERT_TRUE(machine.process(Failed{attempt, Error::timeout}));
  ASSERT_TRUE(machine.process(Closed{attempt, false}));
  EXPECT_EQ(machine.state(), Step::failed);
  EXPECT_EQ(count<PublishFailed>(), 1U);
  const auto* failed = last<PublishFailed>();
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->error, Error::timeout);
}

TEST_F(HomePairingLifecycle, LetsAFailureOverrideADecodedReplyWhileClosing) {
  ASSERT_NO_FATAL_FAILURE(advance_full());
  const auto pre = context.effects.size();
  EXPECT_FALSE(machine.process(Cancel{attempt}));
  EXPECT_EQ(machine.state(), Step::closing);
  EXPECT_EQ(context.effects.size(), pre);
  EXPECT_EQ(context.unexpected_events, 1U);
  ASSERT_TRUE(machine.process(Failed{attempt, Error::network_failure}));
  EXPECT_EQ(machine.state(), Step::closing);
  EXPECT_EQ(context.effects.size(), pre);
  EXPECT_EQ(count<Cleanup>(), 1U);
  ASSERT_TRUE(machine.process(Closed{attempt, true}));
  EXPECT_EQ(machine.state(), Step::failed);
  const auto* failed = last<PublishFailed>();
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->error, Error::network_failure);
}

TEST_F(HomePairingLifecycle, CountsAnEventBeforeTheStartAsStale) {
  EXPECT_FALSE(machine.process(KnockAnswered{attempt}));
  EXPECT_EQ(context.stale_events, 1U);
  EXPECT_TRUE(context.effects.empty());
}

TEST_F(HomePairingLifecycle, RejectsStaleOutOfOrderAndRepeatedStarts) {
  static_cast<void>(machine.process(KnockAnswered{attempt}));
  ASSERT_TRUE(machine.process(Start{attempt}));
  EXPECT_FALSE(machine.process(Connected{stale_id}));
  EXPECT_EQ(context.stale_events, 2U);
  EXPECT_EQ(machine.state(), Step::knocking);
  EXPECT_FALSE(machine.process(Connected{attempt}));
  EXPECT_EQ(context.unexpected_events, 1U);
  EXPECT_EQ(machine.state(), Step::knocking);
  EXPECT_FALSE(machine.process(Start{attempt + 1}));
  EXPECT_EQ(context.unexpected_events, 2U);
  EXPECT_FALSE(machine.process(Start{0}));
  EXPECT_EQ(context.unexpected_events, 3U);
  EXPECT_EQ(count<BeginKnock>(), 1U);
}

TEST_F(HomePairingLifecycle, MakesNoProgressOnceTerminal) {
  ASSERT_NO_FATAL_FAILURE(advance_full());
  ASSERT_TRUE(machine.process(Closed{attempt, true}));
  EXPECT_EQ(machine.state(), Step::paired);
  const auto total = context.effects.size();
  EXPECT_EQ(count<PublishPaired>(), 1U);
  EXPECT_FALSE(machine.process(Closed{attempt, true}));
  EXPECT_EQ(machine.state(), Step::paired);
  EXPECT_EQ(context.effects.size(), total);
  EXPECT_FALSE(machine.process(Start{attempt + 10}));
  EXPECT_EQ(machine.state(), Step::paired);
  EXPECT_EQ(count<BeginKnock>(), 1U);
  EXPECT_EQ(context.effects.size(), total);
}

}  // namespace
}  // namespace consoles_test
