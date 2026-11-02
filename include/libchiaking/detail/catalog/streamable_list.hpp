/// @file
/// @brief Declares StreamableList, the titles PS streams in one store region.
#pragma once

#include "libchiaking/detail/catalog/plus_inclusion.hpp"
#include "libchiaking/detail/catalog/product_id.hpp"
#include "libchiaking/detail/catalog/store_locale.hpp"
#include "libchiaking/detail/catalog/title.hpp"
#include "libchiaking/detail/catalog/title_number.hpp"
#include "libchiaking/detail/catalog/wall_time.hpp"
#include "libchiaking/detail/expected.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace libchiaking::catalog {

/// @brief How a list was obtained.
enum class ListOrigin : std::uint8_t {
  /// @brief Through the signed-in account's own streaming service.
  account_service,
  /// @brief From the public store, used as a fallback.
  public_store,
  /// @brief From the public store of the account's own country, read without
  /// signing in. It lists what the account's streaming service lists, so it is
  /// trusted and kept like account_service.
  regional_store,
};

/// @brief The titles PS streams in one store region, across both streaming
/// services.
///
/// The list is the same for every account in a region; what a particular user
/// may play is decided by matching it against that user's Library. It is
/// immutable; views and pointers it hands out are valid while it lives.
///
/// @see Catalog::get_streamable_list
/// @see MyGames
class StreamableList {
 public:
  /// @brief Wall-clock time in milliseconds.
  using Time = catalog::Time;

  /// @brief Why a list could not be built.
  enum class Invalid : std::uint8_t {
    /// @brief A title's product id is the id or an alias of an earlier
    /// title.
    duplicate_product,
  };

  /// @brief One title with what PS says about a plan including it.
  struct Entry {
    /// @brief The title.
    Title title;
    /// @brief Whether a plan includes it, according to the list.
    PlusInclusion inclusion;

    /// @brief Compares title and inclusion.
    friend auto operator==(const Entry&, const Entry&) -> bool = default;
  };

  /// @brief How far the check for PS4 products without a streamable copy got.
  struct CopiesChecked {
    /// @brief PS4 products that were asked about.
    std::size_t asked{};
    /// @brief Products PS answered for.
    std::size_t answered{};

    /// @brief Compares both counts.
    friend auto operator==(const CopiesChecked&, const CopiesChecked&)
        -> bool = default;
  };

  /// @brief The facts a StreamableList is built from.
  struct Facts {
    /// @brief The store region the list is for.
    StoreLocale region;
    /// @brief The locale the store answered in, which can be a fallback of
    /// the region's.
    StoreLocale settled_locale;
    /// @brief How the list was obtained.
    ListOrigin origin{};
    /// @brief Whether every shelf and page was read; an incomplete list is
    /// not reused.
    bool complete{};
    /// @brief When the list was built.
    Time fetched_at{};
    /// @brief The titles that are listed for streaming.
    std::vector<Entry> titles{};
    /// @brief PS5 store titles a plan includes that the store does not list
    /// as streaming; kept so owned copies can still be matched to them.
    std::vector<Entry> included_but_not_streamable{};
    /// @brief Title numbers the PS5 store flags as not streamable.
    std::vector<TitleNumber> marked_unstreamable{};
    /// @brief How far the PS4 copy check got; empty when it was not made.
    std::optional<CopiesChecked> streaming_copies{};
  };

  /// @brief Builds a list from facts.
  ///
  /// @param facts The facts about the list.
  /// @return The list.
  /// @retval StreamableList::Invalid::duplicate_product Within the streamable
  ///     titles or within the included-only titles, a product id repeats the
  ///     id or an alias of an earlier title.
  [[nodiscard]] static auto of(Facts facts)
      -> Expected<StreamableList, Invalid>;

