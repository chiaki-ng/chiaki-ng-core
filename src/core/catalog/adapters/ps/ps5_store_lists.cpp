#include "src/core/catalog/adapters/ps/ps5_store_lists.hpp"

#include "src/core/psn/rest/store/store_client.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace libchiaking::catalog::ps {

Ps5StoreLists::Ps5StoreLists(net::http::Client& http,
                             const platform::ClockProvider& wall,
                             std::size_t maximum_response_bytes) noexcept
    : http_(http),
      stamp_(wall),
      maximum_response_bytes_(maximum_response_bytes) {}

auto Ps5StoreLists::fetch(const StoreLocale& region)
    -> Expected<RawListing, Unanswered> {
  psn::rest::StoreClient store{http_.limited(maximum_response_bytes_)};
  std::uint16_t last_status = 0;
  for (const auto& settled : region.fallbacks()) {
    const auto locale = settled.lowercase();
    std::vector<ListedEntry> entries;
    bool any_listed = false;
    bool all_ps5_listed = false;
    for (const auto& [name, shelf] : lists) {
      const auto document = store.games_lists<Reply>(locale, name);
      if (!document) {
        last_status = document.error().http_status();
        continue;
      }
      any_listed = true;
      if (shelf == Shelf::all_ps5) all_ps5_listed = true;
      for (auto& entry : listed(document.value(), shelf))
        entries.push_back(std::move(entry));
    }
    if (!any_listed) continue;
    if (http_.stop_requested()) return unexpected(Unanswered::cancelled());
    http_.log(platform::LogSeverity::info,
              "[CLOUD_STREAM] imagic settled on " + settled.text() + ": " +
                  std::to_string(entries.size()) + " listed");
    RawListing listing{{region, settled, ListOrigin::public_store,
                        all_ps5_listed, stamp_.now()}};
    for (auto& entry : entries) listing.add(std::move(entry));
    return listing;
  }
  if (http_.stop_requested()) return unexpected(Unanswered::cancelled());
  return unexpected(Unanswered{Unanswered::Reason::unavailable, last_status});
}

auto Ps5StoreLists::listed(const Reply& list, Shelf shelf)
    -> std::vector<ListedEntry> {
  std::vector<ListedEntry> entries;
  for (const auto& group : list) {
    if (!group || !group->games) continue;
    for (const auto& game : *group->games)
      if (auto entry =
              game ? StoreListing::listed_on(*game, shelf) : std::nullopt)
        entries.push_back(std::move(*entry));
  }
  return entries;
}

}  // namespace libchiaking::catalog::ps
