/// @file
/// @brief Declares the PS5 store category model: PremiumEntitlement,
/// PremiumSku, PremiumProduct, CategoryScan and PremiumCategory, the
/// Premium streaming category of the mobile catalog API.
///
/// A Premium tile is one category product; it is included when its first SKU
/// branded PS_PLUS_STREAM_PS5 carries an accepted entitlement; accepted means a
/// PSGD full game with streamingSupported exactly true; and a bundle counts as
/// owned when one SKU has all its entitlements owned and active. The same
/// product model also carries the public Store streaming category
/// (ForSaleCategory).
#pragma once

#include "src/core/catalog/model/library/exact_id.hpp"

#include <libchiaking/detail/catalog/owned_title.hpp>
#include <libchiaking/detail/catalog/product_id.hpp>
#include <libchiaking/detail/catalog/store_locale.hpp>
#include <libchiaking/detail/catalog/title.hpp>
#include <libchiaking/detail/catalog/title_number.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::catalog::cloud_stream {

/// @brief One entitlement a category SKU grants.
///
/// Immutable; may be shared between threads.
class PremiumEntitlement {
 public:
  /// @brief The offer's featureType, read from its string form.
  enum class Feature : std::uint8_t {
    /// @brief "FULL_GAME"; the only feature that streams.
    full_game,
    /// @brief "TRIAL".
    trial,
    /// @brief "PLAY_FIRST_TRIAL".
    play_first_trial,
    /// @brief "DISC_REQUIRED".
    disc_required,
    /// @brief Any other text, or none.
    unknown,
  };

  /// @brief The packageType of a PS5 game.
  static constexpr std::string_view game_package = "PSGD";

  /// @brief The fields read from one entitlement of an offer.
  struct Facts {
    /// @brief The entitlement id; std::nullopt when absent or malformed.
    std::optional<OwnershipId> id{};
    /// @brief True when PS sent an id that did not parse, so the
    /// entitlement still names an id that can never be matched.
    bool unreadable_id{};
    /// @brief The packageType; see game_package.
    std::string package_type{};
    /// @brief The featureType.
    Feature feature{Feature::unknown};
    /// @brief streamingSupported when it was exactly true or false;
    /// std::nullopt otherwise.
    std::optional<bool> streaming_supported{};

    /// @brief Compares every field.
    friend auto operator==(const Facts&, const Facts&) -> bool = default;
  };

  /// @brief Keeps one entitlement.
  ///
  /// @param facts The fields as read.
  explicit PremiumEntitlement(Facts facts) noexcept;

  /// @brief Reads a featureType string.
  ///
  /// @param name The text PS sent, compared exactly.
  /// @return The feature, or Feature::unknown for any other text.
  static auto feature_named(std::string_view name) noexcept -> Feature;
  /// @brief The featureType string of a feature.
  ///
  /// @param feature The feature.
  /// @return PS's spelling, or an empty view for Feature::unknown.
  static auto name_of(Feature feature) noexcept -> std::string_view;

  /// @brief The fields as read.
  ///
  /// @return The fields, valid while this entitlement lives.
  auto facts() const noexcept -> const Facts& { return facts_; }
  /// @brief The entitlement id.
  ///
  /// @return The id, or std::nullopt when absent or malformed.
  auto id() const noexcept -> const std::optional<OwnershipId>& {
    return facts_.id;
  }
  /// @brief Whether the entitlement is accepted for streaming.
  ///
  /// @return True for a PSGD full game whose streamingSupported is exactly
  ///     true.
  auto is_a_streamable_full_game() const noexcept -> bool;
  /// @brief Whether the entitlement is a trial version that streams.
  ///
  /// @param feature The trial feature asked about.
  /// @return True for a PSGD entitlement of that feature whose
  ///     streamingSupported is exactly true.
  auto is_a_streamable(Feature feature) const noexcept -> bool;
  /// @brief Whether the entitlement is a PS5 game package.
  ///
  /// @return True when the packageType is PSGD, whatever the feature.
  auto is_a_game() const noexcept -> bool;
  /// @brief Whether PS named an id for the entitlement.
  ///
  /// @return True when the id parsed or was sent but did not parse.
  auto names_an_id() const noexcept -> bool;
  /// @brief Whether the entitlement's id is one of a set.
  ///
  /// @param ids The ids to look in.
  /// @return True when the id parsed and is in the set; false for an
  ///     absent or unreadable id.
  auto is_among(const std::set<OwnershipId>& ids) const -> bool;

  /// @brief Compares every field.
  friend auto operator==(const PremiumEntitlement&, const PremiumEntitlement&)
      -> bool = default;

 private:
  /// @brief The fields as read; set once at construction.
  Facts facts_;
};