  /// @brief The store region the list is for.
  ///
  /// @return The region; valid as long as this object.
  auto region() const noexcept -> const StoreLocale& { return region_; }
  /// @brief The locale the store answered in, which can be a fallback of the
  /// region's.
  ///
  /// @return The locale; valid as long as this object.
  auto settled_locale() const noexcept -> const StoreLocale& {
    return settled_locale_;
  }
  /// @brief How the list was obtained.
  ///
  /// @return How the list was obtained.
  auto origin() const noexcept -> ListOrigin { return origin_; }
  /// @brief Whether every shelf and page was read.
  ///
  /// @return True when every shelf and page was read; false otherwise.
  auto is_complete() const noexcept -> bool { return complete_; }
  /// @brief When the list was built.
  ///
  /// @return The time the list was built.
  auto fetched_at() const noexcept -> Time { return fetched_at_; }
  /// @brief The titles that are listed for streaming.
  ///
  /// @return The listed entries; valid as long as this object.
  auto titles() const noexcept -> std::span<const Entry> { return titles_; }
  /// @brief PS5 store titles a plan includes that the store does not list as
  /// streaming.
  ///
  /// @return The entries; valid as long as this object.
  auto included_but_not_streamable() const noexcept -> std::span<const Entry> {
    return not_streamable_;
  }
  /// @brief Every title number the PS5 store flags as not streamable.
  ///
  /// @note StreamableList::marks_unstreamable also checks that no listed
  ///     title streams under the same number.
  /// @return The title numbers; valid as long as this object.
  auto marked_unstreamable() const noexcept -> const std::set<TitleNumber>& {
    return marked_;
  }
  /// @brief How far the PS4 copy check got; empty when it was not made.
  ///
  /// @return The progress, or nothing when the check was not made; valid as
  /// long
  ///     as this object.
  auto streaming_copies() const noexcept
      -> const std::optional<CopiesChecked>& {
    return streaming_copies_;
  }

  /// @brief Finds a streamable title by product.
  ///
  /// @param product The title's product id or one of its aliases.
  /// @return The entry, or null when no streamable title answers to the
  ///     product.
  auto find(const ProductId& product) const noexcept -> const Entry*;
  /// @brief Finds a title among those a plan includes but that are not listed
  /// as streaming.
  ///
  /// @param product The title's product id or one of its aliases.
  /// @return The entry, or null.
  auto find_not_streamable(const ProductId& product) const noexcept
      -> const Entry*;
  /// @brief Whether a streamable title answers to the product.
  ///
  /// @param product The product id to look up.
  /// @return True when a streamable title is known under the product; false
  ///     otherwise.
  auto covers(const ProductId& product) const noexcept -> bool;
  /// @brief Whether any streamable title, or one of its aliases, carries the
  /// title number.
  ///
  /// @param number The title number to look up.
  /// @return True when a streamable title or alias carries the number; false
  ///     otherwise.
  auto lists(const TitleNumber& number) const -> bool;
  /// @brief Whether the title number is flagged as not streamable and no
  /// streamable title carries it.
  ///
  /// @param number The title number to look up.
  /// @return True when the number is flagged and no streamable title carries
  /// it;
  ///     false otherwise.
  auto marks_unstreamable(const TitleNumber& number) const -> bool;
  /// @brief Counts the streamable titles one service carries.
  ///
  /// @param service The service generation to count.
  /// @return The number of streamable titles whose service is the given one.
  auto count_on(ServiceGeneration service) const noexcept -> std::size_t;

  /// @brief Compares every part of the list.
  friend auto operator==(const StreamableList&, const StreamableList&)
      -> bool = default;

 private:
  explicit StreamableList(Facts facts);

  static auto entry_for(std::span<const Entry> entries,
                        const ProductId& product) noexcept -> const Entry*;

  StoreLocale region_;
  StoreLocale settled_locale_;
  ListOrigin origin_;
  bool complete_;
  Time fetched_at_;
  std::vector<Entry> titles_;
  std::vector<Entry> not_streamable_;
  std::set<TitleNumber> marked_;
  std::set<TitleNumber> listed_;
  std::optional<CopiesChecked> streaming_copies_;
};

}  // namespace libchiaking::catalog
