/// @file
/// @brief Declares LogQueue, a bounded in-memory DiagnosticsProvider a frontend
/// drains at its own pace.
#pragma once

#include "libchiaking/platform/diagnostics/provider.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace libchiaking::platform {
class ClockProvider;
}

namespace libchiaking::diagnostics {

/// @brief Stock DiagnosticsProvider that keeps log lines in a fixed ring for
/// the frontend to collect.
///
/// write() never blocks and never allocates, so library threads are not held up
/// by logging. The frontend calls read() from one consumer, for example on a
/// timer, and does what it likes with each Record. DefaultPlatform owns one and
/// installs it as the factory's diagnostics provider.
///
/// When the ring is full, or when a writer finds another caller inside the
/// queue, the line is dropped and counted; the count is reported to the reader
/// as a warning record once the ring has been drained.
class LogQueue final : public platform::DiagnosticsProvider {
 public:
  /// @brief Number of records the ring holds.
  static constexpr std::size_t capacity = 256;
  /// @brief Slots only LogSeverity::error lines may use, so errors still fit
  /// when the ring is otherwise full.
  static constexpr std::size_t error_reserve = 32;
  /// @brief Longest message kept, in bytes; longer text is cut at a UTF-8
  /// character boundary.
  static constexpr std::size_t message_limit = 767;
  /// @brief Text of the record that reports dropped lines.
  static constexpr std::string_view overflow_message =
      "diagnostics queue overflow or contention";

  /// @brief One log line taken from a LogQueue, with the context added when
  /// it was queued.
  class Record {
   public:
    /// @brief How important the line is.
    ///
    /// @return The severity the line was written with.
    auto severity() const noexcept -> platform::LogSeverity {
      return severity_;
    }
    /// @brief Identifier of the LogQueue that produced the record. See
    /// LogQueue::attempt.
    ///
    /// @return The queue's identifier.
    auto attempt() const noexcept -> std::uint64_t { return attempt_; }
    /// @brief Position of the record among those the queue stored, counting
    /// from 1. Dropped lines consume no number.
    ///
    /// @return The record's number, starting at 1.
    auto sequence() const noexcept -> std::uint64_t { return sequence_; }
    /// @brief Time between the queue's construction and the line being
    /// queued, on a monotonic clock.
    ///
    /// @return The elapsed time.
    auto elapsed() const noexcept -> std::chrono::microseconds {
      return elapsed_;
    }
    /// @brief Number of lines lost before this record.
    ///
    /// @return Zero for an ordinary record. Non-zero only on the overflow
    ///     record, whose message() is LogQueue::overflow_message.
    auto dropped() const noexcept -> std::uint64_t { return dropped_; }
    /// @brief The line's text, at most LogQueue::message_limit bytes. Valid
    /// while the Record exists.
    ///
    /// @return A view of the text, without a terminator.
    auto message() const noexcept -> std::string_view {
      return {text_.data(), length_};
    }
    /// @brief The same text as a NUL-terminated string. Valid while the
    /// Record exists.
    ///
    /// @return A pointer to the NUL-terminated text; never null.
    auto c_str() const noexcept -> const char* { return text_.data(); }

   private:
    friend class LogQueue;

    platform::LogSeverity severity_{platform::LogSeverity::info};
    std::uint64_t attempt_{};
    std::uint64_t sequence_{};
    std::chrono::microseconds elapsed_{};
    std::uint64_t dropped_{};
    std::size_t length_{};
    std::array<char, message_limit + 1> text_{};
  };

  /// @brief Creates an empty queue, assigns it the next attempt number and
  /// notes the time its records' elapsed times count from.
  ///
  /// @param clock The monotonic clock elapsed times are read from; must
  ///     outlive the queue.
  explicit LogQueue(const platform::ClockProvider& clock) noexcept;

  /// @brief Identifier of this queue within the process.
  ///
  /// @return A number that starts at 1 and increases with each LogQueue
  ///     constructed, so lines from successive queues can be told apart.
  auto attempt() const noexcept -> std::uint64_t { return attempt_; }
  /// @brief Queues one log line without blocking. See
  /// platform::DiagnosticsProvider::write.
  ///
  /// Safe to call from any thread. The text is copied, and truncated if it
  /// exceeds message_limit.
  ///
  /// @param record Line to queue. It is dropped and counted when the ring is
  ///     full for its severity or another call is in progress.
  void write(platform::LogRecord record) noexcept override;
  /// @brief Takes the oldest queued record.
  ///
  /// Call it repeatedly until it returns std::nullopt. Intended for a single
  /// consumer.
  ///
  /// @return The oldest record. When the ring is empty and lines were dropped
  ///     since the last report, a warning record carrying the dropped count.
  ///     std::nullopt when there is nothing to report, or when a writer is
  ///     inside the queue at that moment; try again later.
  auto read() noexcept -> std::optional<Record>;

 private:
  static auto next_attempt() noexcept -> std::uint64_t;

  auto stamped(platform::LogSeverity severity,
               std::string_view message) noexcept -> Record;

  const std::uint64_t attempt_;
  const platform::ClockProvider& clock_;
  const std::uint64_t started_;
  std::atomic_flag locked_ = ATOMIC_FLAG_INIT;
  std::atomic<std::uint64_t> dropped_{};
  std::array<Record, capacity> records_{};
  std::size_t head_{};
  std::size_t count_{};
  std::uint64_t sequence_{};
};

}  // namespace libchiaking::diagnostics
