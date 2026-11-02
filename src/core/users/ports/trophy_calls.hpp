/// @file
/// @brief Declares TrophyCalls, the trophies context's port onto a user's
/// authorised calls to PS.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/rest/trophy/trophies.hpp"

#include <libchiaking/detail/users/ps_user.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace libchiaking::users {

class AuthorisedRequest;

/// @brief What the trophies context may ask PS on a user's behalf: the
/// trophy summary, titles, groups and trophies, earned or not.
///
/// A user that is off PSN is answered at once and nothing is sent. A failure
/// that means PSN no longer accepts the NPSSO marks the user's sign-in as
/// refused, so the user reports SignInState::expired.
///
/// The object is a short-lived view: it only refers to the user's core, holds
/// no state of its own, and is made afresh wherever a call is needed. It must
/// not outlive the PsUser it was made for. Every call blocks the calling
/// thread for its requests, each bounded by the HTTP client's timeouts and its
/// stop token; calls may be made from several threads at once, the user's core
/// providing the locking.
class TrophyCalls {
 public:
  /// @brief Views a user handed out by SignedInUsers.
  ///
  /// @param user The user; must outlive this object.
  explicit TrophyCalls(PsUser& user) noexcept;

  /// @brief Reads the account's trophy summary, signed with the app token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @return The summary.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason::rate_limited PS answered 429.
  /// @retval Unanswered::Reason Any other failure of the renewal or the
  ///     request.
  [[nodiscard]] auto trophy_summary(net::http::Client& http)
      -> Expected<psn::rest::TrophySummaryRow, Unanswered>;

  /// @brief Reads one page of the account's trophy titles, signed with the
  /// app token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @param offset Index of the first title on the page.
  /// @return The page.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason::rate_limited PS answered 429.
  /// @retval Unanswered::Reason Any other failure of the renewal or the
  ///     request.
  [[nodiscard]] auto trophy_titles(net::http::Client& http, std::size_t offset)
      -> Expected<psn::rest::TrophyPage<psn::rest::TrophyTitleRow>, Unanswered>;

  /// @brief Reads the groups a trophy set defines, signed with the app
  /// token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @param set The set.
  /// @return The groups.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason::rate_limited PS answered 429.
  /// @retval Unanswered::Reason Any other failure of the renewal or the
  ///     request.
  [[nodiscard]] auto trophy_groups(net::http::Client& http,
                                   const psn::rest::TrophySetRef& set)
      -> Expected<std::vector<psn::rest::TrophyGroupRow>, Unanswered>;

  /// @brief Reads the account's progress in each group of a trophy set,
  /// signed with the app token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @param set The set.
  /// @return The progress of each group.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason::rate_limited PS answered 429.
  /// @retval Unanswered::Reason Any other failure of the renewal or the
  ///     request.
  [[nodiscard]] auto earned_trophy_groups(net::http::Client& http,
                                          const psn::rest::TrophySetRef& set)
      -> Expected<std::vector<psn::rest::EarnedGroupRow>, Unanswered>;

  /// @brief Reads one page of the trophies a set defines, signed with the
  /// app token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @param set The set.
  /// @param offset Index of the first trophy on the page.
  /// @return The page.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason::rate_limited PS answered 429.
  /// @retval Unanswered::Reason Any other failure of the renewal or the
  ///     request.
  [[nodiscard]] auto trophies(net::http::Client& http,
                              const psn::rest::TrophySetRef& set,
                              std::size_t offset)
      -> Expected<psn::rest::TrophyPage<psn::rest::TrophyRow>, Unanswered>;

  /// @brief Reads one page of the account's state for a set's trophies,
  /// signed with the app token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @param set The set.
  /// @param offset Index of the first trophy on the page.
  /// @return The page.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason::rate_limited PS answered 429.
  /// @retval Unanswered::Reason Any other failure of the renewal or the
  ///     request.
  [[nodiscard]] auto earned_trophies(net::http::Client& http,
                                     const psn::rest::TrophySetRef& set,
                                     std::size_t offset)
      -> Expected<psn::rest::TrophyPage<psn::rest::EarnedTrophyRow>,
                  Unanswered>;

  /// @brief Reads the trophy sets PS files each of some title ids under,
  /// signed with the app token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @param np_title_ids One to psn::rest::Trophies::title_sets_limit title
  ///     ids.
  /// @return The titles PS answered for.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason::rate_limited PS answered 429.
  /// @retval Unanswered::Reason Any other failure of the renewal or the
  ///     request, including too many or no ids.
  [[nodiscard]] auto trophy_title_sets(
      net::http::Client& http, std::span<const std::string> np_title_ids)
      -> Expected<std::vector<psn::rest::TitleSetsRow>, Unanswered>;

 private:
  /// @brief Opens the shared request helpers on this user.
  ///
  /// @return A view of the same user.
  auto request() const noexcept -> AuthorisedRequest;

  /// @brief The user the calls are made for.
  PsUser::Core& user_;
};

}  // namespace libchiaking::users
