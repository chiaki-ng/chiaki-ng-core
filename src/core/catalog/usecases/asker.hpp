/// @file
/// @brief Declares usecases::Asker, what the catalog reads off the user who
/// asks it: whether they can be served, their store region and the audience
/// the PS5 categories are asked for.
#pragma once

#include "src/core/catalog/model/unanswered.hpp"

#include <libchiaking/detail/catalog/store_locale.hpp>
#include <libchiaking/detail/users/ps_user.hpp>

#include <optional>

namespace libchiaking::catalog::usecases {

/// @brief Reads what every catalog use case needs to know about the user who
/// asks. Holds nothing.
class Asker {
 public:
  /// @brief Who the PS5 categories are asked for: the account's country,
  /// language and real age.
  struct Audience {
    /// @brief The account's language and legal country.
    StoreLocale locale;
    /// @brief The account's real age.
    int age{};
  };

  /// @brief Who the PS5 categories are asked for.
  ///
  /// @param user The user.
  /// @return The account's language, legal country and real age, or
  ///     std::nullopt when the token does not state them.
  static auto audience_of(const users::PsUser& user) -> std::optional<Audience>;
  /// @brief Refuses a user who cannot be served at all.
  ///
  /// @param user The user.
  /// @return users::Unanswered::Reason::signed_out or
  ///     users::Unanswered::Reason::sign_in_expired for those states;
  ///     std::nullopt when signed in, including when only store access lapsed.
  static auto turned_away(const users::PsUser& user)
      -> std::optional<Unanswered>;
  /// @brief Whether a failure stops the whole load rather than one source.
  ///
  /// @param why The failure.
  /// @return True for a cancellation, a signed-out or an expired sign-in.
  static auto ends_the_load(const Unanswered& why) -> bool;
  /// @brief The store region of the user's account.
  ///
  /// @param user The user.
  /// @return The region, or std::nullopt when unknown.
  static auto region_of(const users::PsUser& user)
      -> std::optional<StoreLocale>;
};

}  // namespace libchiaking::catalog::usecases
