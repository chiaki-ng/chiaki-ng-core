/// @file
/// @brief Declares SessionJournal, the bounded diagnostic log of one session.
#pragma once

#include "libchiaking/detail/diagnostics/api_capture.hpp"
#include "libchiaking/detail/diagnostics/log_queue.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace libchiaking::platform {
class ClockProvider;
}

namespace libchiaking::diagnostics {

/// @brief Collects the diagnostics of a session so a frontend can read them at
/// its own pace.
///
/// A session writes its log lines into a journal through the
/// platform::DiagnosticsProvider interface. A frontend drains it with
/// SessionJournal::read, typically from a UI or logging thread, and may keep
/// doing so after the session has ended. The journal is bounded: when it is
/// full or momentarily busy a record is dropped and counted, never waited for,
/// so writing cannot stall the stream.
///
/// A journal can also follow a LogQueue, and then yields that queue's records
/// alongside its own. Each session has its own journal unless
/// Play::journal_sessions_in supplies a shared one.
///
/// @see Session::journal
class SessionJournal final : public platform::DiagnosticsProvider {
 public:
  /// @brief Most session records held at once; further ones are dropped until
  /// some are read.
  static constexpr std::size_t capacity = 256;
  /// @brief Records held before routine ones are dropped; the remainder is
  /// kept for warnings and errors.
  static constexpr std::size_t routine_capacity = 224;
  /// @brief Longest message kept, in bytes; longer ones are cut.
  static constexpr std::size_t message_limit = 899;
  /// @brief Most followed log records kept by SessionJournal::retain.
  static constexpr std::size_t retained_logs = 257;
  /// @brief Most captured API exchanges kept by SessionJournal::retain.
  static constexpr std::size_t retained_captures = 33;

  /// @brief Says where an entry came from.
  enum class Origin : std::uint8_t {
    /// @brief Read from the log queue the journal follows.
    core,
    /// @brief Written into this journal.
    session,
  };

  /// @brief Names shown at the start of each formatted line, per origin.
  struct Labels {
    /// @brief Label for Origin::core entries.
    std::string core{"core"};
    /// @brief Label for Origin::session entries.
    std::string session{"session"};
  };

  /// @brief Holds one diagnostic record read from a journal.
  class Entry {
   public:
    /// @brief Returns where the record came from.
    ///
    /// @return Where the record came from.
    auto origin() const noexcept -> Origin { return origin_; }
    /// @brief Returns how serious the record is.
    ///
    /// @return How serious the record is.
    auto severity() const noexcept -> platform::LogSeverity {
      return severity_;
    }
    /// @brief Returns the number of the source that produced the record.
    ///
    /// Each journal is given its number when it is created; a Play numbers
    /// the journals of its sessions 1, 2, 3 and so on, so records of
    /// different sessions can be told apart. Origin::core entries carry the
    /// number of their log queue instead.
    ///
    /// @return The number of the source that produced the record.
    auto attempt() const noexcept -> std::uint64_t { return attempt_; }
    /// @brief Returns the record's position within its source.
    ///
    /// Session records are counted from 1. It is 0 for the notice that
    /// reports dropped records.
    ///
    /// @return The record's position within its source.
    auto sequence() const noexcept -> std::uint64_t { return sequence_; }
    /// @brief Returns the time from the creation of the source to the
    /// record.
    ///
    /// @return The time from the creation of the source to the record.
    auto elapsed() const noexcept -> std::chrono::microseconds {
      return elapsed_;
    }
    /// @brief Returns the number of records that were dropped.
    ///
    /// For the notice a journal issues about its own losses this is the
    /// number lost since the previous notice. For an Origin::core entry it
    /// is the figure the followed log queue attached to the record.
    ///
    /// @return The number of records that were dropped.
    auto dropped() const noexcept -> std::uint64_t { return dropped_; }
    /// @brief Returns the message, on one line.
    ///
    /// @return A view valid for the life of this entry.
    auto message() const noexcept -> std::string_view { return message_; }
    /// @brief Returns the label for the entry's origin.
    ///
    /// @return The label from the journal's Labels, or "core" or "session"
    ///     when none were given.
    auto label() const noexcept -> std::string_view;
    /// @brief Formats the entry as one log line.
    ///
    /// @return The label, then `attempt=`, `sequence=` and `elapsed_us=`
    ///     fields, a `dropped=` field for Origin::core entries, and the
    ///     message.
    auto line() const -> std::string;

