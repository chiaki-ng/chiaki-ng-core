/// @file
/// @brief Declares AccountContext, what a PSN token states about the account's
/// country, locale and age.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace libchiaking::users {

/// @brief The facts a PSN account states about its holder that decide which
/// catalogue they see: legal country, locale and age.
///
/// The facts are read from the claims inside an app token, which is how PsUser
/// learns them, or supplied directly through stating(). Every fact is optional
/// because a token may omit it or state it in a form that is not accepted. The
/// context holds no secret.
///
/// @see PsUser::account_context
class AccountContext {
 public:
  /// @brief The clock issue times are expressed in.
  using Time = std::chrono::system_clock::time_point;

  /// @brief Greatest age accepted; a larger stated age is dropped.
  static constexpr int oldest_age = 150;
  /// @brief Longest locale accepted, in bytes.
  static constexpr std::size_t locale_limit = 16;

  /// @brief Facts exactly as some source states them, before validation.
  struct Stated {
    /// @brief The stated country code.
    std::optional<std::string> legal_country{};
    /// @brief The stated locale, such as "en-GB".
    std::optional<std::string> locale{};
    /// @brief The stated age in years.
    std::optional<std::int64_t> age{};
    /// @brief Whether the account is stated to belong to a child.
    std::optional<bool> is_child{};
    /// @brief When the stating token was issued.
    std::optional<Time> issued_at{};
  };

  /// @brief Creates a context that states nothing.
  AccountContext() = default;

  /// @brief Builds a context from stated facts, keeping only the ones that
  /// are well formed.
  ///
  /// A country is kept when it is exactly two ASCII letters, and is
  /// upper-cased. A locale is kept as given when it is at most locale_limit
  /// bytes and is a two or three letter language, optionally followed by a
  /// hyphen and further letters, digits or hyphens. An age is kept when it
  /// lies between 0 and oldest_age. The child flag and issue time are kept as
  /// given.
  ///
  /// @param stated The facts to validate.
  /// @return The context holding the facts that passed.
  static auto stating(const Stated& stated) -> AccountContext;

  /// @brief The account's legal country as two upper-case letters, when
  /// stated.
  ///
  /// @return The country, or nothing when not stated. The reference lives as
  /// long as this context.
  auto legal_country() const noexcept -> const std::optional<std::string>& {
    return legal_country_;
  }
  /// @brief The account's locale, such as "en-GB", when stated.
  ///
  /// @return The locale, or nothing when not stated. The reference lives as
  /// long as this context.
  auto locale() const noexcept -> const std::optional<std::string>& {
    return locale_;
  }
  /// @brief The language part of the locale: the text before its first
  /// hyphen.
  ///
  /// @return The language, or nothing when no locale is stated.
  auto language() const -> std::optional<std::string>;
  /// @brief The account holder's age in years, when stated.
  ///
  /// @return The age in years, or nothing when not stated.
  auto age() const noexcept -> std::optional<int> { return age_; }
  /// @brief Whether the account belongs to a child, when stated.
  ///
  /// @return True or false when stated, or nothing when not stated.
  auto is_child() const noexcept -> std::optional<bool> { return is_child_; }
  /// @brief When the token the context was read from was issued, when stated.
  ///
  /// @return The issue time, or nothing when not stated.
  auto issued_at() const noexcept -> std::optional<Time> { return issued_at_; }
  /// @brief Whether the context states the country, locale and age together,
  /// which is what selecting a catalogue audience needs.
  ///
  /// @return True when country, locale and age are all stated, false otherwise.
  auto names_a_catalogue_audience() const noexcept -> bool {
    return legal_country_ && locale_ && age_;
  }

  /// @brief Compares every fact.
  friend auto operator==(const AccountContext&, const AccountContext&)
      -> bool = default;

 private:
  static auto is_a_letter(char character) noexcept -> bool;
  static auto country_stated_by(std::string_view text)
      -> std::optional<std::string>;
  static auto locale_stated_by(std::string_view text)
      -> std::optional<std::string>;

  std::optional<std::string> legal_country_;
  std::optional<std::string> locale_;
  std::optional<int> age_;
  std::optional<bool> is_child_;
  std::optional<Time> issued_at_;
};

}  // namespace libchiaking::users
