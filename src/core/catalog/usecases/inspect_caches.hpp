/// @file
/// @brief Declares usecases::InspectCaches, the use case behind the settings
/// screen that shows and clears the catalog's caches.
#pragma once

#include "libchiaking/detail/catalog/catalog.hpp"
#include "src/core/catalog/adapters/cache/held_answers.hpp"

#include <chrono>
#include <optional>
#include <string_view>
#include <vector>

namespace libchiaking::catalog::usecases {

/// @brief Lists, summarises and clears each kind of cached answer.
///
/// Borrows the held answers, which must outlive it. Every call blocks on the
/// record store. Not copyable.
class InspectCaches {
 public:
  /// @brief Uses the held answers.
  ///
  /// @param held The held answers. Must outlive this object.
  explicit InspectCaches(HeldAnswers& held) noexcept : held_(held) {}
  /// @brief Not copyable: it borrows shared state.
  InspectCaches(const InspectCaches&) = delete;
  /// @brief Not copyable: it borrows shared state.
  auto operator=(const InspectCaches&) -> InspectCaches& = delete;

  /// @brief How many records, and how many bytes, each cache holds.
  ///
  /// @return One summary per cache in every_cache order; the first storage
  ///     failure.
  [[nodiscard]] auto summaries()
      -> Expected<std::vector<CacheSummary>, Unanswered>;
  /// @brief Every record of one cache.
  ///
  /// Each entry carries its size and when it was stored, and, where the record
  /// says, its account, locale, game and expiry.
  ///
  /// @param cache The cache.
  /// @return The entries; a storage failure.
  [[nodiscard]] auto entries(CacheId cache)
      -> Expected<std::vector<CacheEntry>, Unanswered>;
  /// @brief Clears one cache in memory and on disk.
  ///
  /// CacheId::included_titles clears the Premium category only and
  /// CacheId::for_sale_titles the for-sale category only.
  ///
  /// @param cache The cache.
  /// @return Nothing on success; a storage failure.
  [[nodiscard]] auto clear(CacheId cache) -> Expected<void, Unanswered>;
  /// @brief Clears every cache, going on after a failure.
  ///
  /// @return Nothing on success; the last storage failure.
  [[nodiscard]] auto clear_all() -> Expected<void, Unanswered>;

 private:
  /// @brief Where one cache's records live and how long they last.
  struct Shelf {
    /// @brief The first record name segment.
    std::string_view prefix;
    /// @brief The last name segment for per-account records; empty otherwise.
    std::string_view leaf;
    /// @brief How long an answer is served; std::nullopt for images.
    std::optional<std::chrono::milliseconds> lifetime;
  };

  /// @brief Where a cache's records live.
  ///
  /// @param cache The cache.
  /// @return Its shelf.
  static auto shelf_of(CacheId cache) -> Shelf;
  /// @brief The records on a shelf.
  ///
  /// @param shelf The shelf.
  /// @return The records, filtered to the leaf for per-account records; a
  ///     storage failure.
  [[nodiscard]] auto records_of(const Shelf& shelf)
      -> Expected<std::vector<platform::RecordListing>, Unanswered>;
  /// @brief Every kept image as a cache entry.
  ///
  /// @return The entries.
  auto images() -> std::vector<CacheEntry>;

  /// @brief The held answers; not owned.
  HeldAnswers& held_;
};

}  // namespace libchiaking::catalog::usecases
