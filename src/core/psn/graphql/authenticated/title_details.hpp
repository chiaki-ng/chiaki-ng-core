/// @file
/// @brief Declares TitleDetails, a game concept as the store describes it to a
/// signed-in user: its summary, its products (editions) with their offers, and
/// which of them the user owns.
///
/// It is assembled from up to two GraphQL replies: the concept (or the product
/// asked for, with its concept), then optionally the concept's pricing, which
/// adds every purchasable product and the user's ownership.
#pragma once

#include "src/core/psn/graphql/content_rating.hpp"
#include "src/core/psn/graphql/title_media.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::psn::graphql {

/// @brief What a concept's or product's compatibility notices say about cloud
/// streaming. Plain data.
struct TitleStreaming {
  /// @brief Whether any compatibility notice was listed at all.
  bool notices{};
  /// @brief Whether a `STREAMING_SUPPORTED` notice was listed.
  bool listed{};
  /// @brief Whether a `STREAMING_SUPPORTED` notice targets `PS5` or `DWC`;
  /// such a notice wins over one that does not.
  bool supported{};
  /// @brief The chosen streaming notice's target platforms.
  std::vector<std::string> platforms;
  /// @brief The chosen streaming notice's value.
  std::string value;
};

/// @brief One call to action (offer) the store shows for a product: buy,
/// download, play with a subscription, pre-order. Plain data; text fields are
/// empty when PS omitted them.
struct TitleOffer {
  /// @brief The call to action's `type`.
  std::string cta_type;
  /// @brief Its `action.type`.
  std::string action_type;
  /// @brief The first `skuId` action parameter.
  std::string sku_id;
  /// @brief The first `rewardId` action parameter.
  std::string reward_id;
  /// @brief Every `entitlementIds` action parameter value.
  std::vector<std::string> entitlement_ids;
  /// @brief The first `isEntitledViaSubscription` parameter that reads `true`
  /// or `false`; nothing otherwise.
  std::optional<bool> entitled_via_subscription;
  /// @brief The `price.basePrice` as shown.
  std::string base_price;
  /// @brief The `price.discountedPrice` as shown.
  std::string discounted_price;
  /// @brief The `price.currencyCode`.
  std::string currency_code;
  /// @brief The `price.basePriceValue`, in the currency's minor unit; 0 when
  /// absent.
  std::int64_t base_price_value{};
  /// @brief The `price.discountedValue`, in the minor unit; 0 when absent.
  std::int64_t discounted_value{};
  /// @brief The `price.isFree` flag.
  bool is_free{};
  /// @brief The `price.isTiedToSubscription` flag.
  bool tied_to_subscription{};
  /// @brief The `meta.preOrder` flag.
  bool pre_order{};
  /// @brief The `price.serviceBranding`, such as `PS_PLUS`.
  std::vector<std::string> service_branding;
  /// @brief The `cloudStreamingMessage` shown with the offer.
  std::string cloud_streaming_message;
  /// @brief The non-empty `meta.ineligibilityReasons[].type`.
  std::vector<std::string> ineligibility_reasons;
};

/// @brief One product (edition) of a concept. Plain data; text fields are
/// empty when PS omitted them.
struct TitleProduct {
  /// @brief The product id; never empty.
  std::string id;
  /// @brief The product's name.
  std::string name;
  /// @brief The `npTitleId`, such as `PPSA01234_00`.
  std::string np_title_id;
  /// @brief The platforms it runs on.
  std::vector<std::string> platforms;
  /// @brief Its release date as PS wrote it.
  std::string release_date;
  /// @brief Its `storeDisplayClassification`, such as `FULL_GAME`.
  std::string classification;
  /// @brief Its `edition.name`.
  std::string edition;
  /// @brief What its compatibility notices say about streaming.
  TitleStreaming streaming;
  /// @brief Its first `STREAMING_COMPATIBILITY_NOTICE` description.
  std::string streaming_notice;
  /// @brief Whether the user owns it, set by TitleDetails::settle(); false
  /// until then.
  bool owned{};
  /// @brief Its offers, in reply order.
  std::vector<TitleOffer> offers;
  /// @brief Its media.
  TitleMedia media;
};

/// @brief A concept with its products and the user's ownership.
///
/// Its concept id is always 1 to 24 digits. Products are kept once per product
/// id, later replies filling only what earlier ones left empty (offers are
/// replaced when the later reply has some). It is priced once settle() has
/// recorded the ownership. A value; not shared between threads while being
/// assembled.
class TitleDetails {
 public:
  /// @brief The concept as a whole. Plain data.
  struct Summary {
    /// @brief The concept id, digits.
    std::string concept_id;
    /// @brief The concept's name.
    std::string name;
    /// @brief The publisher's name.
    std::string publisher;
    /// @brief The release date as PS wrote it.
    std::string release_date;
    /// @brief The localised genres.
    std::vector<std::string> genres;
    /// @brief The age rating.
    ContentRating rating;
    /// @brief What the concept's compatibility notices say about streaming.
    TitleStreaming streaming;
    /// @brief The concept's media.
    TitleMedia media;
  };

  /// @brief What the user holds of the concept. Plain data.
  struct Ownership {
    /// @brief The ids `ownedTitles` named: product, title, entitlement or
    /// SKU ids, or bare strings.
    std::vector<std::string> owned;
    /// @brief The ids `selectableProducts.purchasedTitles` named.
    std::vector<std::string> purchased;
  };

