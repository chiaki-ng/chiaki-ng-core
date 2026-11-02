/// @file
/// @brief Declares StorePage, a checked address of a page on the PlayStation
/// Store website.
#pragma once

#include "libchiaking/detail/catalog/product_id.hpp"
#include "libchiaking/detail/catalog/store_locale.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace libchiaking::catalog {

/// @brief The address of a PlayStation Store web page for a product or a
/// concept, guaranteed to be on the store's host.
///
/// A frontend opens it in a browser so the user can buy a game or add it to the
/// library.
///
/// @see Catalog::store_page_of
class StorePage {
 public:
  /// @brief What the page describes.
  enum class Source : std::uint8_t {
    /// @brief One product's own page.
    product,
    /// @brief The page of the concept that groups a game's editions.
    concept_page,
  };

  /// @brief The scheme and host every store page starts with.
  static constexpr std::string_view origin = "https://store.playstation.com/";
  /// @brief Longest address accepted, in bytes.
  static constexpr std::size_t url_limit = 512;

  /// @brief Checks an address and wraps it.
  ///
  /// @param url The address as the store gave it.
  /// @param source What the page describes.
  /// @return The page, or nothing unless the address starts with
  ///     StorePage::origin, has a path, is at most StorePage::url_limit bytes
  ///     and uses only letters, digits and the characters -._~/?=&%:+.
  [[nodiscard]] static auto at(std::string_view url, Source source)
      -> std::optional<StorePage>;
  /// @brief Builds the address of a product's own page in a locale.
  ///
  /// No request is made, so the page is not known to exist.
  ///
  /// @param locale The store to open the page in.
  /// @param product The product to show.
  /// @return The page, or nothing when the product id does not have the
  ///     store's full 36-character product form.
  [[nodiscard]] static auto of_product(const StoreLocale& locale,
                                       const ProductId& product)
      -> std::optional<StorePage>;

  /// @brief The full address.
  ///
  /// @return The address, valid while this page lives.
  auto url() const noexcept -> const std::string& { return url_; }
  /// @brief What the page describes.
  ///
  /// @return The source given when the page was built.
  auto source() const noexcept -> Source { return source_; }

  /// @brief Compares address and source.
  friend auto operator==(const StorePage&, const StorePage&) -> bool = default;

 private:
  StorePage(std::string url, Source source) noexcept;

  static auto url_character(char value) noexcept -> bool;
  static auto store_product(std::string_view id) noexcept -> bool;

  std::string url_;
  Source source_;
};

}  // namespace libchiaking::catalog
