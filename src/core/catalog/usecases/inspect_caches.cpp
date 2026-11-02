#include "src/core/catalog/usecases/inspect_caches.hpp"

#include "src/core/catalog/adapters/cache/category_record.hpp"
#include "src/core/catalog/adapters/cache/game_details_record.hpp"
#include "src/core/catalog/adapters/cache/library_record.hpp"
#include "src/core/catalog/adapters/cache/played_games_record.hpp"
#include "src/core/catalog/adapters/cache/refusal_record.hpp"
#include "src/core/catalog/adapters/cache/streamable_list_record.hpp"

#include <numeric>
#include <string>
#include <utility>

namespace libchiaking::catalog::usecases {

auto InspectCaches::shelf_of(CacheId cache) -> Shelf {
  using std::chrono::milliseconds;
  switch (cache) {
    case CacheId::streamable_lists:
      return {cache::StreamableListRecord::kind,
              {},
              milliseconds{cache::StreamableListRecord::lifetime}};
    case CacheId::included_titles:
      return {cache::PremiumCategoryRecord::kind,
              {},
              milliseconds{cache::PremiumCategoryRecord::lifetime}};
    case CacheId::for_sale_titles:
      return {cache::ForSaleCategoryRecord::kind,
              {},
              milliseconds{cache::ForSaleCategoryRecord::lifetime}};
    case CacheId::libraries:
      return {cache::Cache::users_segment, cache::LibraryRecord::kind,
              milliseconds{cache::LibraryRecord::lifetime}};
    case CacheId::refusals:
      return {cache::Cache::users_segment, cache::RefusalRecord::kind,
              milliseconds{StreamRefusal::lifetime}};
    case CacheId::game_details:
      return {cache::GameDetailsRecord::kind,
              {},
              milliseconds{GameDetails::lifetime}};
    case CacheId::played_games:
      return {cache::Cache::users_segment, cache::PlayedGamesRecord::kind,
              milliseconds{cache::PlayedGamesRecord::lifetime}};
    case CacheId::images:
      break;
  }
  return {cache::ImageShelf::shelf, {}, std::nullopt};
}

auto InspectCaches::records_of(const Shelf& shelf)
    -> Expected<std::vector<platform::RecordListing>, Unanswered> {
  auto listed = held_.records().listed(shelf.prefix);
  if (!listed || shelf.leaf.empty()) return listed;
  const auto ending =
      std::string{platform::RecordName::separator} + std::string{shelf.leaf};
  std::erase_if(listed.value(), [&](const platform::RecordListing& record) {
    return !record.name.text().ends_with(ending);
  });
  return listed;
}

auto InspectCaches::images() -> std::vector<CacheEntry> {
  std::vector<CacheEntry> entries;
  for (auto& held : held_.images().held())
    entries.push_back(CacheEntry{.cache = CacheId::images,
                                 .image = std::move(held.image),
                                 .bytes = held.bytes,
                                 .stored_at = held.stored_at});
  return entries;
}

auto InspectCaches::summaries()
    -> Expected<std::vector<CacheSummary>, Unanswered> {
  std::vector<CacheSummary> all;
  for (const auto cache : every_cache) {
    if (cache == CacheId::images) {
      const auto held = images();
      all.push_back(
          {cache, held.size(),
           std::accumulate(held.begin(), held.end(), std::uint64_t{},
                           [](std::uint64_t sum, const CacheEntry& entry) {
                             return sum + entry.bytes;
                           })});
      continue;
    }
    const auto records = records_of(shelf_of(cache));
    if (!records) return unexpected(records.error());
    all.push_back(
        {cache, records.value().size(),
         std::accumulate(
             records.value().begin(), records.value().end(), std::uint64_t{},
             [](std::uint64_t sum, const platform::RecordListing& record) {
               return sum + record.bytes;
             })});
  }
  return all;
}

auto InspectCaches::entries(CacheId cache)
    -> Expected<std::vector<CacheEntry>, Unanswered> {
  if (cache == CacheId::images) return images();
  const auto shelf = shelf_of(cache);
  const auto records = records_of(shelf);
  if (!records) return unexpected(records.error());
  std::vector<CacheEntry> entries;
  for (const auto& record : records.value()) {
    const auto kept = held_.records().kept(record.name);
    if (!kept) return unexpected(kept.error());
    CacheEntry entry{
        .cache = cache,
        .bytes = record.bytes,
        .stored_at = std::chrono::time_point_cast<std::chrono::milliseconds>(
            record.modified_at)};
    if (kept.value()) {
      const auto& body = kept.value()->body;
      entry.stored_at = kept.value()->written_at;
      if (const auto& account = kept.value()->account)
        entry.account = AccountId::parse(*account);
      if (const auto& region = kept.value()->region)
        entry.locale = StoreLocale::parse(*region);
      if (cache == CacheId::game_details) {
        entry.concept_id = ConceptId::parse(body.concept_id.value_or(""));
        if (!entry.concept_id)
          entry.product = ProductId::parse(body.product.value_or(""));
      }
      const auto fetched_at = cache::time_of(body.fetched_at_ms);
      if (shelf.lifetime && (fetched_at || cache == CacheId::refusals))
        entry.expires_at =
            fetched_at.value_or(entry.stored_at) + *shelf.lifetime;
    }
    entries.push_back(std::move(entry));
  }
  return entries;
}

auto InspectCaches::clear(CacheId cache) -> Expected<void, Unanswered> {
  switch (cache) {
    case CacheId::streamable_lists:
      return held_.forget_lists();
    case CacheId::included_titles:
      return held_.forget_included();
    case CacheId::for_sale_titles:
      return held_.forget_for_sale();
    case CacheId::libraries:
      return held_.forget_libraries();
    case CacheId::refusals:
      return held_.forget_refusals();
    case CacheId::game_details:
      return held_.forget_details();
    case CacheId::played_games:
      return held_.forget_played();
    case CacheId::images:
      break;
  }
  return held_.forget_images();
}

auto InspectCaches::clear_all() -> Expected<void, Unanswered> {
  std::optional<Unanswered> failure;
  for (const auto cache : every_cache)
    if (const auto cleared = clear(cache); !cleared) failure = cleared.error();
  if (failure) return unexpected(*failure);
  return {};
}

}  // namespace libchiaking::catalog::usecases
