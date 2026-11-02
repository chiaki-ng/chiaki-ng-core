/// @file
/// @brief Declares RawListing, every row the store lists returned before the
/// catalog decides which of them stream, and the Shelf each row came from.
///
/// The rows come from the LegacyCloudStream Kamaji shelves, the public store
/// walk and the PS5 imagic store-app lists. filter_raw_streamable turns them
/// into a StreamableList: it folds editions of one game together, keeps PS Plus
/// titles the store does not stream aside, and notes titles the PS5 store
/// flags as unstreamable.
#pragma once

#include "libchiaking/detail/catalog/wall_time.hpp"

#include <libchiaking/detail/catalog/product_id.hpp>
#include <libchiaking/detail/catalog/store_locale.hpp>
#include <libchiaking/detail/catalog/store_page.hpp>
#include <libchiaking/detail/catalog/streamable_list.hpp>
#include <libchiaking/detail/catalog/title.hpp>
#include <libchiaking/detail/expected.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace libchiaking::catalog {

/// @brief The store list a row was read from.
///
/// The order matters: filter_raw_streamable reads rows shelf by shelf in
/// this order, so the LegacyCloudStream shelves are taken before the PS5 lists
/// and all-ps5-list is read before the PS Plus lists that are checked against
/// it.
enum class Shelf : std::uint8_t {
  /// @brief A LegacyCloudStream Kamaji shelf of the account's streaming
  /// service.
  old_service,
  /// @brief The public store walk, used when the account's service cannot
  /// be read.
  public_store,
  /// @brief The imagic all-ps5-list, every PS5 store-app title.
  all_ps5,
  /// @brief The imagic plus-games-list; its titles are included with PS
  /// Plus.
  plus_games,
  /// @brief The imagic ubisoft-classics-list; its titles are included with
  /// PS Plus.
  ubisoft_classics,
  /// @brief The imagic plus-classics-list; its titles are included with PS
  /// Plus.
  plus_classics,
  /// @brief The imagic plus-monthly-games-list; its titles are included
  /// with PS Plus.
  plus_monthly_games,
  /// @brief The imagic free-to-play-list; listed but not included.
  free_to_play,
};

/// @brief One row of a store list, as the store adapters read it.
///
/// Plain data with no rules; RawListing::filter_raw_streamable reads it.
struct ListedEntry {
  /// @brief The product the row lists.
  ProductId product;
  /// @brief The display name; empty when the store sent none.
  std::string name{};
  /// @brief The list the row came from.
  Shelf shelf{};
  /// @brief The concept the product belongs to, when known.
  std::optional<ConceptId> concept_id{};
  /// @brief Whether the store marks the product as playable on PS5.
  bool on_ps5{};
  /// @brief Whether the store marks the product as playable on PS4.
  bool on_ps4{};
  /// @brief The row's streaming flag; std::nullopt when the store did not
  /// say.
  std::optional<bool> streaming_supported{};
  /// @brief The portrait cover image URL; empty when none.
  std::string cover_url{};
  /// @brief The landscape cover image URL; empty when none.
  std::string landscape_cover_url{};
  /// @brief The concept's store page, when the row linked one.
  std::optional<StorePage> concept_page{};

  /// @brief Compares every field.
  friend auto operator==(const ListedEntry&, const ListedEntry&)
      -> bool = default;
};

/// @brief The rows read for one store region, in read order, with where and
/// how they were read.
///
/// A listing is filled with add while the store is walked, then turned into
/// a StreamableList once. It owns its rows. It is not safe to call add while
/// another thread reads the listing; once filled it may be read from any
/// thread.
class RawListing {
 public:
  /// @brief Wall-clock time in milliseconds since the Unix epoch.
  using Time = catalog::Time;

  /// @brief Where and how the rows were read.
  struct Source {
    /// @brief The store region the rows were asked for.
    StoreLocale region;
    /// @brief The locale the store answered in, which may be a fallback
    /// of the region.
    StoreLocale settled_locale;
    /// @brief Whether the rows came through the account's service or the
    /// public store.
    ListOrigin origin{};
    /// @brief Whether every shelf and page was read.
    bool complete{};
    /// @brief When the rows were read.
    Time fetched_at{};
    /// @brief How far the check for PS4 products without a streamable
    /// copy got; std::nullopt when it was not made.
    std::optional<StreamableList::CopiesChecked> streaming_copies{};

    /// @brief Compares every field.
    friend auto operator==(const Source&, const Source&) -> bool = default;
  };

  /// @brief Starts an empty listing.
  ///
  /// @param source Where and how the rows are being read.
  explicit RawListing(Source source) noexcept : source_(std::move(source)) {}

  /// @brief Appends one row.
  ///
  /// @param entry The row as read; duplicates are kept and resolved by
  ///     filter_raw_streamable.
  void add(ListedEntry entry) { entries_.push_back(std::move(entry)); }

  /// @brief Where and how the rows were read.
  ///
  /// @return The source, valid while this listing lives.
  auto source() const noexcept -> const Source& { return source_; }
  /// @brief Every row in read order.
  ///
  /// @return The rows, valid until the next add.
  auto entries() const noexcept -> std::span<const ListedEntry> {
    return entries_;
  }

  /// @brief Decides which rows stream and builds the streamable list.
  ///
  /// Rows are read shelf by shelf in Shelf order. LegacyCloudStream and public
  /// store rows become LegacyCloudStream titles, first row per product; a row
  /// whose streaming flag is false is kept as marked unstreamable and not
  /// included, any other is included with Premium. PS5 store-app rows
  /// that are neither PS5 nor PS4 are dropped. A streaming PS5 row becomes
  /// a PS5 title; rows with the same concept (or product), platform and
  /// squashed name are one edition, later products becoming aliases of the
  /// first, and the edition is included when any of its rows came from a
  /// PS Plus list. A PS Plus row that does not stream but is on
  /// all-ps5-list is kept aside as included but not streamable. An
  /// all-ps5-list row flagged false whose title key never streams adds its
  /// title number to the unstreamable marks.
  ///
  /// Does not block.
  ///
  /// @return The list, or StreamableList::Invalid when a title's product
  ///     collides with an earlier title or alias.
  [[nodiscard]] auto filter_raw_streamable() const
      -> Expected<StreamableList, StreamableList::Invalid>;

  /// @brief Compares the source and every row.
  friend auto operator==(const RawListing&, const RawListing&)
      -> bool = default;

 private:
  /// @brief The one-pass accumulator filter_raw_streamable feeds the rows
  /// to; defined in raw_listing.cpp.
  class Filter;

  /// @brief Where and how the rows were read; set at construction.
  Source source_;
  /// @brief The rows in read order.
  std::vector<ListedEntry> entries_;
};

}  // namespace libchiaking::catalog
