#include "tests/unit/core/cloud/links/scripted_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cloud_test {
namespace {

using Reason = gk::Error::Reason;
using Stage = gk::Error::Stage;
using Texts = ScriptedReservation::Texts;
using FailureStage = libchiaking::net::protocol::remote_play::FailureStage;
namespace progress = libchiaking::progress;

class Locked {
 public:
  libchiaking::Expected<void, gk::Error> result;
  std::string key;
  std::optional<std::string> lock_key;
};

class ReservationLock : public ::testing::Test {
 protected:
  static auto poll(bool acquired, int poll_seconds,
                   std::string_view rotated_key = {}) -> Step {
    return Step::reply(
        std::string{"{\"lockAcquired\":"} + (acquired ? "true" : "false") +
            ",\"pollFrequency\":" + std::to_string(poll_seconds) + "}",
        rotated_key);
  }

  static auto backend_failure() -> Step {
    return Step::failure(platform::HttpError::backend_failure);
  }

  static auto reconnecting(std::size_t count) -> Texts {
    Texts texts{"Locking Session - Step 7 of 10"};
    texts.insert(texts.end(), count, "Locking Session - reconnecting...");
    return texts;
  }

  static auto failed(const libchiaking::Expected<void, gk::Error>& result,
                     Reason reason, std::uint32_t detail)
      -> testing::AssertionResult {
    return GkRig::failed(result, Stage::lock, reason, detail);
  }

  auto lock_session(play::LaunchObserver* observer = nullptr,
                    SessionSeed seed = ScriptedReservation::initial_seed(),
                    bool stopped = false) -> Locked {
    auto stop = ScriptedReservation::fresh_stop();
    SessionHarness harness{provider, waiter, std::move(seed), stop, observer};
    stop->stopped = stopped;
    auto result = harness.session().lock();
    const auto& session = harness.session();
    std::optional<std::string> lock_key;
    if (session.lock_key()) lock_key.emplace(session.lock_key()->view());
    return {std::move(result), std::string{session.config_key().view()},
            std::move(lock_key)};
  }

  auto harness(play::LaunchObserver* observer) -> SessionHarness {
    return {provider, waiter, ScriptedReservation::initial_seed(),
            ScriptedReservation::fresh_stop(), observer};
  }

  static auto waits_for_the_old_session(
      const std::vector<play::LaunchProgress>& typed)
      -> testing::AssertionResult {
    for (std::uint32_t attempt = 1; attempt < typed.size(); ++attempt) {
      const auto& waiting = typed[attempt];
      if (waiting.activity() != play::LaunchActivity::reserving_server)
        return testing::AssertionFailure()
               << "attempt " << attempt << " is not reserving the server";
      if (waiting.attempt() != attempt)
        return testing::AssertionFailure()
               << "attempt " << attempt << " reports another attempt";
      if (waiting.wait() !=
          (progress::Wait{progress::WaitReason::previous_session_closing,
                          std::chrono::seconds{15}}))
        return testing::AssertionFailure()
               << "attempt " << attempt
               << " does not wait 15 seconds for the old session";
      if (!waiting.status().is_waiting())
        return testing::AssertionFailure()
               << "attempt " << attempt << " is not waiting";
      if (waiting.status().attempt != attempt)
        return testing::AssertionFailure()
               << "attempt " << attempt << " has another status attempt";
    }
    return testing::AssertionSuccess();
  }

  static auto waits_for_the_network(
      const std::vector<play::LaunchProgress>& typed)
      -> testing::AssertionResult {
    for (std::size_t index = 1; index < typed.size(); ++index) {
      const auto& waiting = typed[index];
      if (waiting.activity() != play::LaunchActivity::reserving_server)
        return testing::AssertionFailure()
               << "report " << index << " is not reserving the server";
      if (waiting.attempt())
        return testing::AssertionFailure()
               << "report " << index << " counts an attempt";
      if (waiting.wait() !=
          (progress::Wait{progress::WaitReason::network_unreachable,
                          std::chrono::seconds{5}}))
        return testing::AssertionFailure()
               << "report " << index
               << " does not wait 5 seconds for the network";
    }
    return testing::AssertionSuccess();
  }

