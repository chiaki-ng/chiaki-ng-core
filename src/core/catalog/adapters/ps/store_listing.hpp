/// @file
/// @brief Declares the store reply shapes the catalog reads (StoreImage,
/// ListedProduct, ContainerRow and the category product tree) and
/// ps::StoreListing, the readers that turn store rows into ListedEntry
/// values.
///
/// PS's store replies are loosely typed: fields come as strings or numbers,
/// are sometimes null, and lists may hold nulls. Every field is therefore read
/// tolerantly, and a row that cannot be read is skipped rather than failing
/// the whole reply.
#pragma once

#include "src/core/catalog/model/list/raw_listing.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <libchiaking/detail/catalog/title.hpp>

#include <glaze/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::catalog::ps {

/// @brief One image a store row lists. Plain data.
struct StoreImage {
  /// @brief The image URL.
  json::ps::Text url;
  /// @brief The store's numeric image type; see StoreListing's cover types.
  json::ps::Tolerant<json::ps::Number> type;
};

/// @brief A tolerant list of names, such as the devices a product plays on.
using StoreNames = json::ps::Tolerant<std::vector<json::ps::Text>>;

/// @brief A tolerant list whose elements are each read tolerantly.
///
/// @tparam Element The element shape.
template <class Element>
using StoreList = json::ps::Tolerant<std::vector<json::ps::Tolerant<Element>>>;
/// @brief The images of a store row.
using ImageList = StoreList<StoreImage>;

/// @brief One product row of a store list, as the Kamaji shelves, the public
/// store walk and the imagic lists send it. Plain data.
struct ListedProduct {
  /// @brief Fields are read under their camelCase names.
  using keys = json::ps::CamelCaseKeys;
  /// @brief The row id; the product id in a container row.
  json::ps::Text id;
  /// @brief The product id in an imagic list row.
  json::ps::Text product_id;
  /// @brief The display name.
  json::ps::Text name;
  /// @brief The concept id, as a string or a number.
  std::optional<glz::raw_json> concept_id;
  /// @brief The devices it plays on, such as "PS5" and "PS4".
  StoreNames device;
  /// @brief The streaming flag, as sent.
  std::optional<glz::raw_json> streaming_supported;
  /// @brief The row's stated cover URL.
  json::ps::Text image_url;
  /// @brief The row's landscape cover URL.
  json::ps::Text landscape_image_url;
  /// @brief The concept's store page URL.
  json::ps::Text concept_url;
  /// @brief The row's images.
  ImageList images;
};

/// @brief The identity of one row of a store container, read before deciding
/// whether it is a product or a nested container. Plain data.
struct ContainerRow {
  /// @brief The row id.
  json::ps::Text id;
  /// @brief The row's URL, for a nested container.
  json::ps::Text url;
  /// @brief The row's container type; empty for a product.
  json::ps::Text container_type;
};

/// @brief One entitlement of a category SKU. Plain data.
struct StoreEntitlement {
  /// @brief Fields are read under their camelCase names.
  using keys = json::ps::CamelCaseKeys;
  /// @brief The entitlement id.
  json::ps::Text id;
  /// @brief The packageType, such as PSGD.
  json::ps::Text package_type;
  /// @brief The featureType string, such as FULL_GAME.
  json::ps::Text feature_type;
  /// @brief streamingSupported as sent; only true and false are trusted.
  std::optional<glz::raw_json> streaming_supported;
};

/// @brief One content collection of a category SKU. Plain data.
struct StoreCollection {
  /// @brief The collection's branding, such as PS_PLUS_STREAM_PS5.
  json::ps::Text branding;
};

/// @brief One SKU of a category product. Plain data.
struct StoreSku {
  /// @brief Fields are read under their camelCase names.
  using keys = json::ps::CamelCaseKeys;
  /// @brief The SKU id.
  json::ps::Text id;
  /// @brief The entitlements the SKU grants.
  StoreList<StoreEntitlement> entitlements;
  /// @brief The content collections the SKU belongs to.
  StoreList<StoreCollection> content_collections;
};

