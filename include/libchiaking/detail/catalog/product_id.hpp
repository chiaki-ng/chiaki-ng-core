/// @file
/// @brief Declares ProductId, the PlayStation Store identifier of a product.
#pragma once

#include "libchiaking/detail/catalog/platform.hpp"
#include "libchiaking/detail/catalog/title_number.hpp"
#include "libchiaking/detail/strong_text.hpp"

#include <compare>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::catalog {

/// @brief The PlayStation Store identifier of one product, such as one edition
/// of a game.
///
/// It is the key by which titles are listed, looked up and asked about. Only
/// the character set and length are checked; the identifier need not have the
/// store's full product form.
class ProductId {
 public:
  /// @brief Longest identifier accepted, in bytes.
  static constexpr std::size_t limit = 64;

  /// @brief Checks an identifier and wraps it.
  ///
  /// @param text The identifier as PS gave it.
  /// @return The identifier, or nothing unless the text is 1 to
  ///     ProductId::limit characters, each a letter, digit, hyphen or
  ///     underscore.
  [[nodiscard]] static auto parse(std::string_view text)
      -> std::optional<ProductId>;

  /// @brief The identifier as PS gave it.
  ///
  /// @return The identifier text, valid while this object lives.
  auto text() const noexcept -> const std::string& { return text_.text(); }
  /// @brief The title number inside the identifier, when it contains one.
  ///
  /// @return The title number, or nothing when the identifier contains none.
  auto title_number() const -> std::optional<TitleNumber>;
  /// @brief The key that groups identifiers of the same title; see TitleKey.
  ///
  /// @return The key, or nothing when TitleKey::of finds none for the
  /// identifier.
  auto title_key() const -> std::optional<TitleKey>;
  /// @brief The console generation the identifier names: PS5 for a PPSA title
  /// number, PS4 for CUSA, otherwise nothing.
  ///
  /// @return PS5 or PS4, or nothing when the identifier names neither.
  auto platform() const -> std::optional<Platform>;
  /// @brief Returns the identifier without a trailing SKU suffix.
  ///
  /// A SKU suffix is a final hyphen followed by one capital letter and three
  /// digits, on an identifier with exactly three hyphens. An identifier
  /// without one is returned unchanged.
  ///
  /// @return The identifier with the SKU suffix removed, or an equal copy when
  /// it
  ///     has none.
  auto without_sku() const -> ProductId;
  /// @brief Whether the identifier ends in a SKU suffix; see
  /// ProductId::without_sku.
  ///
  /// @return True when the identifier ends in a SKU suffix; false otherwise.
  auto has_sku() const noexcept -> bool;
  /// @brief Whether a product in a storefront reply is the one this
  /// identifier asked about.
  ///
  /// @param storefront_product The product the store answered with.
  /// @return True when the two are equal, or when this identifier is the
  ///     store's followed by a hyphen and a suffix.
  auto is_answered_by(const ProductId& storefront_product) const noexcept
      -> bool;

  /// @brief Compares the identifiers byte for byte.
  friend auto operator==(const ProductId&, const ProductId&) -> bool = default;
  /// @brief Orders by text, so identifiers can key a map or set.
  friend auto operator<=>(const ProductId&, const ProductId&)
      -> std::strong_ordering = default;
  /// @brief Compares the identifier with plain text.
  friend auto operator==(const ProductId& id, std::string_view text) -> bool {
    return id.text_ == text;
  }

 private:
  struct Spelling {
    /// @brief Checks identifier text against the accepted spelling.
    ///
    /// @param text The candidate identifier.
    /// @return True when the text is 1 to ProductId::limit characters, each a
    /// letter,
    ///     digit, hyphen or underscore; false otherwise.
    static auto admits(std::string_view text) noexcept -> bool;
  };
  using Text = StrongText<ProductId, Spelling>;

  explicit ProductId(Text text) noexcept : text_(std::move(text)) {}

  auto sku_dash() const noexcept -> std::optional<std::size_t>;

  Text text_;
};

}  // namespace libchiaking::catalog
