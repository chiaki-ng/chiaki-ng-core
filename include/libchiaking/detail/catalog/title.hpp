/// @file
/// @brief Declares Title, a game as PS lists it, with ConceptId and
/// ListStreamability.
#pragma once

#include "libchiaking/detail/catalog/platform.hpp"
#include "libchiaking/detail/catalog/product_id.hpp"
#include "libchiaking/detail/catalog/store_page.hpp"
#include "libchiaking/detail/catalog/title_number.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/strong_text.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::catalog {

/// @brief What PS's lists say about streaming a title.
enum class ListStreamability : std::uint8_t {
  /// @brief PS lists the title as streaming.
  listed,
  /// @brief PS's lists do not say either way.
  unknown,
  /// @brief PS marks the title as not streaming.
  marked_unstreamable
};

/// @brief PS's number for a concept, which groups every edition and platform
/// version of one game.
class ConceptId {
 public:
  /// @brief Most digits accepted.
  static constexpr std::size_t limit = 24;

  /// @brief Checks a concept number written as text and wraps it.
  ///
  /// @param digits The number as PS gave it.
  /// @return The concept, or nothing unless the text is 1 to ConceptId::limit
  ///     decimal digits and not all zeros.
  [[nodiscard]] static auto parse(std::string_view digits)
      -> std::optional<ConceptId>;
  /// @brief Wraps a concept number.
  ///
  /// @param number The number as PS gave it.
  /// @return The concept, or nothing when the number is zero.
  [[nodiscard]] static auto of(std::uint64_t number)
      -> std::optional<ConceptId>;

  /// @brief The concept number in decimal digits, as it was given.
  ///
  /// @return The digits; valid as long as this object.
  auto text() const noexcept -> const std::string& { return text_.text(); }

  /// @brief Compares the digits; leading zeros are significant.
  friend auto operator==(const ConceptId&, const ConceptId&) -> bool = default;
  /// @brief Orders by text, so concepts can key a map or set.
  friend auto operator<=>(const ConceptId&, const ConceptId&)
      -> std::strong_ordering = default;

 private:
  struct Digits {
    /// @brief Whether a string is a valid concept number.
    ///
    /// @param digits The candidate digits.
    /// @return True when the digits are 1 to limit ASCII digits that are not
    ///     all zeros; false otherwise.
    static auto admits(std::string_view digits) noexcept -> bool;
  };
  using Text = StrongText<ConceptId, Digits>;

  explicit ConceptId(Text text) noexcept : text_(std::move(text)) {}

  Text text_;
};

/// @brief A game as PS lists it: its product, name, platform, service, art
/// and the other product ids it answers to.
///
/// A title is immutable; the with and without members return changed copies.
class Title {
 public:
  /// @brief Why the facts do not describe a title.
  enum class Invalid : std::uint8_t {
    /// @brief The name is empty or only white space.
    name,
  };

  /// @brief The facts a Title is built from.
  struct Facts {
    /// @brief The product id the title is listed under.
    ProductId product;
    /// @brief The display name; must not be blank.
    std::string name{};
    /// @brief The console generation the title was made for.
    Platform platform{};
    /// @brief Which streaming service carries the title.
    ServiceGeneration service{};
    /// @brief What PS's lists say about streaming it.
    ListStreamability streamability{ListStreamability::unknown};
    /// @brief The concept the title belongs to, when known.
    std::optional<ConceptId> concept_id{};
    /// @brief Address of the cover image; empty when there is none.
    std::string cover_url{};
    /// @brief Address of the landscape cover; empty when there is none.
    std::string landscape_cover_url{};
    /// @brief The store page of the title's concept, when known.
    std::optional<StorePage> concept_page{};
    /// @brief Other product ids of the same title, such as editions shown
    /// as one game.
    std::vector<ProductId> aliases{};

    /// @brief Compares every fact.
    friend auto operator==(const Facts&, const Facts&) -> bool = default;
  };

