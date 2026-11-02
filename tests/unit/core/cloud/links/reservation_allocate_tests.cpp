#include "src/core/psn/gk/measurement.hpp"
#include "tests/unit/core/cloud/links/scripted_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cloud_test {
namespace {

using Reason = gk::Error::Reason;
using Stage = gk::Error::Stage;
using Texts = ScriptedReservation::Texts;
using FailureStage = libchiaking::net::protocol::remote_play::FailureStage;
using Result = libchiaking::Expected<gk::Allocation, gk::Error>;

class Allocated {
 public:
  Result result;
  std::string key;
};

class ReservationAllocate : public ::testing::Test {
 protected:
  static constexpr std::string_view slot_body =
      R"({"launchSlot":{"publicIp":"1.2.3.4","port":8299},"handshakeKey":"hk","launchSpecification":"spec","sessionId":"sid"})";

  static auto queued(int poll_frequency, int wait_time_estimate = 0) -> Step {
    return Step::reply(
        "{\"queued\":true,\"pollFrequency\":" + std::to_string(poll_frequency) +
        ",\"waitTimeEstimate\":" + std::to_string(wait_time_estimate) + "}");
  }

  static auto backend_failure() -> Step {
    return Step::failure(platform::HttpError::backend_failure);
  }

  static auto slot() -> Step { return Step::reply(slot_body); }

  static auto datacenter() -> gk::Ping {
    gk::Ping ping;
    ping.name = "dc";
    ping.rtt_ms = 20;
    ping.selected_port = 2053;
    return ping;
  }

  static auto reconnecting(std::size_t count) -> Texts {
    Texts texts{"Allocating Streaming Slot - Step 10 of 10"};
    texts.insert(texts.end(), count,
                 "Allocating streaming slot - reconnecting...");
    return texts;
  }

  static auto failed(const Result& result, Reason reason, std::uint32_t detail)
      -> testing::AssertionResult {
    return GkRig::failed(result, Stage::allocate, reason, detail);
  }

  auto allocate_session(play::LaunchObserver* observer = nullptr,
                        SessionSeed seed = ScriptedReservation::initial_seed(),
                        bool stopped = false) -> Allocated {
    auto stop = ScriptedReservation::fresh_stop();
    SessionHarness harness{provider, waiter, std::move(seed), stop, observer};
    stop->stopped = stopped;
    auto result = harness.session().allocate(datacenter(), 10000);
    return {std::move(result),
            std::string{harness.session().config_key().view()}};
  }

  auto harness(play::LaunchObserver* observer) -> SessionHarness {
    return {provider, waiter, ScriptedReservation::initial_seed(),
            ScriptedReservation::fresh_stop(), observer};
  }

