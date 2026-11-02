#include "libchiaking/detail/diagnostics/log_queue.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"

#include "src/core/utils/text/utf8.hpp"

#include <algorithm>

namespace libchiaking::diagnostics {

auto LogQueue::next_attempt() noexcept -> std::uint64_t {
  static std::atomic<std::uint64_t> next{1};
  return next.fetch_add(1);
}

LogQueue::LogQueue(const platform::ClockProvider& clock) noexcept
    : attempt_(next_attempt()),
      clock_(clock),
      started_(clock.now_microseconds()) {}

auto LogQueue::stamped(platform::LogSeverity severity,
                       std::string_view message) noexcept -> Record {
  Record record;
  record.severity_ = severity;
  record.attempt_ = attempt_;
  record.sequence_ = ++sequence_;
  record.elapsed_ =
      std::chrono::microseconds{static_cast<std::chrono::microseconds::rep>(
          clock_.now_microseconds() - started_)};
  record.length_ = text::utf8::prefix(message, message_limit).size();
  std::copy_n(message.data(), record.length_, record.text_.data());
  return record;
}

void LogQueue::write(platform::LogRecord record) noexcept {
  if (locked_.test_and_set(std::memory_order_acquire)) {
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const auto limit = record.severity == platform::LogSeverity::error
                         ? capacity
                         : capacity - error_reserve;
  if (count_ >= limit) {
    dropped_.fetch_add(1, std::memory_order_relaxed);
  } else {
    records_[(head_ + count_) % capacity] =
        stamped(record.severity, record.message);
    ++count_;
  }
  locked_.clear(std::memory_order_release);
}

auto LogQueue::read() noexcept -> std::optional<Record> {
  if (locked_.test_and_set(std::memory_order_acquire)) return std::nullopt;
  std::optional<Record> result;
  if (count_) {
    result = records_[head_];
    head_ = (head_ + 1) % capacity;
    --count_;
  } else if (const auto lost =
                 dropped_.exchange(0, std::memory_order_relaxed)) {
    result = stamped(platform::LogSeverity::warning, overflow_message);
    result->dropped_ = lost;
  }
  locked_.clear(std::memory_order_release);
  return result;
}

}  // namespace libchiaking::diagnostics
