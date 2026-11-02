#include "src/core/users/ports/trophy_calls.hpp"

#include "src/core/users/ports/authorised_request.hpp"

#include <utility>

namespace libchiaking::users {

TrophyCalls::TrophyCalls(PsUser& user) noexcept
    : user_(PsUser::Core::of(user)) {}

auto TrophyCalls::request() const noexcept -> AuthorisedRequest {
  return AuthorisedRequest{user_};
}

auto TrophyCalls::trophy_summary(net::http::Client& http)
    -> Expected<psn::rest::TrophySummaryRow, Unanswered> {
  return request().asked_of_console_directory<psn::rest::TrophySummaryRow>(
      http, [&](psn::auth::BearerToken token) {
        return psn::rest::Trophies{http, std::move(token)}.summary();
      });
}

auto TrophyCalls::trophy_titles(net::http::Client& http, std::size_t offset)
    -> Expected<psn::rest::TrophyPage<psn::rest::TrophyTitleRow>, Unanswered> {
  return request()
      .asked_of_console_directory<
          psn::rest::TrophyPage<psn::rest::TrophyTitleRow>>(
          http, [&](psn::auth::BearerToken token) {
            return psn::rest::Trophies{http, std::move(token)}.titles(offset);
          });
}

auto TrophyCalls::trophy_groups(net::http::Client& http,
                                const psn::rest::TrophySetRef& set)
    -> Expected<std::vector<psn::rest::TrophyGroupRow>, Unanswered> {
  return request()
      .asked_of_console_directory<std::vector<psn::rest::TrophyGroupRow>>(
          http, [&](psn::auth::BearerToken token) {
            return psn::rest::Trophies{http, std::move(token)}.groups(set);
          });
}

auto TrophyCalls::earned_trophy_groups(net::http::Client& http,
                                       const psn::rest::TrophySetRef& set)
    -> Expected<std::vector<psn::rest::EarnedGroupRow>, Unanswered> {
  return request()
      .asked_of_console_directory<std::vector<psn::rest::EarnedGroupRow>>(
          http, [&](psn::auth::BearerToken token) {
            return psn::rest::Trophies{http, std::move(token)}.earned_groups(
                set);
          });
}

auto TrophyCalls::trophies(net::http::Client& http,
                           const psn::rest::TrophySetRef& set,
                           std::size_t offset)
    -> Expected<psn::rest::TrophyPage<psn::rest::TrophyRow>, Unanswered> {
  return request()
      .asked_of_console_directory<psn::rest::TrophyPage<psn::rest::TrophyRow>>(
          http, [&](psn::auth::BearerToken token) {
            return psn::rest::Trophies{http, std::move(token)}.trophies(set,
                                                                        offset);
          });
}

auto TrophyCalls::earned_trophies(net::http::Client& http,
                                  const psn::rest::TrophySetRef& set,
                                  std::size_t offset)
    -> Expected<psn::rest::TrophyPage<psn::rest::EarnedTrophyRow>, Unanswered> {
  return request()
      .asked_of_console_directory<
          psn::rest::TrophyPage<psn::rest::EarnedTrophyRow>>(
          http, [&](psn::auth::BearerToken token) {
            return psn::rest::Trophies{http, std::move(token)}.earned_trophies(
                set, offset);
          });
}

auto TrophyCalls::trophy_title_sets(net::http::Client& http,
                                    std::span<const std::string> np_title_ids)
    -> Expected<std::vector<psn::rest::TitleSetsRow>, Unanswered> {
  return request()
      .asked_of_console_directory<std::vector<psn::rest::TitleSetsRow>>(
          http, [&](psn::auth::BearerToken token) {
            return psn::rest::Trophies{http, std::move(token)}.title_sets(
                np_title_ids);
          });
}

}  // namespace libchiaking::users
