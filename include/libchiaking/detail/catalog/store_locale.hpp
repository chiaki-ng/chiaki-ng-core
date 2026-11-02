/// @file
/// @brief Declares StoreLocale, a language and country the PlayStation Store is
/// asked in.
#pragma once

#include "libchiaking/detail/users/credential_store.hpp"

#include <compare>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::catalog {

/// @brief A language and country pair the PlayStation Store is asked in, such
/// as en-US.
///
/// The country selects the store and its catalogue; the language selects the
/// texts. The language is kept in lower case and the country in upper case.
class StoreLocale {
 public:
  /// @brief Longest language accepted, in characters.
  static constexpr std::size_t language_limit = 8;
  /// @brief Length of a country code, in letters.
  static constexpr std::size_t country_length = 2;

  /// @brief Reads a locale written as language, a hyphen or underscore, then
  /// country.
  ///
  /// @param text The text, such as en-US or en_us; surrounding space is
  ///     ignored and letter case is normalised.
  /// @return The locale, or nothing unless the part after the last separator
  ///     is two letters and the part before it is 2 to
  ///     StoreLocale::language_limit letters, with separators allowed inside.
  [[nodiscard]] static auto parse(std::string_view text)
      -> std::optional<StoreLocale>;
  /// @brief Works out the locale to use for an account's region.
  ///
  /// @param region The country and language of the account.
  /// @return The locale, with English standing in when the region's language
  ///     is unusable, or nothing when the country is not two letters.
  [[nodiscard]] static auto for_account(const users::Region& region)
      -> std::optional<StoreLocale>;
  /// @brief The en-US locale, used when nothing better is known.
  ///
  /// @return The locale with language en and country US.
  static auto english_us() -> StoreLocale;
  /// @brief The fallback store an account's country walks.
  ///
  /// @param account_country The account's country code, in any case.
  /// @return "US" for the Americas, "GB" otherwise.
  static auto shared_store_country(std::string_view account_country)
      -> std::string_view;

  /// @brief The language, in lower case.
  ///
  /// @return The language text, valid while this locale lives.
  auto language() const noexcept -> const std::string& { return language_; }
  /// @brief The two-letter country, in upper case.
  ///
  /// @return The country text, valid while this locale lives.
  auto country() const noexcept -> const std::string& { return country_; }
  /// @brief The locale written as language, hyphen, country, such as en-US.
  ///
  /// @return The locale text, such as en-US.
  auto text() const -> std::string;
  /// @brief The locale in lower case throughout, such as en-us, as store
  /// addresses use it.
  ///
  /// @return The locale text in lower case, such as en-us.
  auto lowercase() const -> std::string;
  /// @brief The English locale of the same country.
  ///
  /// @return The locale with language en and this locale's country.
  auto english() const -> StoreLocale;
  /// @brief Lists the locales to try in turn when the store has nothing in
  /// this one.
  ///
  /// @return This locale, then English in the same country, then en-US,
  ///     without repeats.
  auto fallbacks() const -> std::vector<StoreLocale>;
  /// @brief Whether the locale's country is the region's country, ignoring
  /// letter case.
  ///
  /// @param region The region to compare with.
  /// @return True when the countries are equal ignoring case; false otherwise.
  auto is_in_country_of(const users::Region& region) const noexcept -> bool;

  /// @brief Compares language and country.
  friend auto operator==(const StoreLocale&, const StoreLocale&)
      -> bool = default;
  /// @brief Orders by language, then country, so locales can key a map or
  /// set.
  friend auto operator<=>(const StoreLocale&, const StoreLocale&)
      -> std::strong_ordering = default;

 private:
  StoreLocale(std::string language, std::string country) noexcept;

  [[nodiscard]] static auto language_of(std::string_view text)
      -> std::optional<std::string>;
  [[nodiscard]] static auto country_of(std::string_view text)
      -> std::optional<std::string>;
  static auto letter(char value) noexcept -> bool;

  std::string language_;
  std::string country_;
};

}  // namespace libchiaking::catalog
