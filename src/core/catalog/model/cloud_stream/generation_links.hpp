/// @file
/// @brief Declares cloud_stream::GenerationLinks, which pairs the PS4 and PS5
/// versions of one game by title number.
///
/// PS's store products for a cross-generation game grant both versions:
/// a PS5 product's SKUs list the PS4 title's entitlement next to the PS5
/// one. The links are read from the Premium and for-sale categories the
/// catalog already holds, so they cost no request of their own.
#pragma once

#include "src/core/catalog/model/cloud_stream/premium_category.hpp"

#include <libchiaking/detail/catalog/title_number.hpp>

#include <map>
#include <optional>
#include <span>

namespace libchiaking::catalog::cloud_stream {

/// @brief The PS4 and PS5 title numbers PS's store sells together.
///
/// A value; build it on one thread, then share it read-only.
class GenerationLinks {
 public:
  /// @brief Adds the links one category's products declare.
  ///
  /// For each product whose own title number is a PS5 title, every
  /// entitlement of its SKUs whose title number is a PS4 title links to it.
  /// A PS4 or PS5 title linked already keeps its first link, so PS's
  /// product order decides between editions.
  ///
  /// @param products The products in PS's order.
  void add(std::span<const PremiumProduct> products);

  /// @brief The other generation's version of a title.
  ///
  /// @param title A PS4 or PS5 title number.
  /// @return The linked PS5 title for a PS4 title, the linked PS4 title for
  ///     a PS5 title, or std::nullopt when the store links none.
  auto other_of(const TitleNumber& title) const -> std::optional<TitleNumber>;

  /// @brief Whether no link is known.
  ///
  /// @return True when no category declared a link.
  auto empty() const noexcept -> bool { return ps5_of_ps4_.empty(); }

 private:
  /// @brief The PS5 title linked to each PS4 title.
  std::map<TitleNumber, TitleNumber> ps5_of_ps4_;
  /// @brief The PS4 title linked to each PS5 title.
  std::map<TitleNumber, TitleNumber> ps4_of_ps5_;
};

}  // namespace libchiaking::catalog::cloud_stream
