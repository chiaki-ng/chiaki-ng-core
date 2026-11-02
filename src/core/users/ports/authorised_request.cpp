#include "src/core/users/ports/authorised_request.hpp"

#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/psn/graphql/authenticated/client.hpp"

#include <string>

namespace libchiaking::users {

auto AuthorisedRequest::unanswered(psn::rest::Cause cause,
                                   std::optional<net::http::Failure> failure,
                                   std::uint16_t status) noexcept
    -> Unanswered {
  using Reason = Unanswered::Reason;
  switch (cause) {
    case psn::rest::Cause::failure:
      if (failure && failure->is_cancelled())
        return {Reason::cancelled, status};
      if (failure &&
          failure->kind() == net::http::Failure::Kind::invalid_request)
        return {Reason::rejected, status};
      break;
    case psn::rest::Cause::status:
      if (net::HttpStatus{status} == net::HttpStatus::too_many_requests)
        return {Reason::rate_limited, status};
      break;
    case psn::rest::Cause::request:
      return {Reason::rejected, status};
    case psn::rest::Cause::response:
      return {Reason::malformed, status};
  }
  return {Reason::unavailable, status};
}

auto AuthorisedRequest::unanswered(const psn::auth::AuthError& error)
    -> Unanswered {
  using Reason = Unanswered::Reason;
  switch (error.kind()) {
    case psn::auth::AuthErrorKind::cancelled:
      return {Reason::cancelled, error.http_status()};
    case psn::auth::AuthErrorKind::refused:
    case psn::auth::AuthErrorKind::rejected:
    case psn::auth::AuthErrorKind::invalid_npsso:
      user_.refuse_sign_in();
      return {Reason::sign_in_expired, error.http_status()};
    case psn::auth::AuthErrorKind::malformed:
      return {Reason::malformed, error.http_status()};
    case psn::auth::AuthErrorKind::invalid_request:
      return {Reason::rejected, error.http_status()};
    default:
      break;
  }
  return {Reason::unavailable, error.http_status()};
}

auto AuthorisedRequest::unanswered(const psn::rest::CommerceError& error)
    -> Unanswered {
  using Reason = Unanswered::Reason;
  using Cause = psn::rest::CommerceError::Reason;
  switch (error.reason()) {
    case Cause::cancelled:
      return {Reason::cancelled, error.http_status()};
    case Cause::auth_refused:
      user_.refuse_sign_in();
      return {Reason::sign_in_expired, error.http_status()};
    case Cause::not_found:
    case Cause::rejected:
      return {Reason::rejected, error.http_status()};
    case Cause::malformed:
    case Cause::truncated:
      return {Reason::malformed, error.http_status()};
    case Cause::unavailable:
      break;
  }
  return {Reason::unavailable, error.http_status()};
}

auto AuthorisedRequest::question(std::size_t maximum_response_bytes,
                                 std::string_view about) -> std::string {
  return std::to_string(maximum_response_bytes).append(1, ' ').append(about);
}

auto AuthorisedRequest::decimal_ids(std::span<const AccountId> accounts)
    -> std::vector<std::string> {
  std::vector<std::string> ids;
  ids.reserve(accounts.size());
  for (const auto& account : accounts) ids.push_back(account.decimal());
  return ids;
}

auto AuthorisedRequest::membership(net::http::Client& http,
                                   Tier remembered_from) -> MembershipCheck {
  using psn::graphql::CheckError;
  const auto& operation = psn::graphql::authenticated::get_subscriptions;
  if (user_.off_psn())
    return unexpected(
        CheckError{psn::graphql::CheckStatus::no_session, operation.name});
  return user_.memberships().get(
      http.stop_token(), remembered_from, [this, &http]() -> MembershipCheck {
        const psn::SessionInternals held{user_.session()};
        psn::graphql::AuthenticatedClient account{http, held.app_token(),
                                                  &held.graphql()};
        auto fetched = account.subscriptions();
        if (!fetched) return unexpected(CheckError{std::move(fetched.error())});
        auto answered = membership_of(fetched.value());
        if (!answered)
          return unexpected(CheckError{psn::graphql::GraphqlError::replied(
              operation.name, psn::graphql::GraphqlError::Kind::malformed,
              fetched.value().http_status, fetched.value().attempts)});
        return MembershipAnswer{user_.account_id(), std::move(*answered)};
      });
}

auto AuthorisedRequest::answered_membership(net::http::Client& http,
                                            Tier remembered_from)
    -> Expected<MembershipAnswer, Unanswered> {
  using Reason = Unanswered::Reason;
  using psn::graphql::CheckStatus;
  if (user_.off_psn()) return unexpected(Unanswered{Reason::signed_out});
  return user_.in_flight().membership.answer(
      Membership::name_of(remembered_from), http.stop_token(),
      [&]() -> Expected<MembershipAnswer, Unanswered> {
        if (auto token = user_.app_token_unless_refused(http.stop_token());
            !token && token.error().reason == Reason::cancelled)
          return unexpected(Unanswered{Reason::cancelled});
        auto check = membership(http, remembered_from);
        if (check) return std::move(check.value());
        const auto status = check.error().status();
        if (status == CheckStatus::cancelled)
          return unexpected(Unanswered{Reason::cancelled});
        if (auto inferred = user_.inferred_membership();
            inferred.is_ps_plus_member())
          return MembershipAnswer{
              user_.account_id(), std::move(inferred), false,
              static_cast<std::int64_t>(user_.wall().now_microseconds() /
                                        microseconds_per_millisecond)};
        switch (status) {
          case CheckStatus::no_session:
            return unexpected(Unanswered{Reason::signed_out});
          case CheckStatus::no_app_token:
          case CheckStatus::app_token_expired:
          case CheckStatus::auth_refused:
            return unexpected(Unanswered{Reason::store_access_lapsed,
                                         check.error().http_status()});
          case CheckStatus::rejected:
          case CheckStatus::invalid_request:
          case CheckStatus::not_found:
            return unexpected(
                Unanswered{Reason::rejected, check.error().http_status()});
          case CheckStatus::malformed:
            return unexpected(
                Unanswered{Reason::malformed, check.error().http_status()});
          default:
            break;
        }
        return unexpected(
            Unanswered{Reason::unavailable, check.error().http_status()});
      });
}

void AuthorisedRequest::membership_rejected_by_service() {
  user_.memberships().clear();
}

auto AuthorisedRequest::membership_of(
    const psn::graphql::PlusSubscriptions& subscriptions)
    -> std::optional<Membership> {
  Membership best{Tier::none};
  for (const auto& subscription : subscriptions.active) {
    const auto tier = tier_of_label(subscription.tier_label);
    const auto named = tier_of_sku(subscription.sku_id);
    if (!tier || (named && named != tier)) return std::nullopt;
    if (best.tier() >= tier) continue;
    best = Membership{*tier,
                      {subscription.tier_label, subscription.sku_id,
                       subscription.product_name, subscription.end_date}};
  }
  return best;
}

auto AuthorisedRequest::tier_of_label(std::string_view label) noexcept
    -> std::optional<Tier> {
  if (label == "TIER_30") return Tier::premium;
  if (label == "TIER_20") return Tier::extra;
  if (label == "TIER_10") return Tier::essential;
  return std::nullopt;
}

auto AuthorisedRequest::tier_of_sku(std::string_view sku) noexcept
    -> std::optional<Tier> {
  if (sku.find("PLUS3T") != std::string_view::npos) return Tier::premium;
  if (sku.find("PLUS2T") != std::string_view::npos) return Tier::extra;
  if (sku.find("PLUS1T") != std::string_view::npos) return Tier::essential;
  return std::nullopt;
}

auto AuthorisedRequest::played_titles(net::http::Client& http,
                                      std::size_t offset,
                                      std::size_t maximum_response_bytes)
    -> Expected<psn::rest::PlayedTitlesPage, Unanswered> {
  return asked_of_console_directory<psn::rest::PlayedTitlesPage>(
      http, [&](psn::auth::BearerToken token) {
        auto capped = http.limited(maximum_response_bytes);
        return psn::rest::Gamelist{capped, std::move(token)}.played_titles(
            offset);
      });
}

}  // namespace libchiaking::users
