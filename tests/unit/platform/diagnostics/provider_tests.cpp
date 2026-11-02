#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/platform.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using libchiaking::platform::DefaultThreadingProvider;
using libchiaking::platform::DiagnosticsProvider;
using libchiaking::platform::Lock;
using libchiaking::platform::LogRecord;
using libchiaking::platform::LogSeverity;
using libchiaking::platform::Mutex;
using libchiaking::platform::Thread;
using libchiaking::platform::ThreadingProvider;
using libchiaking::platform::ThreadRole;

class DiagnosticsProviderContract : public ::testing::Test {
 protected:
  struct OwnedLogRecord {
    LogSeverity severity;
    std::string message;

    auto operator<=>(const OwnedLogRecord&) const = default;
  };

  class RecordingDiagnosticsProvider final : public DiagnosticsProvider {
   public:
    explicit RecordingDiagnosticsProvider(ThreadingProvider& threading)
        : mutex_(threading) {}

    void write(LogRecord record) noexcept override {
      const Lock lock{mutex_};
      records_.push_back(
          OwnedLogRecord{record.severity, std::string{record.message}});
    }

    [[nodiscard]] auto take_records() -> std::vector<OwnedLogRecord> {
      return std::move(records_);
    }

   private:
    Mutex mutex_;
    std::vector<OwnedLogRecord> records_;
  };

  static auto threaded_record(std::size_t thread_index,
                              std::size_t record_index) -> OwnedLogRecord {
    return {static_cast<LogSeverity>(thread_index),
            "thread " + std::to_string(thread_index) + " record " +
                std::to_string(record_index)};
  }

  static constexpr std::size_t thread_count = 4;
  static constexpr std::size_t records_per_thread = 32;

  DefaultThreadingProvider threading;
  RecordingDiagnosticsProvider provider{threading};
};

TEST_F(DiagnosticsProviderContract,
       IsDestroyedThroughItsInterfaceAndNeverCopied) {
  EXPECT_TRUE(std::has_virtual_destructor_v<DiagnosticsProvider>);
  EXPECT_FALSE(std::is_copy_constructible_v<DiagnosticsProvider>);
  EXPECT_FALSE(std::is_copy_assignable_v<DiagnosticsProvider>);
}

TEST_F(DiagnosticsProviderContract,
       KeepsSynchronousRecordsInOrderAndEveryThreadedRecord) {
  provider.write({LogSeverity::error, "formatted failure"});
  provider.write({LogSeverity::warning, "warning with \\x01"});
  provider.write({LogSeverity::info, ""});
  provider.write({LogSeverity::verbose, "message"});
  provider.write({LogSeverity::debug, "message"});

  std::array<Thread, thread_count> threads;
  for (std::size_t thread_index = 0; thread_index < thread_count;
       ++thread_index) {
    auto started = Thread::start(
        threading, "diagnostics writer", ThreadRole::unspecified,
        [this, thread_index] {
          for (std::size_t record_index = 0; record_index < records_per_thread;
               ++record_index) {
            const auto record = threaded_record(thread_index, record_index);
            provider.write({record.severity, record.message});
          }
        });
    ASSERT_TRUE(started);
    threads[thread_index] = std::move(started).value();
  }
  for (auto& thread : threads) EXPECT_TRUE(thread.join());

  auto records = provider.take_records();
  ASSERT_EQ(records.size(), 5 + thread_count * records_per_thread);

  const std::array<OwnedLogRecord, 5> expected_synchronous_records{
      OwnedLogRecord{LogSeverity::error, "formatted failure"},
      OwnedLogRecord{LogSeverity::warning, "warning with \\x01"},
      OwnedLogRecord{LogSeverity::info, ""},
      OwnedLogRecord{LogSeverity::verbose, "message"},
      OwnedLogRecord{LogSeverity::debug, "message"},
  };
  EXPECT_TRUE(std::equal(expected_synchronous_records.begin(),
                         expected_synchronous_records.end(), records.begin()));

  std::ranges::sort(records);
  for (std::size_t thread_index = 0; thread_index < thread_count;
       ++thread_index) {
    for (std::size_t record_index = 0; record_index < records_per_thread;
         ++record_index)
      EXPECT_TRUE(std::ranges::binary_search(
          records, threaded_record(thread_index, record_index)))
          << "thread " << thread_index << " record " << record_index;
  }
}

}  // namespace
