/// @file
/// @brief Declares graphql::Stats, a session's per-operation record of the
/// GraphQL calls it made, for the diagnostics snapshot.
#pragma once

#include "src/core/psn/graphql/error.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::psn::graphql {

/// @brief Counts, per operation name, the GraphQL requests made and how the
/// last one ended.
///
/// Owned by the psn::Session's state. Thread-safe: every member takes one
/// mutex briefly.
class Stats {
 public:
  /// @brief One operation's record. Plain data.
  struct Entry {
    /// @brief The operation's name.
    std::string operation;
    /// @brief HTTP attempts made, retries included.
    std::uint64_t requests{};
    /// @brief Calls that failed.
    std::uint64_t failures{};
    /// @brief `ok`, or the last failure's GraphqlError::name().
    std::string_view last_status{"ok"};
    /// @brief The last reply's status; 0 when it had none.
    std::uint16_t last_http_status{};
    /// @brief The last failure's GraphQL error code; empty after a success.
    std::string last_code;
    /// @brief When the last call was recorded, in wall-clock milliseconds
    /// since the Unix epoch.
    std::optional<std::int64_t> last_at_ms;
  };

  /// @brief Makes empty statistics.
  ///
  /// @param threading Provider of the statistics' mutex; must outlive this
  ///     object.
  /// @param wall The wall clock calls are stamped with; must outlive this
  ///     object.
  Stats(platform::ThreadingProvider& threading,
        const platform::ClockProvider& wall)
      : wall_(wall), mutex_(threading) {}

  /// @brief Records one call that was sent.
  ///
  /// @param operation The operation's name.
  /// @param http_status The reply's status; 0 when none.
  /// @param attempts HTTP attempts made; negative counts as 0.
  /// @param error The failure, or null for a success.
  void record(std::string_view operation, std::uint16_t http_status,
              int attempts, const GraphqlError* error);
  /// @brief Forgets every record.
  void clear();
  /// @brief Copies the records.
  ///
  /// @return One entry per operation, by name.
  auto entries() const -> std::vector<Entry>;

 private:
  /// @brief The wall clock.
  const platform::ClockProvider& wall_;
  /// @brief Guards entries_.
  mutable platform::Mutex mutex_;
  /// @brief The records, by operation name.
  std::map<std::string, Entry, std::less<>> entries_;
};

}  // namespace libchiaking::psn::graphql