  ScriptedGk provider{"/allocate"};
  test_support::FakeWaiter waiter = GkRig::recording_waiter();
  StageProgress observed{FailureStage::cloud_allocate};
};

TEST_F(ReservationAllocate, GivesUpAfterFiveNetworkRetries) {
  provider.steps.assign(6, backend_failure());
  auto session = harness(&observed);
  const auto allocated = session.session().allocate(datacenter(), 10000);
  EXPECT_TRUE(failed(allocated, Reason::network_exhausted, 2));
  ASSERT_FALSE(allocated);
  EXPECT_TRUE(allocated.error().is_transport());
  EXPECT_EQ(provider.calls, 6U);
  EXPECT_EQ(waiter.waits, ScriptedReservation::repeated(5, 5000));
  EXPECT_EQ(observed.texts, reconnecting(5));
}

TEST_F(ReservationAllocate, StartsItsNetworkRetriesAfreshOnTheNextAllocation) {
  provider.steps.assign(6, backend_failure());
  auto session = harness(&observed);
  EXPECT_FALSE(session.session().allocate(datacenter(), 10000));
  provider.steps.insert(provider.steps.end(), 5, backend_failure());
  provider.steps.push_back(slot());
  const auto allocated = session.session().allocate(datacenter(), 10000);
  ASSERT_TRUE(allocated);
  EXPECT_EQ(allocated.value().handshake_key.view(), "hk");
  EXPECT_EQ(provider.calls, 12U);
  EXPECT_EQ(waiter.waits, ScriptedReservation::repeated(10, 5000));
  auto texts = reconnecting(5);
  const auto again = reconnecting(5);
  texts.insert(texts.end(), again.begin(), again.end());
  EXPECT_EQ(observed.texts, texts);
}

TEST_F(ReservationAllocate, ShortensAQueueWaitByTheTimeAlreadySpent) {
  provider.steps.assign(5, backend_failure());
  provider.steps.push_back(queued(1000));
  provider.steps.push_back(slot());
  EXPECT_TRUE(allocate_session().result);
  EXPECT_EQ(provider.calls, 7U);
  EXPECT_EQ(waiter.waits,
            (std::vector<int>{5000, 5000, 5000, 5000, 5000, 275000}));
}

TEST_F(ReservationAllocate, TimesOutUnreachableAfterALongQueueWait) {
  provider.steps = {queued(300), backend_failure()};
  auto session = harness(&observed);
  EXPECT_TRUE(failed(session.session().allocate(datacenter(), 10000),
                     Reason::timed_out_unreachable, 3));
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_EQ(waiter.waits, std::vector<int>{300000});
  provider.steps.push_back(queued(300));
  provider.steps.push_back(slot());
  EXPECT_TRUE(session.session().allocate(datacenter(), 10000));
  EXPECT_EQ(provider.calls, 4U);
  EXPECT_EQ(waiter.waits, (std::vector<int>{300000, 300000}));
  EXPECT_EQ(observed.texts, (Texts{
                                "Allocating Streaming Slot - Step 10 of 10",
                                "Allocating streaming slot - Attempt 1",
                                "Allocating Streaming Slot - Step 10 of 10",
                                "Allocating streaming slot - Attempt 1",
                            }));
}

TEST_F(ReservationAllocate, AllocatesAfterALongQueueWaitAndAgain) {
  provider.steps = {queued(300), slot()};
  auto session = harness(&observed);
  const auto allocated = session.session().allocate(datacenter(), 10000);
  ASSERT_TRUE(allocated);
  EXPECT_EQ(allocated.value().server_ip, "1.2.3.4");
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_EQ(waiter.waits, std::vector<int>{300000});
  provider.steps.push_back(queued(300));
  provider.steps.push_back(slot());
  EXPECT_TRUE(session.session().allocate(datacenter(), 10000));
  EXPECT_EQ(provider.calls, 4U);
  EXPECT_EQ(waiter.waits, (std::vector<int>{300000, 300000}));
  EXPECT_EQ(observed.texts, (Texts{
                                "Allocating Streaming Slot - Step 10 of 10",
                                "Allocating streaming slot - Attempt 1",
                                "Allocating Streaming Slot - Step 10 of 10",
                                "Allocating streaming slot - Attempt 1",
                            }));
}

TEST_F(ReservationAllocate, TimesOutAtTheEstimatedQueueDeadline) {
  provider.steps = {queued(10, 449), queued(10, 1), queued(5000, 1),
                    queued(10, 1)};
  const auto outcome = allocate_session(&observed);
  EXPECT_TRUE(failed(outcome.result, Reason::timed_out, 3));
  EXPECT_EQ(provider.calls, 4U);
  EXPECT_EQ(waiter.waits, (std::vector<int>{10000, 10000, 878000}));
  EXPECT_EQ(observed.texts, (Texts{
                                "Allocating Streaming Slot - Step 10 of 10",
                                "Allocating streaming slot - Attempt 1",
                                "Allocating streaming slot - Attempt 2",
                                "Allocating streaming slot - Attempt 3",
                            }));
}

TEST_F(ReservationAllocate, CapsTheQueueDeadlineAtFifteenMinutes) {
  provider.steps = {queued(5000, 450), queued(10, 450)};
  EXPECT_TRUE(failed(allocate_session().result, Reason::timed_out, 3));
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_EQ(waiter.waits, std::vector<int>{900000});
}

TEST_F(ReservationAllocate, CapsAHugeEstimate) {
  provider.steps = {
      queued(5000, std::numeric_limits<std::int32_t>::max()),
      queued(10, std::numeric_limits<std::int32_t>::max()),
  };
  EXPECT_TRUE(failed(allocate_session().result, Reason::timed_out, 3));
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_EQ(waiter.waits, std::vector<int>{900000});
}

TEST_F(ReservationAllocate, PollsEveryFifteenSecondsWithoutAFrequency) {
  provider.steps = {queued(0), queued(-5), queued(1000), queued(1000)};
  EXPECT_TRUE(failed(allocate_session().result, Reason::timed_out, 3));
  EXPECT_EQ(provider.calls, 4U);
  EXPECT_EQ(waiter.waits, (std::vector<int>{15000, 15000, 270000}));
}

TEST_F(ReservationAllocate, ResetsTheNetworkRetriesAfterAReply) {
  provider.steps.assign(5, backend_failure());
  provider.steps.push_back(queued(10));
  provider.steps.insert(provider.steps.end(), 6, backend_failure());
  EXPECT_TRUE(
      failed(allocate_session(&observed).result, Reason::network_exhausted, 2));
  EXPECT_EQ(provider.calls, 12U);
  auto delays = ScriptedReservation::repeated(5, 5000);
  delays.push_back(10000);
  delays.insert(delays.end(), 5, 5000);
  EXPECT_EQ(waiter.waits, delays);
  auto texts = reconnecting(5);
  texts.emplace_back("Allocating streaming slot - Attempt 1");
  texts.insert(texts.end(), 5, "Allocating streaming slot - reconnecting...");
  EXPECT_EQ(observed.texts, texts);
}

TEST_F(ReservationAllocate, WaitsOutADataMigration) {
  provider.steps = {Step::reply(R"({"dataMigration":true,"pollFrequency":10})"),
                    slot()};
  EXPECT_TRUE(allocate_session(&observed).result);
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_EQ(waiter.waits, std::vector<int>{10000});
  EXPECT_EQ(observed.texts, (Texts{
                                "Allocating Streaming Slot - Step 10 of 10",
                                "Migrating data (0%) - Attempt 1",
                            }));
}

TEST_F(ReservationAllocate, StopsWhenNeitherQueuedNorMigrating) {
  provider.steps = {Step::reply(
      R"({"queued":false,"dataMigration":false,"pollFrequency":10})")};
  EXPECT_TRUE(
      failed(allocate_session().result, Reason::missing_launch_slot, 4));
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_TRUE(waiter.waits.empty());
}

TEST_F(ReservationAllocate, IsCancelledWithTheRequestAndCanAllocateAgain) {
  provider.steps = {Step::failure(platform::HttpError::cancelled)};
  auto session = harness(&observed);
  const auto allocated = session.session().allocate(datacenter(), 10000);
  EXPECT_TRUE(failed(allocated, Reason::cancelled, 1));
  ASSERT_FALSE(allocated);
  EXPECT_TRUE(allocated.error().is_cancelled());
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_TRUE(waiter.waits.empty());
  provider.steps.push_back(queued(3));
  provider.steps.push_back(slot());
  EXPECT_TRUE(session.session().allocate(datacenter(), 10000));
  EXPECT_EQ(provider.calls, 3U);
  EXPECT_EQ(waiter.waits, std::vector<int>{3000});
  EXPECT_EQ(observed.texts, (Texts{
                                "Allocating Streaming Slot - Step 10 of 10",
                                "Allocating Streaming Slot - Step 10 of 10",
                                "Allocating streaming slot - Attempt 1",
                            }));
}

TEST_F(ReservationAllocate, StopsAtARejection) {
  provider.steps = {Step::rejection(500)};
  EXPECT_TRUE(failed(allocate_session().result, Reason::rejected, 4));
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_TRUE(waiter.waits.empty());
}

TEST_F(ReservationAllocate, StopsAtARotatedKeyThatBreaksAHeader) {
  provider.steps = {Step::reply(R"({"queued":true})", "bad\r\nkey")};
  EXPECT_TRUE(
      failed(allocate_session().result, Reason::invalid_session_key, 4));
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_TRUE(waiter.waits.empty());
}

TEST_F(ReservationAllocate, StopsAtAMalformedReply) {
  provider.steps = {Step::reply("bad json")};
  EXPECT_TRUE(failed(allocate_session().result, Reason::malformed, 4));
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_TRUE(waiter.waits.empty());
}

TEST_F(ReservationAllocate, StopsWithoutALaunchSlot) {
  provider.steps = {Step::reply(R"({"handshakeKey":"hk"})")};
  EXPECT_TRUE(
      failed(allocate_session().result, Reason::missing_launch_slot, 4));
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_TRUE(waiter.waits.empty());
}

TEST_F(ReservationAllocate, PollsWithEachRotatedKey) {
  provider.steps = {
      Step::reply(R"({"queued":true,"pollFrequency":7,"waitTimeEstimate":10})",
                  "rotated-a"),
      Step::reply(R"({"dataMigration":true,"pollFrequency":4})", "rotated-b"),
      Step::reply(slot_body, "rotated-c"),
  };
  const auto outcome = allocate_session();
  ASSERT_TRUE(outcome.result);
  EXPECT_EQ(outcome.result.value().handshake_key.view(), "hk");
  EXPECT_EQ(outcome.result.value().server_ip, "1.2.3.4");
  EXPECT_EQ(outcome.key, "rotated-c");
  EXPECT_EQ(provider.seen_keys,
            (Texts{"initial-key", "rotated-a", "rotated-b"}));
  EXPECT_EQ(waiter.waits, (std::vector<int>{7000, 4000}));
}

TEST_F(ReservationAllocate, ReportsTheQueuePositionAndEstimate) {
  provider.steps = {
      Step::reply(
          R"({"queued":true,"pollFrequency":1,"waitTimeEstimate":245,"displayQueuePosition":4,"queuePosition":9})"),
      Step::reply(
          R"({"queued":true,"pollFrequency":1,"waitTimeEstimate":125,"queuePosition":3})"),
      Step::reply(R"({"queued":true,"pollFrequency":1})"),
      backend_failure(),
      Step::reply(
          R"({"queued":true,"dataMigration":true,"pollFrequency":1,"dataMigrationPercentageComplete":42,"queuePosition":2})"),
      slot(),
  };
  EXPECT_TRUE(allocate_session(&observed).result);
  EXPECT_EQ(observed.texts,
            (Texts{
                "Allocating Streaming Slot - Step 10 of 10",
                "Allocating streaming slot - Queue position: 4 - Attempt 1",
                "Allocating streaming slot - Queue position: 3 - Attempt 2",
                "Allocating streaming slot - Attempt 3",
                "Allocating streaming slot - reconnecting...",
                "Migrating data (42%) - Attempt 4",
            }));
  EXPECT_EQ(
      observed.queue_estimates,
      (std::vector<std::optional<std::uint32_t>>{245, 125, std::nullopt}));
}

TEST_F(ReservationAllocate, KeepsTheInitialKeyThroughNetworkRetries) {
  provider.steps.assign(6, backend_failure());
  const auto outcome = allocate_session();
  EXPECT_TRUE(failed(outcome.result, Reason::network_exhausted, 2));
  EXPECT_EQ(provider.calls, 6U);
  EXPECT_EQ(waiter.waits, ScriptedReservation::repeated(5, 5000));
  EXPECT_EQ(outcome.key, "initial-key");
}

TEST_F(ReservationAllocate, TimesOutUnreachableWithTheRotatedKey) {
  provider.steps = {
      Step::reply(R"({"queued":true,"pollFrequency":300})", "rotated-a"),
      backend_failure(),
  };
  EXPECT_TRUE(
      failed(allocate_session().result, Reason::timed_out_unreachable, 3));
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_EQ(waiter.waits, std::vector<int>{300000});
  EXPECT_EQ(provider.seen_keys, (Texts{"initial-key", "rotated-a"}));
}

TEST_F(ReservationAllocate, IsCancelledWhenTheWaitIsRefused) {
  provider.steps = {
      Step::reply(R"({"queued":true,"pollFrequency":3})", "rotated")};
  waiter.answer = false;
  EXPECT_TRUE(failed(allocate_session().result, Reason::cancelled, 1));
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_EQ(waiter.waits, std::vector<int>{3000});
}

TEST_F(ReservationAllocate, SendsNothingOnceStopped) {
  EXPECT_TRUE(failed(
      allocate_session(nullptr, ScriptedReservation::initial_seed(), true)
          .result,
      Reason::cancelled, 1));
  EXPECT_EQ(provider.calls, 0U);
  EXPECT_TRUE(waiter.waits.empty());
}

TEST_F(ReservationAllocate, RefusesAnInvalidSessionIdBeforeSending) {
  auto seed = ScriptedReservation::initial_seed();
  seed.session_id = "bad\nsession";
  seed.until = SessionSeed::Until::started;
  const auto outcome = allocate_session(nullptr, std::move(seed));
  EXPECT_TRUE(failed(outcome.result, Reason::invalid_session_id, 0));
  ASSERT_FALSE(outcome.result);
  EXPECT_TRUE(outcome.result.error().is_invalid_request());
  EXPECT_EQ(provider.calls, 0U);
  EXPECT_TRUE(waiter.waits.empty());
}

}  // namespace
}  // namespace cloud_test