/// @brief One product of a store category with its SKUs. Plain data.
struct StoreProduct {
  /// @brief The product id.
  json::ps::Text id;
  /// @brief The SKUs.
  StoreList<StoreSku> skus;
};

/// @brief The readers shared by the store adapters. Stateless; every member is
/// static and safe to call from any thread.
class StoreListing {
 public:
  /// @brief The longest image URL kept, in bytes; longer ones are cut.
  static constexpr std::size_t image_url_limit = 1023;
  /// @brief The image type preferred as a cover.
  static constexpr std::int64_t preferred_cover = 10;
  /// @brief An image type used as a cover when there is no preferred one.
  static constexpr std::int64_t fallback_cover = 12;
  /// @brief The other image type used as a cover when there is no preferred
  /// one.
  static constexpr std::int64_t other_fallback_cover = 13;

  /// @brief Reads a product row into a list entry.
  ///
  /// Container rows (LegacyCloudStream and public store shelves) take the
  /// product from id and the cover from the images; imagic rows take the
  /// product from productId and the stated cover when there is one.
  ///
  /// @param product The row.
  /// @param shelf The list it came from.
  /// @return The entry, or std::nullopt when the product id does not parse.
  static auto listed_on(const ListedProduct& product, Shelf shelf)
      -> std::optional<ListedEntry>;
  /// @brief Picks a cover image.
  ///
  /// @param images The row's images.
  /// @param stated The row's stated cover URL.
  /// @return The first type 10 image, else the first type 12 or 13 image, else
  ///     the stated URL, cut to image_url_limit; empty when none.
  static auto cover(const ImageList& images, std::string_view stated)
      -> std::string;
  /// @brief Reads a concept id sent as a string or a number.
  ///
  /// @param stated The field as sent.
  /// @return The concept, or std::nullopt when absent, not positive or not a
  ///     concept id. Numbers beyond the 64-bit signed range are clamped to it.
  static auto concept_of(const std::optional<glz::raw_json>& stated)
      -> std::optional<ConceptId>;
  /// @brief Reads a scalar field as text.
  ///
  /// @param stated The field as sent.
  /// @return A string's contents (empty when it does not decode), any other
  ///     token's raw text, or std::nullopt when absent.
  static auto scalar_text(const std::optional<glz::raw_json>& stated)
      -> std::optional<std::string>;
  /// @brief Reads a flag.
  ///
  /// @param stated The field as sent.
  /// @return True only for the token true, false for any other token,
  ///     std::nullopt when absent.
  static auto decided(const std::optional<glz::raw_json>& stated)
      -> std::optional<bool>;
  /// @brief Whether a list of names holds one exactly.
  ///
  /// @param listed The names.
  /// @param wanted The name.
  /// @return True when present.
  static auto names(const StoreNames& listed, std::string_view wanted) -> bool;
  /// @brief Reads a container row's identity.
  ///
  /// @param link The row as sent.
  /// @return The row; empty fields when it does not parse.
  static auto row_of(const glz::raw_json& link) -> ContainerRow;
  /// @brief Reads a raw container row into a list entry.
  ///
  /// @param link The row as sent.
  /// @param shelf The list it came from.
  /// @return The entry, or std::nullopt when the row does not parse or its
  ///     product id does not.
  static auto listed_on(const glz::raw_json& link, Shelf shelf)
      -> std::optional<ListedEntry>;

 private:
  /// @brief The first non-empty image of either type.
  ///
  /// @param images The images.
  /// @param first One image type.
  /// @param second The other image type.
  /// @return The URL, or an empty view when none matches.
  static auto image_of(const ImageList& images, std::int64_t first,
                       std::int64_t second) -> std::string_view;
  /// @brief Reads a numeric concept id.
  ///
  /// @param number The number.
  /// @return The concept, clamped to the 64-bit signed range; std::nullopt for
  ///     zero, a negative value, NaN or a value below 1.
  static auto concept_numbered(const json::ps::Number& number)
      -> std::optional<ConceptId>;
};

}  // namespace libchiaking::catalog::ps