   private:
    friend class SessionJournal;

    std::shared_ptr<const Labels> labels_;
    Origin origin_{Origin::session};
    platform::LogSeverity severity_{platform::LogSeverity::info};
    std::uint64_t attempt_{};
    std::uint64_t sequence_{};
    std::chrono::microseconds elapsed_{};
    std::uint64_t dropped_{};
    std::string message_;
  };

  /// @brief Creates a journal with the default labels.
  ///
  /// @param threading Provider of the journal's mutex; must outlive the
  ///     journal.
  /// @param clock Monotonic clock the entries' elapsed times are measured
  ///     on; must outlive the journal.
  /// @param attempt The number the journal's own entries carry.
  SessionJournal(platform::ThreadingProvider& threading,
                 const platform::ClockProvider& clock, std::uint64_t attempt);
  /// @brief Creates a journal whose lines carry the given labels.
  ///
  /// @param threading Provider of the journal's mutex; must outlive the
  ///     journal.
  /// @param clock Monotonic clock the entries' elapsed times are measured
  ///     on; must outlive the journal.
  /// @param attempt The number the journal's own entries carry.
  /// @param labels Names to print for each origin.
  SessionJournal(platform::ThreadingProvider& threading,
                 const platform::ClockProvider& clock, std::uint64_t attempt,
                 Labels labels);

  /// @brief Starts yielding the records of a log queue, and optionally
  /// captured API exchanges.
  ///
  /// Both sources are borrowed. Call SessionJournal::retain before either of
  /// them is destroyed.
  ///
  /// @param log Log queue to read from.
  /// @param capture Source of captured API exchanges; may be null.
  void follow(LogQueue& log, ApiCapture* capture = nullptr);
  /// @brief Stops following and keeps what the followed sources still hold.
  ///
  /// Up to SessionJournal::retained_logs records and
  /// SessionJournal::retained_captures captures are copied into the journal,
  /// so they can still be read after the sources are gone. A session calls
  /// this on its journal when it ends. Calling it when nothing is followed
  /// does nothing.
  void retain();
  /// @brief Reports whether a log queue is currently being followed.
  ///
  /// @return `true` when a log queue is currently being followed.
  auto following() const -> bool;

  /// @brief Adds a session record; never blocks and never throws.
  ///
  /// The message is cut to SessionJournal::message_limit and its line breaks
  /// become spaces. The record is dropped and counted when another thread is
  /// using the journal at that moment, or when it is full: routine records at
  /// SessionJournal::routine_capacity, warnings and errors at
  /// SessionJournal::capacity.
  ///
  /// @param record Severity and message. The message is copied before the
  ///     call returns.
  void write(platform::LogRecord record) noexcept override;
  /// @brief Takes the next record out of the journal.
  ///
  /// Followed or retained log records come first, then the session's own.
  /// When both are exhausted and records have been dropped since the last
  /// notice, one warning entry reporting the count is returned.
  ///
  /// @return The next entry, or `std::nullopt` when there is nothing to read.
  auto read() -> std::optional<Entry>;
  /// @brief Takes the next captured API exchange.
  ///
  /// @return The capture, from the followed source or from those kept by
  ///     SessionJournal::retain; `std::nullopt` when there is none.
  /// @warning Captures can contain account data. Sanitise them before storing
  ///     or sharing.
  auto take_capture() -> std::optional<ApiCapture::Text>;

 private:
  auto from(const LogQueue::Record& record) const -> Entry;

  const std::shared_ptr<const Labels> labels_;
  const platform::ClockProvider& clock_;
  const std::uint64_t attempt_;
  const std::uint64_t started_us_;
  std::atomic<std::uint64_t> dropped_{};
  mutable platform::Mutex mutex_;
  LogQueue* log_{};
  ApiCapture* capture_{};
  std::deque<Entry> core_;
  std::deque<Entry> entries_;
  std::deque<ApiCapture::Text> captures_;
  std::uint64_t sequence_{};
};

}  // namespace libchiaking::diagnostics