  /// @brief Builds a title from facts.
  ///
  /// Aliases that repeat the product id or each other are dropped.
  ///
  /// @param facts The facts about the title.
  /// @return The title.
  /// @retval Title::Invalid::name The name is empty or only white space.
  [[nodiscard]] static auto from(Facts facts) -> Expected<Title, Invalid>;

  /// @brief Every fact about the title at once, as Title::from accepted
  /// them, for building a changed copy or a related title.
  ///
  /// @return A reference into this title, valid while it lives.
  auto facts() const noexcept -> const Facts& { return facts_; }

  /// @brief The product id the title is listed under.
  ///
  /// @return The product id; valid as long as this object.
  auto product_id() const noexcept -> const ProductId& {
    return facts_.product;
  }
  /// @brief The display name.
  ///
  /// @return The name; valid as long as this object.
  auto name() const noexcept -> const std::string& { return facts_.name; }
  /// @brief The console generation the title was made for.
  ///
  /// @return The platform of the title.
  auto platform() const noexcept -> Platform { return facts_.platform; }
  /// @brief Which streaming service carries the title.
  ///
  /// @return The streaming service generation.
  auto service() const noexcept -> ServiceGeneration { return facts_.service; }
  /// @brief What PS's lists say about streaming it.
  ///
  /// @return The streamability recorded from the lists.
  auto streamability() const noexcept -> ListStreamability {
    return facts_.streamability;
  }
  /// @brief The concept the title belongs to, when known.
  ///
  /// @return The concept id, or nothing when unknown; valid as long as this
  ///     object.
  auto concept_id() const noexcept -> const std::optional<ConceptId>& {
    return facts_.concept_id;
  }
  /// @brief Address of the cover image; empty when there is none.
  ///
  /// @return The cover address, or an empty string when there is none; valid as
  ///     long as this object.
  auto cover_url() const noexcept -> const std::string& {
    return facts_.cover_url;
  }
  /// @brief Address of the landscape cover; empty when there is none.
  ///
  /// @return The landscape cover address, or an empty string when there is
  /// none;
  ///     valid as long as this object.
  auto landscape_cover_url() const noexcept -> const std::string& {
    return facts_.landscape_cover_url;
  }
  /// @brief The store page of the title's concept, when known.
  ///
  /// @return The store page, or nothing when unknown; valid as long as this
  ///     object.
  auto concept_page() const noexcept -> const std::optional<StorePage>& {
    return facts_.concept_page;
  }
  /// @brief Other product ids of the same title; never contains the title's
  /// own product id.
  ///
  /// @return The aliases; valid as long as this object.
  auto aliases() const noexcept -> std::span<const ProductId> {
    return facts_.aliases;
  }

  /// @brief The title number inside the listed product id, when it contains
  /// one.
  ///
  /// @return The title number, or nothing when the product id contains none.
  auto title_number() const -> std::optional<TitleNumber>;
  /// @brief Whether the product is the title's own id or one of its aliases.
  ///
  /// @param product The product id to test.
  /// @return True when it equals the title's product id or one of its aliases;
  ///     false otherwise.
  auto is_known_as(const ProductId& product) const noexcept -> bool;
  /// @brief Returns a copy with a different streamability.
  ///
  /// @param streamability The streamability the copy takes.
  /// @return The amended title; this object is unchanged.
  auto with_streamability(ListStreamability streamability) const -> Title;
  /// @brief Returns a copy that also answers to another product id; unchanged
  /// when it already does.
  ///
  /// @param alias The product id to add.
  /// @return The amended title; this object is unchanged.
  auto with_alias(const ProductId& alias) const -> Title;
  /// @brief Returns a copy that no longer answers to an alias; the title's
  /// own product id cannot be removed this way.
  ///
  /// @param alias The alias to remove.
  /// @return The amended title; this object is unchanged.
  auto without_alias(const ProductId& alias) const -> Title;

  /// @brief Compares every fact.
  friend auto operator==(const Title&, const Title&) -> bool = default;

 private:
  explicit Title(Facts facts) noexcept;

  Facts facts_;
};

}  // namespace libchiaking::catalog
