/// @file
/// @brief Declares the names and summaries of the caches the catalog keeps in
/// the record store.
#pragma once

#include "libchiaking/detail/catalog/content_url.hpp"
#include "libchiaking/detail/catalog/product_id.hpp"
#include "libchiaking/detail/catalog/store_locale.hpp"
#include "libchiaking/detail/catalog/title.hpp"
#include "libchiaking/detail/catalog/wall_time.hpp"
#include "libchiaking/detail/users/account_id.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace libchiaking::catalog {

/// @brief One of the caches the catalog keeps.
enum class CacheId : std::uint8_t {
  /// @brief Streamable lists, one per store region, reused for 24 hours.
  streamable_lists,
  /// @brief The premium streaming category, which says which PS5 titles a
  /// plan includes; one record per locale, reused for 7 days.
  included_titles,
  /// @brief Owned games, one record per account, reused for 24 hours.
  libraries,
  /// @brief Remembered refusals by PS's streaming service, one record per
  /// account; each refusal stands for 24 hours.
  refusals,
  /// @brief Game details, one per locale and product or concept, reused for
  /// 24 hours.
  game_details,
  /// @brief Downloaded store images. They have no expiry time.
  images,
  /// @brief The store's for-sale streaming category, which says which PS5
  /// titles can be bought to stream; one record per locale, reused for 7
  /// days. Appended last so the values before it keep their numbers.
  for_sale_titles,
  /// @brief Every game an account has played, one record per account, reused
  /// for 6 hours.
  played_games,
};

/// @brief Every CacheId, in the order summaries are reported.
inline constexpr std::array every_cache{
    CacheId::streamable_lists, CacheId::included_titles,
    CacheId::for_sale_titles,  CacheId::libraries,
    CacheId::refusals,         CacheId::game_details,
    CacheId::played_games,     CacheId::images};

/// @brief How much one cache holds.
///
/// @see Catalog::caches
struct CacheSummary {
  /// @brief The cache that was measured.
  CacheId id{};
  /// @brief Number of records in the cache.
  std::size_t entries{};
  /// @brief Total size of those records, in bytes.
  std::uint64_t bytes{};

  /// @brief Compares every field.
  friend auto operator==(const CacheSummary&, const CacheSummary&)
      -> bool = default;
};

/// @brief One record in a cache, described by whatever identifies it.
///
/// Which optional fields are set depends on the cache: account for per-account
/// records, locale for per-region ones, concept or product for game details,
/// image for images.
///
/// @see Catalog::cache_entries
struct CacheEntry {
  /// @brief Wall-clock time in milliseconds.
  using Time = catalog::Time;

  /// @brief The cache the record belongs to.
  CacheId cache{};
  /// @brief The account the record was kept for, when it is per account.
  std::optional<AccountId> account{};
  /// @brief The store region or locale the record was kept for, when it has
  /// one.
  std::optional<StoreLocale> locale{};
  /// @brief For game details kept by product: that product.
  std::optional<ProductId> product{};
  /// @brief For game details kept by concept: that concept.
  std::optional<ConceptId> concept_id{};
  /// @brief For an image: its address.
  std::optional<ImageRef> image{};
  /// @brief Size of the record, in bytes.
  std::uint64_t bytes{};
  /// @brief When the record was written.
  Time stored_at{};
  /// @brief When the record stops being reused; empty for images and for
  /// records that do not state when their content was fetched.
  std::optional<Time> expires_at{};

  /// @brief Compares every field.
  friend auto operator==(const CacheEntry&, const CacheEntry&)
      -> bool = default;
};

}  // namespace libchiaking::catalog