/// @brief One SKU of a category product, with the entitlements it grants.
///
/// Immutable; may be shared between threads.
class PremiumSku {
 public:
  /// @brief The contentCollections branding that marks the SKU Premium
  /// streaming includes.
  static constexpr std::string_view premium_stream_branding =
      "PS_PLUS_STREAM_PS5";

  /// @brief Keeps one SKU.
  ///
  /// @param id The SKU id; std::nullopt when absent or malformed.
  /// @param premium_stream_branded Whether one of its content collections
  ///     carries premium_stream_branding.
  /// @param entitlements The entitlements it grants, in PS's order.
  PremiumSku(std::optional<SkuId> id, bool premium_stream_branded,
             std::vector<PremiumEntitlement> entitlements) noexcept;

  /// @brief The SKU id.
  ///
  /// @return The id, or std::nullopt when absent or malformed.
  auto id() const noexcept -> const std::optional<SkuId>& { return id_; }
  /// @brief Whether Premium streaming includes the SKU.
  ///
  /// @return True when a content collection is branded PS_PLUS_STREAM_PS5.
  auto is_premium_stream_branded() const noexcept -> bool {
    return premium_stream_branded_;
  }
  /// @brief The entitlements it grants.
  ///
  /// @return The entitlements in PS's order, valid while this SKU lives.
  auto entitlements() const noexcept -> const std::vector<PremiumEntitlement>& {
    return entitlements_;
  }
  /// @brief The first accepted entitlement.
  ///
  /// @return The first entitlement that is a streamable full game, or
  ///     std::nullopt when none is.
  auto first_streamable_full_game() const -> std::optional<PremiumEntitlement>;
  /// @brief Whether the user holds everything this SKU grants.
  ///
  /// A SKU whose entitlements name no id at all counts as owned. Otherwise only
  /// the PSGD entitlements that name an id are checked: there must be at least
  /// one and every one must be owned and active; an unreadable id is never
  /// owned.
  ///
  /// @param owned_and_active The ids of the user's active PS5
  ///     entitlements.
  /// @return True when the SKU is wholly owned.
  auto is_wholly_owned_given(
      const std::set<OwnershipId>& owned_and_active) const -> bool;

  /// @brief Compares every field.
  friend auto operator==(const PremiumSku&, const PremiumSku&)
      -> bool = default;

 private:
  /// @brief The SKU id.
  std::optional<SkuId> id_;
  /// @brief Whether Premium streaming includes the SKU.
  bool premium_stream_branded_;
  /// @brief The entitlements in PS's order.
  std::vector<PremiumEntitlement> entitlements_;
};

/// @brief One product of a store streaming category.
///
/// Immutable; may be shared between threads.
class PremiumProduct {
 public:
  /// @brief One accepted entitlement and the SKU it came from.
  struct Offer {
    /// @brief The SKU that grants it.
    std::optional<SkuId> sku;
    /// @brief The accepted entitlement.
    PremiumEntitlement entitlement;

    /// @brief Compares both fields.
    friend auto operator==(const Offer&, const Offer&) -> bool = default;
  };

  /// @brief The fields read from one category product.
  struct Facts {
    /// @brief The product id; the identity of a Premium tile.
    ProductId id;
    /// @brief The display name; empty when PS sent none.
    std::string name{};
    /// @brief The URL of the product's MASTER image; empty when none.
    std::string master_image_url{};
    /// @brief The concept the product belongs to; presentation only.
    std::optional<ConceptId> concept_id{};
    /// @brief The SKUs in PS's order.
    std::vector<PremiumSku> skus{};

    /// @brief Compares every field.
    friend auto operator==(const Facts&, const Facts&) -> bool = default;
  };

  /// @brief Keeps one product.
  ///
  /// @param facts The fields as read.
  explicit PremiumProduct(Facts facts) noexcept;