  /// @brief Starts the details of a concept.
  ///
  /// @param summary The concept.
  /// @return The details, unpriced and without products; nothing when the
  ///     concept id is not valid_concept_id().
  [[nodiscard]] static auto of(Summary summary) -> std::optional<TitleDetails>;
  /// @brief Reduces a SKU id to its product id.
  ///
  /// @param id A product or SKU id, such as
  ///     `EP9000-PPSA01234_00-GAME000000000000-E001`.
  /// @return The id without a final `-` and an upper-case letter and three
  ///     digits, when it has exactly three `-`; `id` unchanged otherwise.
  static auto sku_base(std::string_view id) -> std::string_view;
  /// @brief Checks a concept id.
  ///
  /// @param id The id.
  /// @return True for 1 to 24 ASCII digits.
  static auto valid_concept_id(std::string_view id) noexcept -> bool;

  /// @brief Reads the concept id.
  ///
  /// @return The id.
  auto concept_id() const noexcept -> const std::string& {
    return summary_.concept_id;
  }
  /// @brief Reads the concept's name.
  ///
  /// @return The name.
  auto name() const noexcept -> const std::string& { return summary_.name; }
  /// @brief Reads the publisher.
  ///
  /// @return The name.
  auto publisher() const noexcept -> const std::string& {
    return summary_.publisher;
  }
  /// @brief Reads the release date.
  ///
  /// @return The date as PS wrote it.
  auto release_date() const noexcept -> const std::string& {
    return summary_.release_date;
  }
  /// @brief Reads the genres.
  ///
  /// @return The genres.
  auto genres() const noexcept -> const std::vector<std::string>& {
    return summary_.genres;
  }
  /// @brief Reads the age rating.
  ///
  /// @return The rating.
  auto rating() const noexcept -> const ContentRating& {
    return summary_.rating;
  }
  /// @brief Reads the concept's streaming notices.
  ///
  /// @return What they say.
  auto streaming() const noexcept -> const TitleStreaming& {
    return summary_.streaming;
  }
  /// @brief Reads the concept's media.
  ///
  /// @return The media.
  auto media() const noexcept -> const TitleMedia& { return summary_.media; }
  /// @brief Tells whether ownership has been settled.
  ///
  /// @return True after settle().
  auto priced() const noexcept -> bool { return priced_; }
  /// @brief Reads the owned ids.
  ///
  /// @return The ids; empty until settled.
  auto owned_titles() const noexcept -> const std::vector<std::string>& {
    return ownership_.owned;
  }
  /// @brief Reads the purchased ids.
  ///
  /// @return The ids; empty until settled.
  auto purchased_titles() const noexcept -> const std::vector<std::string>& {
    return ownership_.purchased;
  }
  /// @brief Reads the products.
  ///
  /// @return The products, in the order first seen.
  auto products() const noexcept -> const std::vector<TitleProduct>& {
    return products_;
  }

  /// @brief Adds a product, or merges it into the one with the same id.
  ///
  /// @param product The product.
  void add(TitleProduct product);
  /// @brief Tells whether a product is listed.
  ///
  /// @param product_id A product or SKU id.
  /// @return True when a product has its sku_base().
  auto lists(std::string_view product_id) const -> bool;
  /// @brief Replaces a product's offers.
  ///
  /// @param product_id A product or SKU id.
  /// @param offers The offers; nothing changes when empty or the product is
  ///     not listed.
  void offer(std::string_view product_id, std::vector<TitleOffer> offers);
  /// @brief Records what the user holds and marks each product owned whose
  /// id, or whose title id, an owned or purchased id names (a SKU id counting
  /// as its product).
  ///
  /// @param ownership The ownership.
  void settle(Ownership ownership);

 private:
  /// @brief The longest concept id, in digits.
  static constexpr std::size_t concept_id_max_length = 24;

  /// @brief Wraps a checked summary.
  ///
  /// @param summary The concept.
  explicit TitleDetails(Summary summary) noexcept
      : summary_(std::move(summary)) {}

  /// @brief Fills a field only when it is empty.
  ///
  /// @tparam Field A type with `empty()`.
  /// @param target The field.
  /// @param source The value, moved in when used.
  template <class Field>
  static void fill(Field& target, Field&& source) {
    if (target.empty()) target = std::move(source);
  }
  /// @brief Fills each empty media piece.
  ///
  /// @param target The media.
  /// @param source The other media.
  static void fill_media(TitleMedia& target, TitleMedia&& source);
  /// @brief Merges a later reply's product into an earlier one.
  ///
  /// @param target The product kept.
  /// @param source The later product.
  static void merge(TitleProduct& target, TitleProduct&& source);
  /// @brief Finds a product.
  ///
  /// @param product_id A product or SKU id.
  /// @return The product with its sku_base(), or the end.
  auto edition(std::string_view product_id)
      -> std::vector<TitleProduct>::iterator;
  /// @brief Tells whether the ownership covers a product.
  ///
  /// @param product The product.
  /// @return True when an owned or purchased id names it.
  auto owns(const TitleProduct& product) const -> bool;

  /// @brief The concept.
  Summary summary_;
  /// @brief Whether settle() has run.
  bool priced_{};
  /// @brief What the user holds.
  Ownership ownership_;
  /// @brief The products.
  std::vector<TitleProduct> products_;
};

}  // namespace libchiaking::psn::graphql