  ScriptedGk provider{"/lock?forceLogout=true"};
  test_support::FakeWaiter waiter = GkRig::recording_waiter();
  StageProgress observed{FailureStage::cloud_lock};
};

TEST_F(ReservationLock, GivesUpAfterTwelvePollsForTheOldSession) {
  provider.steps.assign(13, poll(false, 15));
  Texts texts{"Locking Session - Step 7 of 10"};
  for (int attempt = 1; attempt <= 12; ++attempt)
    texts.push_back("Closing old session - Attempt " + std::to_string(attempt));
  auto session = harness(&observed);
  EXPECT_TRUE(failed(session.session().lock(), Reason::lock_unavailable, 5));
  EXPECT_FALSE(session.session().lock_key());
  EXPECT_EQ(provider.calls, 13U);
  EXPECT_EQ(waiter.waits, ScriptedReservation::repeated(12, 15000));
  EXPECT_EQ(observed.texts, texts);
  ASSERT_EQ(observed.typed.size(), std::size_t{13});
  EXPECT_FALSE(observed.typed.front().wait());
  EXPECT_EQ(observed.typed.front().step(), (progress::Position{7, 10}));
  EXPECT_TRUE(waits_for_the_old_session(observed.typed));
}

TEST_F(ReservationLock, StartsItsAttemptsAfreshOnTheNextLock) {
  provider.steps.assign(13, poll(false, 15));
  auto session = harness(&observed);
  EXPECT_FALSE(session.session().lock());
  provider.steps.push_back(poll(false, 15));
  provider.steps.push_back(poll(true, 1));
  Texts texts = observed.texts;
  EXPECT_TRUE(session.session().lock());
  EXPECT_TRUE(session.session().lock_key());
  EXPECT_EQ(provider.calls, 15U);
  EXPECT_EQ(waiter.waits, ScriptedReservation::repeated(13, 15000));
  texts.emplace_back("Locking Session - Step 7 of 10");
  texts.emplace_back("Closing old session - Attempt 1");
  EXPECT_EQ(observed.texts, texts);
}

TEST_F(ReservationLock, GivesUpAfterFiveNetworkRetries) {
  provider.steps.assign(6, backend_failure());
  auto session = harness(&observed);
  EXPECT_TRUE(failed(session.session().lock(), Reason::transport, 5));
  EXPECT_FALSE(session.session().lock_key());
  EXPECT_EQ(provider.calls, 6U);
  EXPECT_EQ(waiter.waits, ScriptedReservation::repeated(5, 5000));
  EXPECT_EQ(observed.texts, reconnecting(5));
  ASSERT_EQ(observed.typed.size(), std::size_t{6});
  EXPECT_FALSE(observed.typed.front().wait());
  EXPECT_TRUE(waits_for_the_network(observed.typed));
}

TEST_F(ReservationLock, StartsItsNetworkRetriesAfreshOnTheNextLock) {
  provider.steps.assign(6, backend_failure());
  auto session = harness(&observed);
  EXPECT_FALSE(session.session().lock());
  provider.steps.insert(provider.steps.end(), 5, backend_failure());
  provider.steps.push_back(poll(true, 1));
  EXPECT_TRUE(session.session().lock());
  EXPECT_TRUE(session.session().lock_key());
  EXPECT_EQ(provider.calls, 12U);
  EXPECT_EQ(waiter.waits, ScriptedReservation::repeated(10, 5000));
  auto texts = reconnecting(5);
  const auto again = reconnecting(5);
  texts.insert(texts.end(), again.begin(), again.end());
  EXPECT_EQ(observed.texts, texts);
}

TEST_F(ReservationLock, ResetsTheNetworkRetriesAfterAReply) {
  provider.steps.assign(5, backend_failure());
  provider.steps.push_back(poll(false, 30));
  provider.steps.insert(provider.steps.end(), 6, backend_failure());
  const auto outcome = lock_session(&observed);
  EXPECT_TRUE(failed(outcome.result, Reason::transport, 5));
  EXPECT_EQ(provider.calls, 12U);
  auto delays = ScriptedReservation::repeated(5, 5000);
  delays.push_back(30000);
  delays.insert(delays.end(), 5, 5000);
  EXPECT_EQ(waiter.waits, delays);
  auto texts = reconnecting(5);
  texts.emplace_back("Closing old session - Attempt 1");
  texts.insert(texts.end(), 5, "Locking Session - reconnecting...");
  EXPECT_EQ(observed.texts, texts);
}

TEST_F(ReservationLock, WaitsTheDefaultForANonPositivePollAndKeepsALongOne) {
  provider.steps = {poll(false, 0), poll(false, -3), poll(false, 1200),
                    poll(true, 1)};
  const auto outcome = lock_session();
  EXPECT_TRUE(outcome.result);
  ASSERT_TRUE(outcome.lock_key);
  EXPECT_EQ(*outcome.lock_key, "initial-key");
  EXPECT_EQ(provider.calls, 4U);
  EXPECT_EQ(waiter.waits, (std::vector<int>{10000, 10000, 1200000}));
}

TEST_F(ReservationLock, IsCancelledWithTheRequestAndCanLockAgain) {
  provider.steps = {Step::failure(platform::HttpError::cancelled)};
  auto session = harness(&observed);
  const auto locked = session.session().lock();
  EXPECT_TRUE(failed(locked, Reason::cancelled, 1));
  ASSERT_FALSE(locked);
  EXPECT_TRUE(locked.error().is_cancelled());
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_TRUE(waiter.waits.empty());
  provider.steps.push_back(poll(false, 4));
  provider.steps.push_back(poll(true, 1));
  EXPECT_TRUE(session.session().lock());
  EXPECT_TRUE(session.session().lock_key());
  EXPECT_EQ(provider.calls, 3U);
  EXPECT_EQ(waiter.waits, std::vector<int>{4000});
  EXPECT_EQ(observed.texts, (Texts{
                                "Locking Session - Step 7 of 10",
                                "Locking Session - Step 7 of 10",
                                "Closing old session - Attempt 1",
                            }));
}

TEST_F(ReservationLock, StopsAtARejection) {
  provider.steps = {Step::rejection(403)};
  EXPECT_TRUE(failed(lock_session().result, Reason::rejected, 5));
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_TRUE(waiter.waits.empty());
}

TEST_F(ReservationLock, StopsAtARotatedKeyThatBreaksAHeader) {
  provider.steps = {poll(true, 5, "bad\r\nkey")};
  const auto outcome = lock_session();
  EXPECT_TRUE(failed(outcome.result, Reason::invalid_session_key, 5));
  EXPECT_FALSE(outcome.lock_key);
  EXPECT_EQ(outcome.key, "initial-key");
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_TRUE(waiter.waits.empty());
}

TEST_F(ReservationLock, PollsWithEachRotatedKey) {
  provider.steps = {poll(false, 7, "rotated-a"), poll(true, 7, "rotated-b")};
  const auto outcome = lock_session(&observed);
  EXPECT_TRUE(outcome.result);
  EXPECT_EQ(provider.seen_keys, (Texts{"initial-key", "rotated-a"}));
  EXPECT_EQ(waiter.waits, std::vector<int>{7000});
  EXPECT_EQ(outcome.key, "rotated-b");
  ASSERT_TRUE(outcome.lock_key);
  EXPECT_EQ(*outcome.lock_key, "rotated-b");
  EXPECT_EQ(observed.texts, (Texts{
                                "Locking Session - Step 7 of 10",
                                "Closing old session - Attempt 1",
                            }));
}

TEST_F(ReservationLock, KeepsARotatedKeyAcrossANetworkRetry) {
  provider.steps = {poll(false, 7, "rotated"), backend_failure(),
                    poll(true, 7)};
  const auto outcome = lock_session(&observed);
  EXPECT_TRUE(outcome.result);
  EXPECT_EQ(provider.seen_keys, (Texts{"initial-key", "rotated", "rotated"}));
  EXPECT_EQ(waiter.waits, (std::vector<int>{7000, 5000}));
  EXPECT_EQ(outcome.key, "rotated");
  EXPECT_EQ(observed.texts, (Texts{
                                "Locking Session - Step 7 of 10",
                                "Closing old session - Attempt 1",
                                "Locking Session - reconnecting...",
                            }));
}

TEST_F(ReservationLock, KeepsTheInitialKeyThroughNetworkRetries) {
  provider.steps.assign(6, backend_failure());
  const auto outcome = lock_session(&observed);
  EXPECT_TRUE(failed(outcome.result, Reason::transport, 5));
  EXPECT_EQ(provider.seen_keys, Texts(6, "initial-key"));
  EXPECT_EQ(waiter.waits, ScriptedReservation::repeated(5, 5000));
  EXPECT_EQ(observed.texts, reconnecting(5));
  EXPECT_EQ(outcome.key, "initial-key");
  EXPECT_FALSE(outcome.lock_key);
}

TEST_F(ReservationLock, CountsNetworkRetriesAfreshAfterAPoll) {
  provider.steps = {backend_failure(), poll(false, 7)};
  provider.steps.insert(provider.steps.end(), 6, backend_failure());
  const auto outcome = lock_session();
  EXPECT_TRUE(failed(outcome.result, Reason::transport, 5));
  EXPECT_EQ(provider.calls, 8U);
  EXPECT_EQ(waiter.waits,
            (std::vector<int>{5000, 7000, 5000, 5000, 5000, 5000, 5000}));
  EXPECT_EQ(outcome.key, "initial-key");
}

TEST_F(ReservationLock, LocksWithTheLastRotatedKey) {
  provider.steps = {
      Step::reply(R"({"lockAcquired":false,"pollFrequency":7})", "rotated-a"),
      Step::reply(R"({"lockAcquired":true,"pollFrequency":5})", "rotated-b"),
  };
  const auto outcome = lock_session();
  EXPECT_TRUE(outcome.result);
  ASSERT_TRUE(outcome.lock_key);
  EXPECT_EQ(*outcome.lock_key, "rotated-b");
  EXPECT_EQ(outcome.key, "rotated-b");
  EXPECT_EQ(provider.calls, 2U);
  ASSERT_EQ(provider.seen_keys.size(), std::size_t{2});
  EXPECT_EQ(provider.seen_keys[0], "initial-key");
  EXPECT_EQ(provider.seen_keys[1], "rotated-a");
  EXPECT_EQ(waiter.waits, std::vector<int>{7000});
}

TEST_F(ReservationLock, RetriesANetworkFailureEveryFiveSeconds) {
  provider.steps.assign(6, backend_failure());
  const auto outcome = lock_session();
  EXPECT_TRUE(failed(outcome.result, Reason::transport, 5));
  EXPECT_EQ(provider.calls, 6U);
  EXPECT_EQ(waiter.waits, ScriptedReservation::repeated(5, 5000));
  EXPECT_EQ(outcome.key, "initial-key");
}

TEST_F(ReservationLock, GivesUpAfterTwelveShortPolls) {
  provider.steps.assign(
      13, Step::reply(R"({"lockAcquired":false,"pollFrequency":1})"));
  const auto outcome = lock_session();
  EXPECT_TRUE(failed(outcome.result, Reason::lock_unavailable, 5));
  EXPECT_EQ(provider.calls, 13U);
  EXPECT_EQ(waiter.waits, ScriptedReservation::repeated(12, 1000));
}

TEST_F(ReservationLock, NamesTheOldSessionFromTheEventHeader) {
  provider.steps = {
      Step::reply(R"({"lockAcquired":false,"pollFrequency":2})", "",
                  R"({"name":"Oren Session","id":1})"),
      Step::reply(R"({"lockAcquired":false,"pollFrequency":2})", "",
                  R"({"name":")" + std::string(80, 'n') + R"("})"),
      Step::reply(R"({"lockAcquired":false,"pollFrequency":2})", "",
                  "not json"),
      Step::reply(R"({"lockAcquired":true,"pollFrequency":2})"),
  };
  EXPECT_TRUE(lock_session(&observed).result);
  EXPECT_EQ(
      observed.texts,
      (Texts{
          "Locking Session - Step 7 of 10",
          "Closing old session (Oren Session) - Attempt 1",
          "Closing old session (" + std::string(63, 'n') + ") - Attempt 2",
          "Closing old session - Attempt 3",
      }));
}

TEST_F(ReservationLock, ReportsEachNetworkRetryAsReconnecting) {
  provider.steps.assign(6, backend_failure());
  EXPECT_FALSE(lock_session(&observed).result);
  EXPECT_EQ(observed.texts, reconnecting(5));
}

TEST_F(ReservationLock, RefusesAnInvalidSessionIdBeforeSending) {
  auto seed = ScriptedReservation::initial_seed();
  seed.session_id = "bad\nsession";
  seed.until = SessionSeed::Until::started;
  const auto outcome = lock_session(nullptr, std::move(seed));
  EXPECT_TRUE(failed(outcome.result, Reason::invalid_session_id, 0));
  ASSERT_FALSE(outcome.result);
  EXPECT_TRUE(outcome.result.error().is_invalid_request());
  EXPECT_EQ(provider.calls, 0U);
  EXPECT_TRUE(waiter.waits.empty());
}

TEST_F(ReservationLock, IsCancelledWhenTheWaitIsRefused) {
  provider.steps = {
      Step::reply(R"({"lockAcquired":false,"pollFrequency":3})", "rotated")};
  waiter.answer = false;
  EXPECT_TRUE(failed(lock_session().result, Reason::cancelled, 1));
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_EQ(waiter.waits.size(), std::size_t{1});
}

TEST_F(ReservationLock, SendsNothingOnceStopped) {
  EXPECT_TRUE(failed(
      lock_session(nullptr, ScriptedReservation::initial_seed(), true).result,
      Reason::cancelled, 1));
  EXPECT_EQ(provider.calls, 0U);
  EXPECT_TRUE(waiter.waits.empty());
}

}  // namespace
}  // namespace cloud_test