  /// @brief The fields as read.
  ///
  /// @return The fields, valid while this product lives.
  auto facts() const noexcept -> const Facts& { return facts_; }
  /// @brief The product id.
  ///
  /// @return The id, valid while this product lives.
  auto id() const noexcept -> const ProductId& { return facts_.id; }
  /// @brief The SKUs in PS's order.
  ///
  /// @return The SKUs, valid while this product lives.
  auto skus() const noexcept -> const std::vector<PremiumSku>& {
    return facts_.skus;
  }
  /// @brief What Premium includes.
  ///
  /// Takes the first SKU branded PS_PLUS_STREAM_PS5 and its first accepted
  /// entitlement; other SKUs are never tried.
  ///
  /// @return The offer, or std::nullopt when no SKU is branded or the
  ///     first branded SKU has no accepted entitlement.
  auto included_offer() const -> std::optional<Offer>;
  /// @brief Whether the product holds more than one streamable game.
  ///
  /// @return True when streamable_children has more than one entry.
  auto is_bundle() const noexcept -> bool;
  /// @brief Every accepted entitlement across all SKUs, once each.
  ///
  /// Entitlements without a parseable id are skipped; a repeated id keeps
  /// its first SKU.
  ///
  /// @return The children in SKU then entitlement order; possibly empty.
  auto streamable_children() const -> std::vector<Offer>;
  /// @brief The offer a "for sale" tile launches.
  ///
  /// @return The first of streamable_children, or std::nullopt when there
  ///     is none.
  auto offer_for_sale() const -> std::optional<Offer>;
  /// @brief Whether the user owns the product.
  ///
  /// @param owned_and_active The ids of the user's active PS5
  ///     entitlements.
  /// @return True when at least one SKU is wholly owned.
  auto is_owned_given(const std::set<OwnershipId>& owned_and_active) const
      -> bool;
  /// @brief Whether the product offers a streamable version of a feature
  /// that the user does not hold.
  ///
  /// An entitlement whose id is unreadable is never offered.
  ///
  /// @param feature The feature asked about, such as Feature::trial.
  /// @param owned_and_active The ids of the user's active PS5
  ///     entitlements.
  /// @return True when some SKU grants such an entitlement whose id is not
  ///     held.
  auto offers(PremiumEntitlement::Feature feature,
              const std::set<OwnershipId>& owned_and_active) const -> bool;

  /// @brief Compares every field.
  friend auto operator==(const PremiumProduct&, const PremiumProduct&)
      -> bool = default;

 private:
  /// @brief The fields as read; set once at construction.
  Facts facts_;
};

/// @brief How a category scan went. Plain data.
struct CategoryScan {
  /// @brief The totalNumProducts the last page stated.
  std::size_t server_total{};
  /// @brief How many products the pages carried, kept or not; the raw
  /// server offset of the next page.
  std::size_t products_read{};
  /// @brief Whether the scan reached the end: an empty page, or as many
  /// products as the server total.
  bool complete{};

  /// @brief Compares every field.
  friend auto operator==(const CategoryScan&, const CategoryScan&)
      -> bool = default;
};

/// @brief The Premium streaming category for one locale and age.
///
/// Immutable; the catalog holds it behind a shared pointer to const and
/// reads it from several threads.
class PremiumCategory {
 public:
  /// @brief Wall-clock time in milliseconds since the Unix epoch.
  using Time = std::chrono::sys_time<std::chrono::milliseconds>;
  /// @brief How the scan went.
  using Scan = CategoryScan;

  /// @brief The fewest distinct included title numbers a real Premium
  /// category has; fewer is refused as malformed, a sign that the store
  /// changed shape rather than a real answer.
  static constexpr std::size_t fewest_plausible = 80;

  /// @brief Keeps one scan of the category.
  ///
  /// @param locale The country and language it was asked with.
  /// @param products The products kept, in server order.
  /// @param scan How the scan went.
  /// @param fetched_at When the scan finished.
  PremiumCategory(StoreLocale locale, std::vector<PremiumProduct> products,
                  Scan scan, Time fetched_at) noexcept;

  /// @brief The country and language the category was asked with.
  ///
  /// @return The locale, valid while the category lives.
  auto locale() const noexcept -> const StoreLocale& { return locale_; }
  /// @brief The products kept.
  ///
  /// @return The products in server order, valid while the category lives.
  auto products() const noexcept -> const std::vector<PremiumProduct>& {
    return products_;
  }
  /// @brief How the scan went.
  ///
  /// @return The scan, valid while the category lives.
  auto scan() const noexcept -> const Scan& { return scan_; }
  /// @brief Whether the scan reached the end of the category.
  ///
  /// @return The scan's complete flag.
  auto is_complete() const noexcept -> bool { return scan_.complete; }
  /// @brief When the scan finished.
  ///
  /// @return Wall-clock time in milliseconds.
  auto fetched_at() const noexcept -> Time { return fetched_at_; }
  /// @brief How many products Premium includes.
  ///
  /// @return The number of products with an included_offer.
  auto included_count() const -> std::size_t;
  /// @brief Whether the scan looks like a real Premium category.
  ///
  /// @return True when complete and the included products carry at least
  ///     PremiumCategory::fewest_plausible distinct title numbers.
  auto is_plausible() const -> bool;

  /// @brief Compares every field.
  friend auto operator==(const PremiumCategory&, const PremiumCategory&)
      -> bool = default;

 private:
  /// @brief The country and language asked with.
  StoreLocale locale_;
  /// @brief The products kept, in server order.
  std::vector<PremiumProduct> products_;
  /// @brief How the scan went.
  Scan scan_;
  /// @brief When the scan finished; wall-clock milliseconds.
  Time fetched_at_;
};

}  // namespace libchiaking::catalog::cloud_stream
