/// @file
/// @brief Declares PlusInclusion, whether a PlayStation Plus plan includes a
/// title.
#pragma once

#include "libchiaking/detail/users/membership.hpp"

#include <cstdint>
#include <optional>

namespace libchiaking::catalog {

/// @brief Whether a PlayStation Plus plan includes a title, from which tier,
/// and which of PS's sources said so.
class PlusInclusion {
 public:
  /// @brief Where the statement came from, most trusted first.
  enum class Source : std::uint8_t {
    /// @brief The premium streaming category fetched for the account.
    store_tags,
    /// @brief The PS5 store's lists of plan games.
    store_lists,
    /// @brief The older LegacyCloudStream service's shelves.
    old_service,
  };

  /// @brief Builds the statement that no plan includes the title.
  ///
  /// @param source Where the statement came from.
  /// @return The inclusion.
  static auto not_included(Source source) noexcept -> PlusInclusion;
  /// @brief Builds the statement that plans from a given tier upwards include
  /// the title.
  ///
  /// @param lowest_tier The lowest tier that includes it.
  /// @param source Where the statement came from.
  /// @return The inclusion, or nothing when lowest_tier is users::Tier::none.
  [[nodiscard]] static auto from_tier(users::Tier lowest_tier,
                                      Source source) noexcept
      -> std::optional<PlusInclusion>;
  /// @brief Builds the statement that only the Premium tier includes the
  /// title.
  ///
  /// @param source Where the statement came from.
  /// @return The inclusion.
  static auto with_premium(Source source) noexcept -> PlusInclusion;

  /// @brief Whether any plan includes the title.
  ///
  /// @return True when a lowest tier is held; false when no plan includes the
  /// title.
  auto is_included() const noexcept -> bool { return lowest_tier_.has_value(); }
  /// @brief The lowest tier that includes the title; empty when no plan does.
  ///
  /// @return The tier, or nothing when no plan includes the title.
  auto lowest_tier() const noexcept -> std::optional<users::Tier> {
    return lowest_tier_;
  }
  /// @brief Where the statement came from.
  ///
  /// @return The source given when the statement was built.
  auto source() const noexcept -> Source { return source_; }
  /// @brief Whether a given membership includes the title.
  ///
  /// @param membership The user's membership.
  /// @return False when no plan includes the title; nothing when a plan does
  ///     but the membership's tier is not known; otherwise whether the tier
  ///     held is high enough.
  auto covers(const users::Membership& membership) const noexcept
      -> std::optional<bool>;
  /// @brief Picks the stronger of two statements about the same title.
  ///
  /// The statement from the more trusted PlusInclusion::Source wins. From the
  /// same source, one that includes the title wins over one that does not,
  /// and then the lower tier wins.
  ///
  /// @param other The other statement.
  /// @return This statement or the other one, unchanged.
  auto combined_with(const PlusInclusion& other) const noexcept
      -> PlusInclusion;

  /// @brief Compares tier and source.
  friend auto operator==(const PlusInclusion&, const PlusInclusion&) noexcept
      -> bool = default;

 private:
  PlusInclusion(std::optional<users::Tier> lowest_tier, Source source) noexcept;

  auto outranks(const PlusInclusion& other) const noexcept -> bool;

  std::optional<users::Tier> lowest_tier_;
  Source source_;
};

}  // namespace libchiaking::catalog
