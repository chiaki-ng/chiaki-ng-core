#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/utils/diagnostics/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace diagnostics_test {
namespace {

using libchiaking::diagnostics::LogQueue;
using libchiaking::platform::LogSeverity;

class LogQueueTest : public ::testing::Test {
 protected:
  auto write_and_read(std::string message) -> std::optional<LogQueue::Record> {
    queue.write({LogSeverity::info, std::move(message)});
    return queue.read();
  }

  const libchiaking::platform::SteadyClock monotonic;
  LogQueue queue{monotonic};
};

TEST_F(LogQueueTest, KeepsAnErrorAndCountsWhatOverflowDropped) {
  for (unsigned i = 0; i < 400; ++i)
    queue.write({LogSeverity::info, "bounded"});
  queue.write({LogSeverity::error, "primary cause"});
  std::optional<LogQueue::Record> record;
  std::size_t info{};
  std::size_t errors{};
  std::uint64_t lost{};
  std::uint64_t prior{};
  std::uint64_t attempt{};
  while ((record = queue.read())) {
    EXPECT_GT(record->sequence(), prior);
    prior = record->sequence();
    if (!attempt) attempt = record->attempt();
    EXPECT_EQ(record->attempt(), attempt);
    if (record->dropped()) {
      EXPECT_EQ(record->message(), LogQueue::overflow_message);
      EXPECT_EQ(record->severity(), LogSeverity::warning);
    }
    if (record->dropped())
      lost += record->dropped();
    else if (record->severity() == LogSeverity::error)
      ++errors;
    else
      ++info;
  }
  EXPECT_EQ(info, 224U);
  EXPECT_EQ(errors, 1U);
  EXPECT_EQ(lost, 176U);
}

TEST_F(LogQueueTest, GivesEachQueueItsOwnAttempt) {
  const auto first = write_and_read("first");
  ASSERT_TRUE(first);
  LogQueue next{monotonic};
  next.write({LogSeverity::info, "next"});
  const auto record = next.read();
  ASSERT_TRUE(record);
  EXPECT_NE(record->attempt(), first->attempt());
}

TEST_F(LogQueueTest, CutsLongMessagesOnACharacterBoundary) {
  auto record = write_and_read(std::string(1000, 'x'));
  ASSERT_TRUE(record);
  EXPECT_EQ(std::strlen(record->c_str()), 767U);
  record = write_and_read(std::string(766, 'x') + "\xc3\xa9tail");
  ASSERT_TRUE(record);
  EXPECT_EQ(std::strlen(record->c_str()), 766U);
  EXPECT_EQ(record->message()[765], 'x');
  record = write_and_read(std::string(765, 'x') + "\xe2\x82\xactail");
  ASSERT_TRUE(record);
  EXPECT_EQ(std::strlen(record->c_str()), 765U);
  record = write_and_read(std::string(764, 'x') + "\xe2\x82\xactail");
  ASSERT_TRUE(record);
  EXPECT_EQ(std::strlen(record->c_str()), 767U);
  EXPECT_EQ(record->message().substr(764, 3), "\xe2\x82\xac");
}

TEST_F(LogQueueTest, AccountsForEveryWriteFromParallelProducers) {
  constexpr unsigned producers = 4;
  constexpr unsigned writes = 1000;
  libchiaking::platform::DefaultThreadingProvider threading;
  std::vector<libchiaking::platform::Thread> threads;
  for (unsigned p = 0; p < producers; ++p)
    threads.push_back(libchiaking::platform::Thread::start(
                          threading, "producer",
                          libchiaking::platform::ThreadRole::unspecified,
                          [this] {
                            for (unsigned i = 0; i < writes; ++i)
                              queue.write({LogSeverity::info, "parallel"});
                          })
                          .value());
  for (auto& thread : threads) EXPECT_TRUE(thread.join());
  std::uint64_t total{};
  while (const auto record = queue.read())
    total += record->dropped() ? record->dropped() : 1;
  EXPECT_EQ(total, producers * writes);
}

}  // namespace
}  // namespace diagnostics_test
