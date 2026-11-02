#include "src/core/psn/graphql/stats.hpp"

#include <algorithm>

namespace libchiaking::psn::graphql {

void Stats::record(std::string_view operation, std::uint16_t http_status,
                   int attempts, const GraphqlError* error) {
  const auto now_ms =
      static_cast<std::int64_t>(wall_.now_microseconds() / 1000U);
  const platform::Lock lock{mutex_};
  auto found = entries_.find(operation);
  if (found == entries_.end()) {
    found = entries_.emplace(std::string{operation}, Entry{}).first;
    found->second.operation = std::string{operation};
  }
  auto& entry = found->second;
  entry.requests += static_cast<std::uint64_t>(std::max(attempts, 0));
  if (error != nullptr) ++entry.failures;
  entry.last_status = error != nullptr ? error->name() : std::string_view{"ok"};
  entry.last_http_status = http_status;
  entry.last_code =
      error != nullptr ? std::string{error->code()} : std::string{};
  entry.last_at_ms = now_ms;
}

void Stats::clear() {
  const platform::Lock lock{mutex_};
  entries_.clear();
}

auto Stats::entries() const -> std::vector<Entry> {
  const platform::Lock lock{mutex_};
  std::vector<Entry> result;
  result.reserve(entries_.size());
  for (const auto& [operation, entry] : entries_) result.push_back(entry);
  return result;
}

}  // namespace libchiaking::psn::graphql
