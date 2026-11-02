/// @file
/// @brief Declares ResultCache, a keyed, time-limited memory of answers from
/// PS that concurrent askers share.
#pragma once

#include "src/core/psn/auth/timeline.hpp"
#include "src/core/psn/graphql/check.hpp"
#include "src/core/utils/concurrency/single_flight.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::psn {

/// @brief How full a ResultCache is, for diagnostics. Plain data.
struct ResultCacheCounts {
  /// @brief Keys holding an answer, fresh or stale.
  std::size_t entries{};
  /// @brief Keys holding an answer younger than the lifetime.
  std::size_t fresh{};
};

/// @brief Remembers the last successful answer per key for a fixed lifetime,
/// and lets concurrent askers of one key share one fetch.
///
/// A remembered answer is reused while fresh (younger than the lifetime on the
/// monotonic clock) and acceptable to the asker; failures are never
/// remembered, but one is shared with the askers waiting on that fetch unless
/// it is a cancellation. clear() and forget() also stop an answer still in
/// flight from being remembered.
///
/// The users context keeps one per question type (for example the PlayStation
/// Plus membership) per user. Thread-safe: one mutex guards every key; a fetch
/// runs without it.
///
/// @tparam Value The answer. Copyable, with a `bool cached` member set on
///     every answer handed out and a `std::int64_t fetched_at_ms` member set
///     when fetched.
/// @tparam Error The failure. Provides a static `cancelled()` and a member
///     `is_cancelled()`.
template <class Value, class Error = graphql::CheckError>
class ResultCache {
 public:
  /// @brief An answer or a failure.
  using Result = Expected<Value, Error>;
  /// @brief Asks PS; it may block for the request's duration.
  using Fetch = std::function<Result()>;

  /// @brief Makes an empty cache.
  ///
  /// @param threading Provider of the cache's mutex and of the condition
  ///     variables waiting askers park on; must outlive the cache.
  /// @param clock The monotonic clock freshness is measured on; must outlive
  ///     the cache.
  /// @param wall The wall clock fetched_at_ms is stated in; must outlive the
  ///     cache.
  /// @param ttl How long an answer stays fresh.
  ResultCache(platform::ThreadingProvider& threading,
              const platform::ClockProvider& clock,
              const platform::ClockProvider& wall,
              std::chrono::microseconds ttl)
      : time_(clock, wall),
        ttl_(ttl),
        threading_(threading),
        clock_(clock),
        mutex_(threading) {}

  /// @brief Answers from memory or by fetching, sharing a fetch in flight.
  ///
  /// Blocks while another asker's fetch of the same key is in flight, then
  /// for this asker's own fetch, if one is needed.
  ///
  /// @tparam Accept Callable as `accept (const Value&)` returning whether a
  ///     remembered answer will do.
  /// @param key Which question; empty is a valid key.
  /// @param stop Ends the wait for another asker's fetch; it does not
  ///     interrupt a fetch.
  /// @param accept Judges a remembered or freshly landed answer.
  /// @param fetch Asks PS; called at most once, without the lock.
  /// @return A fresh acceptable answer, marked cached; or this fetch's
  ///     answer, marked not cached and stamped with the wall-clock time it
  ///     landed.
  /// @retval Error::cancelled() `stop` fired before a fetch was begun or
  ///     while waiting.
  /// @retval Error The shared fetch's failure, when it was not a
  ///     cancellation, or this asker's own fetch's failure.
  template <class Accept>
  [[nodiscard]] auto get(std::string_view key, const platform::StopToken& stop,
                         const Accept& accept, const Fetch& fetch) -> Result {
    platform::Lock lock{mutex_};
    auto found = slots_.find(key);
    if (found == slots_.end())
      found = slots_.try_emplace(std::string{key}, threading_, clock_).first;
    auto& slot = found->second;
    for (;;) {
      if (fresh(slot) && accept(*slot.value)) return reused(*slot.value);
      if (!slot.flight.in_flight()) break;
      if (!slot.flight.await_landing(lock, stop)) return cancelled();
      if (fresh(slot) && accept(*slot.value)) continue;
      if (const auto& shared = slot.flight.landed();
          shared && !shared->has_value() && !shared->error().is_cancelled())
        return *shared;
    }
    if (stop.stop_requested()) return cancelled();
    const auto generation = generation_;
    const auto forgotten = slot.forgotten;
    return slot.flight.lead(lock, fetch, [&](Result& result) {
      if (result) {
        const auto now = time_.now_us();
        result.value().cached = false;
        result.value().fetched_at_ms = time_.unix_ms(now);
        if (generation == generation_ && forgotten == slot.forgotten) {
          slot.value = result.value();
          slot.fetched_us = now;
        }
      }
      return result;
    });
  }

