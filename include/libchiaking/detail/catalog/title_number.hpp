/// @file
/// @brief Declares TitleNumber and TitleKey, the two ways identifiers of the
/// same title are recognised.
#pragma once

#include "libchiaking/detail/catalog/platform.hpp"
#include "libchiaking/detail/strong_text.hpp"

#include <compare>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::catalog {

/// @brief A console title number: CUSA for PS4 or PPSA for PS5, followed by
/// digits.
///
/// Product ids, entitlement ids and launch identifiers of the same title share
/// its title number, which is how copies are matched to listings.
class TitleNumber {
 public:
  /// @brief Finds the title number inside an identifier.
  ///
  /// @param id Any identifier that may contain a title number.
  /// @return The first run of CUSA or PPSA followed by at least one digit,
  ///     with all the digits that follow; nothing when there is none.
  [[nodiscard]] static auto from(std::string_view id)
      -> std::optional<TitleNumber>;
  /// @brief The console generation an identifier names by the title number
  /// prefix it contains anywhere, digits or not.
  ///
  /// This is the one place the PPSA and CUSA prefixes are recognised. Never
  /// blocks; safe from any thread.
  ///
  /// @param text Any identifier: a product id, entitlement id or launch
  ///     identifier.
  /// @return Platform::ps5 when the text contains "PPSA", Platform::ps4 when
  ///     it contains "CUSA" but not "PPSA", and nothing otherwise.
  static auto platform_marked_in(std::string_view text) noexcept
      -> std::optional<Platform>;

  /// @brief The prefix and its digits, exactly as found.
  ///
  /// @return The prefix and digits, valid while this object lives.
  auto text() const noexcept -> const std::string& { return text_.text(); }
  /// @brief The console generation the prefix names: PS5 for PPSA, PS4
  /// otherwise.
  ///
  /// @return PS5 for a PPSA prefix, PS4 for a CUSA prefix.
  auto platform() const noexcept -> Platform;

  /// @brief Compares the title numbers byte for byte.
  friend auto operator==(const TitleNumber&, const TitleNumber&)
      -> bool = default;
  /// @brief Orders by text, so title numbers can key a map or set.
  friend auto operator<=>(const TitleNumber&, const TitleNumber&)
      -> std::strong_ordering = default;
  /// @brief Compares the title number with plain text.
  friend auto operator==(const TitleNumber& number, std::string_view text)
      -> bool {
    return number.text_ == text;
  }

 private:
  static constexpr std::string_view ps5_prefix = "PPSA";
  static constexpr std::string_view ps4_prefix = "CUSA";

  struct Prefixed {
    /// @brief Checks title number text against the accepted spelling.
    ///
    /// @param text The candidate title number.
    /// @return True when the text is a CUSA or PPSA prefix followed by one or
    /// more
    ///     digits and nothing else; false otherwise.
    static auto admits(std::string_view text) noexcept -> bool;
  };
  using Text = StrongText<TitleNumber, Prefixed>;

  explicit TitleNumber(Text text) noexcept : text_(std::move(text)) {}

  Text text_;
};

/// @brief A key that is equal for identifiers of the same title, usable even
/// when an identifier has no title number.
///
/// The key is the title number when the identifier contains one. Otherwise the
/// identifier is split at hyphens and underscores and the key is every part but
/// the last, joined with a vertical bar.
class TitleKey {
 public:
  /// @brief Longest key produced, in bytes; longer keys are cut.
  static constexpr std::size_t text_limit = 127;
  /// @brief How many bytes of an identifier are read when it has no title
  /// number.
  static constexpr std::size_t input_limit = 255;
  /// @brief Most parts taken from an identifier.
  static constexpr std::size_t label_limit = 16;
  /// @brief Longest part kept, in bytes; longer parts are cut.
  static constexpr std::size_t label_bytes = 63;

  /// @brief Derives the key of an identifier.
  ///
  /// @param id A product id or entitlement id.
  /// @return The key, or nothing when the identifier is empty, or has no
  ///     title number and fewer than two parts.
  [[nodiscard]] static auto of(std::string_view id) -> std::optional<TitleKey>;

  /// @brief The key's text.
  ///
  /// @return The key text, valid while this key lives.
  auto text() const noexcept -> const std::string& { return text_.text(); }

  /// @brief Compares the keys byte for byte.
  friend auto operator==(const TitleKey&, const TitleKey&) -> bool = default;
  /// @brief Orders by text, so keys can key a map or set.
  friend auto operator<=>(const TitleKey&, const TitleKey&)
      -> std::strong_ordering = default;
  /// @brief Compares the key with plain text.
  friend auto operator==(const TitleKey& key, std::string_view text) -> bool {
    return key.text_ == text;
  }

 private:
  struct Bounded {
    /// @brief Checks key text against the accepted spelling.
    ///
    /// @param text The candidate key.
    /// @return True when the text is non-empty and at most TitleKey::text_limit
    /// bytes;
    ///     false otherwise.
    static auto admits(std::string_view text) noexcept -> bool;
  };
  using Text = StrongText<TitleKey, Bounded>;

  explicit TitleKey(Text text) noexcept : text_(std::move(text)) {}

  Text text_;
};

}  // namespace libchiaking::catalog
