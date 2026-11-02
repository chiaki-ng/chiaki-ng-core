#include "libchiaking/detail/diagnostics/session_journal.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::diagnostics {

SessionJournal::SessionJournal(platform::ThreadingProvider& threading,
                               const platform::ClockProvider& clock,
                               std::uint64_t attempt)
    : clock_(clock),
      attempt_(attempt),
      started_us_(clock.now_microseconds()),
      mutex_(threading) {}

SessionJournal::SessionJournal(platform::ThreadingProvider& threading,
                               const platform::ClockProvider& clock,
                               std::uint64_t attempt, Labels labels)
    : labels_(std::make_shared<const Labels>(std::move(labels))),
      clock_(clock),
      attempt_(attempt),
      started_us_(clock.now_microseconds()),
      mutex_(threading) {}

auto SessionJournal::Entry::label() const noexcept -> std::string_view {
  if (labels_)
    return origin_ == Origin::core ? labels_->core : labels_->session;
  return origin_ == Origin::core ? "core" : "session";
}

auto SessionJournal::Entry::line() const -> std::string {
  std::string text{label()};
  text += " attempt=" + std::to_string(attempt_);
  text += " sequence=" + std::to_string(sequence_);
  text += " elapsed_us=" + std::to_string(elapsed_.count());
  if (origin_ == Origin::core) text += " dropped=" + std::to_string(dropped_);
  text += ' ';
  text += message_;
  return text;
}

auto SessionJournal::from(const LogQueue::Record& record) const -> Entry {
  Entry entry;
  entry.labels_ = labels_;
  entry.origin_ = Origin::core;
  entry.severity_ = record.severity();
  entry.attempt_ = record.attempt();
  entry.sequence_ = record.sequence();
  entry.elapsed_ = record.elapsed();
  entry.dropped_ = record.dropped();
  entry.message_.assign(record.message());
  return entry;
}

void SessionJournal::follow(LogQueue& log, ApiCapture* capture) {
  const platform::Lock lock(mutex_);
  log_ = &log;
  capture_ = capture;
}

void SessionJournal::retain() {
  const platform::Lock lock(mutex_);
  if (auto* log = std::exchange(log_, nullptr)) {
    while (core_.size() < retained_logs) {
      const auto record = log->read();
      if (!record) break;
      core_.push_back(from(*record));
    }
  }
  if (auto* capture = std::exchange(capture_, nullptr)) {
    while (captures_.size() < retained_captures) {
      auto text = capture->take();
      if (!text) break;
      captures_.push_back(std::move(*text));
    }
  }
}

auto SessionJournal::following() const -> bool {
  const platform::Lock lock(mutex_);
  return log_ != nullptr;
}

void SessionJournal::write(platform::LogRecord record) noexcept {
  try {
    auto lock = platform::Lock::try_to_take(mutex_);
    const bool urgent = record.severity == platform::LogSeverity::error ||
                        record.severity == platform::LogSeverity::warning;
    if (!lock.owns_lock() ||
        entries_.size() >= (urgent ? capacity : routine_capacity)) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    Entry entry;
    entry.labels_ = labels_;
    entry.severity_ = record.severity;
    entry.attempt_ = attempt_;
    entry.sequence_ = ++sequence_;
    const auto now_us = clock_.now_microseconds();
    entry.elapsed_ = std::chrono::microseconds{
        now_us > started_us_
            ? static_cast<std::chrono::microseconds::rep>(now_us - started_us_)
            : 0};
    entry.message_.assign(record.message.substr(0, message_limit));
    std::ranges::replace(entry.message_, '\n', ' ');
    std::ranges::replace(entry.message_, '\r', ' ');
    entries_.push_back(std::move(entry));
  } catch (...) {
    dropped_.fetch_add(1, std::memory_order_relaxed);
  }
}

auto SessionJournal::read() -> std::optional<Entry> {
  const platform::Lock lock(mutex_);
  if (log_) {
    if (const auto record = log_->read()) return from(*record);
  } else if (!core_.empty()) {
    auto entry = std::move(core_.front());
    core_.pop_front();
    return entry;
  }
  if (!entries_.empty()) {
    auto entry = std::move(entries_.front());
    entries_.pop_front();
    return entry;
  }
  const auto dropped = dropped_.exchange(0, std::memory_order_relaxed);
  if (dropped == 0) return std::nullopt;
  Entry entry;
  entry.labels_ = labels_;
  entry.severity_ = platform::LogSeverity::warning;
  entry.attempt_ = attempt_;
  entry.dropped_ = dropped;
  entry.message_ = "the session journal dropped " + std::to_string(dropped) +
                   " diagnostic records before they were read: its " +
                   std::to_string(capacity) + "-record queue was full (" +
                   std::to_string(routine_capacity) +
                   " for routine records) or busy";
  return entry;
}

auto SessionJournal::take_capture() -> std::optional<ApiCapture::Text> {
  const platform::Lock lock(mutex_);
  if (capture_) return capture_->take();
  if (captures_.empty()) return std::nullopt;
  auto text = std::move(captures_.front());
  captures_.pop_front();
  return text;
}

}  // namespace libchiaking::diagnostics