  /// @brief Reads a fresh answer without fetching or waiting.
  ///
  /// @param key Which question.
  /// @return The answer, marked cached; nothing when none is fresh.
  [[nodiscard]] auto peek(std::string_view key) -> std::optional<Value> {
    platform::Lock lock{mutex_};
    const auto found = slots_.find(key);
    if (found == slots_.end() || !fresh(found->second)) return std::nullopt;
    return reused(*found->second.value);
  }

  /// @brief Reads the remembered answer, fresh or stale.
  ///
  /// @param key Which question.
  /// @return The answer, as fetched (not marked cached), and whether it is
  ///     still fresh; nothing when none is remembered.
  [[nodiscard]] auto held(std::string_view key)
      -> std::optional<std::pair<Value, bool>> {
    platform::Lock lock{mutex_};
    const auto found = slots_.find(key);
    if (found == slots_.end() || !found->second.value) return std::nullopt;
    return std::pair<Value, bool>{*found->second.value, fresh(found->second)};
  }

  /// @brief Counts the remembered answers.
  ///
  /// @return The counts.
  auto counts() -> ResultCacheCounts {
    platform::Lock lock{mutex_};
    ResultCacheCounts counts;
    for (const auto& [key, slot] : slots_) {
      if (!slot.value) continue;
      ++counts.entries;
      if (fresh(slot)) ++counts.fresh;
    }
    return counts;
  }

  /// @brief Forgets every answer, and keeps fetches in flight from being
  /// remembered.
  void clear() {
    platform::Lock lock{mutex_};
    ++generation_;
    for (auto& [key, slot] : slots_) slot.forget();
  }

  /// @brief Forgets one key's answer, and keeps a fetch in flight for it from
  /// being remembered.
  ///
  /// @param key Which question.
  void forget(std::string_view key) {
    platform::Lock lock{mutex_};
    if (const auto found = slots_.find(key); found != slots_.end())
      found->second.forget();
  }

  /// @brief Forgets every key starting with a prefix.
  ///
  /// @param prefix The prefix; empty forgets every key.
  void forget_prefix(std::string_view prefix) {
    platform::Lock lock{mutex_};
    for (auto found = slots_.lower_bound(prefix);
         found != slots_.end() && found->first.starts_with(prefix); ++found)
      found->second.forget();
  }

 private:
  /// @brief One key's memory.
  struct Slot {
    /// @brief Makes an empty slot whose flight parks waiters on the cache's
    /// providers.
    ///
    /// @param threading Provider of the flight's condition variable.
    /// @param clock Monotonic clock the flight's waits measure on.
    Slot(platform::ThreadingProvider& threading,
         const platform::ClockProvider& clock)
        : flight(threading, clock) {}

    /// @brief The remembered answer.
    std::optional<Value> value;
    /// @brief When it landed, in monotonic microseconds.
    std::uint64_t fetched_us{};
    /// @brief Incremented by every forget(), so a fetch begun earlier is
    /// dropped.
    std::uint64_t forgotten{};
    /// @brief The fetch in flight, shared by its waiters.
    concurrency::SingleFlight<Result> flight;

    /// @brief Drops the answer and the shared outcome.
    void forget() {
      ++forgotten;
      value.reset();
      flight.forget();
    }
  };

  /// @brief Tells whether a slot's answer is younger than the lifetime.
  ///
  /// @param slot The slot.
  /// @return True when it holds an answer fetched less than ttl_ ago.
  auto fresh(const Slot& slot) const noexcept -> bool {
    return slot.value.has_value() &&
           time_.now_us() <
               slot.fetched_us + static_cast<std::uint64_t>(ttl_.count());
  }

  /// @brief Copies a remembered answer for an asker.
  ///
  /// @param value The answer.
  /// @return A copy marked cached.
  static auto reused(const Value& value) -> Value {
    auto result = value;
    result.cached = true;
    return result;
  }

  /// @brief Builds the failure of a cancelled ask.
  ///
  /// @return Error::cancelled().
  static auto cancelled() -> Result { return unexpected(Error::cancelled()); }

  /// @brief The clocks.
  auth::Timeline time_;
  /// @brief How long an answer stays fresh.
  std::chrono::microseconds ttl_;
  /// @brief Provider of the condition variable each new slot's flight parks
  /// waiters on.
  platform::ThreadingProvider& threading_;
  /// @brief The monotonic clock each slot's flight waits on.
  const platform::ClockProvider& clock_;
  /// @brief Guards the slots and the generation.
  platform::Mutex mutex_;
  /// @brief One slot per key ever asked; slots are never removed.
  std::map<std::string, Slot, std::less<>> slots_;
  /// @brief Incremented by clear(), so fetches begun earlier are dropped.
  std::uint64_t generation_{};
};

}  // namespace libchiaking::psn
