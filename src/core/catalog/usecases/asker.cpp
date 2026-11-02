#include "src/core/catalog/usecases/asker.hpp"

#include <libchiaking/detail/users/account_context.hpp>

#include <string>
#include <utility>

namespace libchiaking::catalog::usecases {

auto Asker::audience_of(const users::PsUser& user) -> std::optional<Audience> {
  const auto context = user.account_context();
  const auto language = context.language();
  if (!context.names_a_catalogue_audience() || !language) return std::nullopt;
  auto locale = StoreLocale::parse(*language + "-" + *context.legal_country());
  if (!locale) return std::nullopt;
  return Audience{std::move(*locale), *context.age()};
}

auto Asker::turned_away(const users::PsUser& user)
    -> std::optional<Unanswered> {
  using Cause = users::Unanswered::Reason;
  switch (user.sign_in_state()) {
    case users::SignInState::signed_out:
      return Unanswered{users::Unanswered{Cause::signed_out}};
    case users::SignInState::expired:
      return Unanswered{users::Unanswered{Cause::sign_in_expired}};
    case users::SignInState::signed_in:
    case users::SignInState::store_access_lapsed:
      break;
  }
  return std::nullopt;
}

auto Asker::ends_the_load(const Unanswered& why) -> bool {
  using Cause = users::Unanswered::Reason;
  const auto& cause = why.cause();
  return why.is_cancelled() ||
         (cause && (cause->reason == Cause::signed_out ||
                    cause->reason == Cause::sign_in_expired));
}

auto Asker::region_of(const users::PsUser& user) -> std::optional<StoreLocale> {
  const auto region = user.region();
  return region ? StoreLocale::for_account(*region) : std::nullopt;
}

}  // namespace libchiaking::catalog::usecases
