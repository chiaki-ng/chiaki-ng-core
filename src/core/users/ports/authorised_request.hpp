/// @file
/// @brief Declares AuthorisedRequest, what every port onto a user's
/// authorised calls shares: picking and renewing the credential PS expects,
/// and turning every failure into the users context's vocabulary.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/net/transport/http/status.hpp"
#include "src/core/psn/auth/authenticator.hpp"
#include "src/core/psn/graphql/authenticated/subscription.hpp"
#include "src/core/psn/rest/commerce/commerce_client.hpp"
#include "src/core/psn/rest/error.hpp"
#include "src/core/psn/rest/gamelist/gamelist.hpp"
#include "src/core/psn/session_internals.hpp"
#include "src/core/users/adapters/membership_cache.hpp"
#include "src/core/users/adapters/ps_user_core.hpp"
#include "src/core/utils/retry/with_renewal.hpp"

#include <libchiaking/detail/users/ps_user.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::users {

/// @brief The request helpers the ports onto a user share: CatalogCalls,
/// ConsoleCalls, MessageCalls, TrophyCalls, PlayCalls and MembershipCalls.
///
/// Each helper picks the credential PS expects for a kind of question:
/// - the app token as held for the app library, subscriptions and the cloud
///   game catalogue;
/// - the app token, restored and renewed once, for the console directory;
/// - a commerce token from the NPSSO for the owned-entitlement records.
///
/// A view over the user's core like the ports themselves; it must not outlive
/// the PsUser it was made for.
class AuthorisedRequest {
 public:
  /// @brief Views a user's core.
  ///
  /// @param user The user's core; must outlive this object.
  explicit AuthorisedRequest(PsUser::Core& user) noexcept : user_(user) {}

  /// @brief Diagnostics label of the commerce token for owned entitlements.
  static constexpr std::string_view library_credential = "commerce bearer";

  /// @brief Diagnostics label of the Remote Play token.
  static constexpr std::string_view remote_play_credential =
      "remote play bearer";

  /// @brief Diagnostics label of the app token sent to the console
  /// directory.
  static constexpr std::string_view console_directory_credential =
      "console directory bearer";

  /// @brief Diagnostics label of the LegacyCloudStream authorisation codes.
  static constexpr std::string_view old_service_credential =
      "legacy_cloud_stream code";

  /// @brief Diagnostics label of the app token sent to the app library and
  /// subscriptions services.
  static constexpr std::string_view app_library_credential = "app bearer";

  /// @brief Which refusals of a question mean the app token was refused.
  enum class TokenRefusal : std::uint8_t {
    /// @brief HTTP 401 and 403 both do.
    unauthorised_or_forbidden,
    /// @brief Only HTTP 401 does; a 403 refuses the question itself, as the
    /// message service answers for a conversation the user may not write in.
    unauthorised_only,
  };

  /// @brief Asks the commerce service something with a commerce token for
  /// owned entitlements, renewing a refused cached token once.
  ///
  /// A token still refused at the end is forgotten from the cache.
  ///
  /// @tparam Reply What the question returns.
  /// @tparam Ask Callable as `ask (psn::auth::Authenticator&, const
  ///     psn::auth::BearerToken&)` returning
  ///     `Expected<Reply, psn::rest::CommerceError>`; it may be called twice.
  /// @param http The client the token exchange is sent through.
  /// @param ask Sends the question.
  /// @return The reply.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason Any failure of the token or the question.
  template <class Reply, class Ask>
  [[nodiscard]] auto asked_of_library(net::http::Client& http, const Ask& ask)
      -> Expected<Reply, Unanswered>;

  /// @brief Asks an app-token service something with the app token as held,
  /// recording a success against the token.
  ///
  /// @tparam Reply What the question returns.
  /// @tparam Service The client type, constructible from a
  ///     `net::http::Client` and a `psn::auth::BearerToken`.
  /// @tparam Ask Callable as `ask (Service&)` returning
  ///     `Expected<Reply, psn::rest::Error<...>>`.
  /// @param http The client the question is sent through.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @param ask Sends the question.
  /// @return The reply.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed No usable app token is
  ///     held.
  /// @retval Unanswered::Reason Any failure of the question.
  template <class Reply, class Service, class Ask>
  [[nodiscard]] auto asked_of_app_library(net::http::Client& http,
                                          std::size_t maximum_response_bytes,
                                          const Ask& ask)
      -> Expected<Reply, Unanswered>;

  /// @brief Asks the console directory something with the app token,
  /// restoring it first and renewing it once if PS refuses it.
  ///
  /// @tparam Reply What the question returns.
  /// @tparam Ask Callable as `ask (psn::auth::BearerToken)` returning
  ///     `Expected<Reply, psn::rest::Error<...>>`; it may be called twice.
  /// @param http The client the requests are sent through.
  /// @param ask Sends the question.
  /// @param refusal Which refusals mean the token was refused.
  /// @return The reply.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal; the refusal is recorded against it.
  /// @retval Unanswered::Reason::rejected PS answered 403 under
  ///     TokenRefusal::unauthorised_only; the token is left alone and the
  ///     question is not sent again.
  /// @retval Unanswered::Reason Any failure of the renewal or the question.
  template <class Reply, class Ask>
  [[nodiscard]] auto asked_of_console_directory(
      net::http::Client& http, const Ask& ask,
      TokenRefusal refusal = TokenRefusal::unauthorised_or_forbidden)
      -> Expected<Reply, Unanswered>;

  /// @brief Translates an authorisation server failure, marking the sign-in
  /// refused when PSN refused or rejected the NPSSO.
  ///
  /// @param error The failure.
  /// @return sign_in_expired for a refused, rejected or invalid NPSSO;
  ///     cancelled, malformed, or rejected (for an invalid request) as the
  ///     error says; unavailable otherwise. The HTTP status is carried over.
  [[nodiscard]] auto unanswered(const psn::auth::AuthError& error)
      -> Unanswered;

  /// @brief Translates a commerce service failure, marking the sign-in
  /// refused when the service refused the token.
  ///
  /// @param error The failure.
  /// @return sign_in_expired when the token was refused; cancelled; rejected
  ///     for not found or rejected; malformed for malformed or truncated
  ///     replies; unavailable otherwise. The HTTP status is carried over.
  [[nodiscard]] auto unanswered(const psn::rest::CommerceError& error)
      -> Unanswered;

  /// @brief Translates a REST service failure.
  ///
  /// @param error The failure.
  /// @return cancelled when the stop token fired; rejected for an invalid
  ///     request or a request the service rejected; rate_limited for HTTP
  ///     429; malformed for an unreadable reply; unavailable otherwise,
  ///     including other error statuses. The HTTP status is carried over.
  ///
  /// @tparam Reasons The call's reasons.
  template <psn::rest::ReasonTag... Reasons>
  [[nodiscard]] static auto unanswered(
      const psn::rest::Error<Reasons...>& error) noexcept -> Unanswered {
    return unanswered(error.cause(), error.failure(), error.http_status());
  }

  /// @brief Translates the parts of a REST service failure.
  ///
  /// @param cause Where the call failed.
  /// @param failure The transport failure, for Cause::failure.
  /// @param status The error status, or 0.
  /// @return As unanswered(const psn::rest::Error<Reasons...>&).
  [[nodiscard]] static auto unanswered(
      psn::rest::Cause cause, std::optional<net::http::Failure> failure,
      std::uint16_t status) noexcept -> Unanswered;

  /// @brief Builds the key that tells shared questions apart.
  ///
  /// @param maximum_response_bytes The size limit asked with.
  /// @param about What else distinguishes the question; may be empty.
  /// @return The limit in decimal, a space, then `about`.
  static auto question(std::size_t maximum_response_bytes,
                       std::string_view about = {}) -> std::string;

  /// @brief Writes accounts in decimal, the form PSN's invitations take.
  ///
  /// @param accounts The accounts.
  /// @return Their ids in decimal, in the same order.
  static auto decimal_ids(std::span<const AccountId> accounts)
      -> std::vector<std::string>;

  /// @brief Reads the user's PlayStation Plus membership through the GraphQL
  /// subscriptions query, reusing a remembered answer when it will do.
  ///
  /// The highest tier among the active subscriptions wins; no active
  /// subscription means Tier::none. Concurrent readers share one request
  /// through MembershipCache.
  ///
  /// @param http The client the query is sent through; its stop token
  ///     cancels it.
  /// @param remembered_from The lowest tier a remembered answer must show to
  ///     be reused; Tier::none reuses any fresh answer.
  /// @return The membership.
  /// @retval psn::graphql::CheckStatus::no_session The user is off PSN.
  /// @retval psn::graphql::GraphqlError::Kind::malformed An active subscription
  ///     has an unknown tier label, or its SKU names a different tier.
  /// @retval psn::graphql::CheckError Any other failure of the query.
  [[nodiscard]] auto membership(net::http::Client& http,
                                Tier remembered_from = Tier::none)
      -> MembershipCheck;

  /// @brief Finds out the user's PlayStation Plus membership, answering with
  /// what the profile showed when PSN cannot be asked.
  ///
  /// The app token is restored first unless the sign-in has expired; a
  /// failure to restore it is not reported itself, except cancellation. Then
  /// membership() is asked; concurrent callers with the same
  /// `remembered_from` share one attempt. When the query fails but the last
  /// profile showed the user to be a member, that answer is returned, not
  /// cached and stamped now, with the tier unknown.
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param remembered_from The lowest tier a remembered answer must show to
  ///     be reused.
  /// @return The membership and when it was learned.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::cancelled The stop token fired.
  /// @retval Unanswered::Reason::store_access_lapsed The app token is
  ///     missing, expired or was refused.
  /// @retval Unanswered::Reason::rejected PSN rejected the query or found
  ///     nothing.
  /// @retval Unanswered::Reason::malformed The reply could not be read.
  /// @retval Unanswered::Reason::unavailable Any other failure.
  [[nodiscard]] auto answered_membership(net::http::Client& http,
                                         Tier remembered_from = Tier::none)
      -> Expected<MembershipAnswer, Unanswered>;

  /// @brief Forgets the remembered membership after PS refused a cloud
  /// server reservation for want of PlayStation Plus, so the next question
  /// asks PSN again.
  void membership_rejected_by_service();

  /// @brief Judges the membership from the active subscriptions.
  ///
  /// @param subscriptions The subscriptions query's reply.
  /// @return The highest tier among the active subscriptions with that
  ///     subscription's plan, or Tier::none when none is active; nothing
  ///     when one has an unknown tier label or a SKU naming another tier.
  [[nodiscard]] static auto membership_of(
      const psn::graphql::PlusSubscriptions& subscriptions)
      -> std::optional<Membership>;

  /// @brief Reads a PS tier label.
  ///
  /// @param label The label.
  /// @return Tier::premium for `TIER_30`, Tier::extra for `TIER_20`,
  ///     Tier::essential for `TIER_10`; nothing for any other label.
  [[nodiscard]] static auto tier_of_label(std::string_view label) noexcept
      -> std::optional<Tier>;

  /// @brief Reads the tier a SKU names.
  ///
  /// @param sku The SKU.
  /// @return Tier::premium when it contains `PLUS3T`, Tier::extra for
  ///     `PLUS2T`, Tier::essential for `PLUS1T`; nothing otherwise.
  [[nodiscard]] static auto tier_of_sku(std::string_view sku) noexcept
      -> std::optional<Tier>;

  /// @brief Reads one page of the titles the account has played recently,
  /// signed with the app token.
  ///
  /// Renews the app token as ConsoleCalls::installed_titles does.
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param offset Index of the first title on the page.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @return The page.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto played_titles(
      net::http::Client& http, std::size_t offset,
      std::size_t maximum_response_bytes =
          net::http::default_maximum_response_bytes)
      -> Expected<psn::rest::PlayedTitlesPage, Unanswered>;

 private:
  /// @brief The user the requests are made for.
  PsUser::Core& user_;
};

template <class Reply, class Ask>
auto AuthorisedRequest::asked_of_library(net::http::Client& http,
                                         const Ask& ask)
    -> Expected<Reply, Unanswered> {
  if (user_.off_psn())
    return unexpected(Unanswered{Unanswered::Reason::signed_out});
  psn::auth::Authenticator auth{
      http, user_.session().npsso().clone(),
      &psn::SessionInternals{user_.session()}.tokens()};
  auto issued = auth.commerce_token(psn::auth::owned_entitlements_scope);
  if (!issued) return unexpected(unanswered(issued.error()));
  auto token = std::move(issued.value());
  auto renewed = retry::with_renewal(
      [&] { return ask(auth, token); },
      [&](const psn::rest::CommerceError& refused) {
        return refused.reason() ==
                   psn::rest::CommerceError::Reason::auth_refused &&
               auth.renewable(token);
      },
      [&](const psn::rest::CommerceError& refused)
          -> Expected<void, Unanswered> {
        return auth.refresh(token, refused.http_status())
            .transform(
                [&](psn::auth::BearerToken fresh) { token = std::move(fresh); })
            .transform_error([this](const psn::auth::AuthError& error) {
              return unanswered(error);
            });
      });
  if (!renewed) return unexpected(renewed.error());
  auto& reply = renewed.value();
  if (reply) return std::move(reply.value());
  if (reply.error().reason() == psn::rest::CommerceError::Reason::auth_refused)
    auth.forget(token);
  return unexpected(unanswered(reply.error()));
}

template <class Reply, class Service, class Ask>
auto AuthorisedRequest::asked_of_app_library(net::http::Client& http,
                                             std::size_t maximum_response_bytes,
                                             const Ask& ask)
    -> Expected<Reply, Unanswered> {
  using Reason = Unanswered::Reason;
  if (user_.off_psn()) return unexpected(Unanswered{Reason::signed_out});
  auto& slot = psn::SessionInternals{user_.session()}.app_token();
  const auto token = slot.lease();
  if (!token) return unexpected(Unanswered{Reason::store_access_lapsed});
  auto capped = http.limited(maximum_response_bytes);
  Service service{capped, psn::auth::BearerToken{token.value().secret().clone(),
                                                 app_library_credential}};
  auto learned = ask(service);
  if (!learned) return unexpected(unanswered(learned.error()));
  slot.record_use(token.value(), net::HttpStatus::ok.code(), 0);
  return std::move(learned.value());
}

template <class Reply, class Ask>
auto AuthorisedRequest::asked_of_console_directory(net::http::Client& http,
                                                   const Ask& ask,
                                                   TokenRefusal refusal)
    -> Expected<Reply, Unanswered> {
  using Reason = Unanswered::Reason;
  if (user_.off_psn()) return unexpected(Unanswered{Reason::signed_out});
  auto& slot = psn::SessionInternals{user_.session()}.app_token();
  auto token = user_.restored_app_token(http.stop_token());
  if (!token) return unexpected(token.error());
  auto renewed = retry::with_renewal(
      [&] {
        return ask(psn::auth::BearerToken{token.value().secret().clone(),
                                          console_directory_credential});
      },
      [refusal](const auto& refused) {
        return refusal == TokenRefusal::unauthorised_only
                   ? net::HttpStatus{refused.http_status()} ==
                         net::HttpStatus::unauthorized
                   : refused.auth_refused();
      },
      [&](const auto& refused) -> Expected<void, Unanswered> {
        slot.record_use(token.value(), refused.http_status(), 0);
        token = user_.restored_app_token(http.stop_token());
        if (!token)
          return unexpected(
              Unanswered{token.error().reason, refused.http_status()});
        return {};
      });
  if (!renewed) return unexpected(renewed.error());
  auto& reply = renewed.value();
  if (reply) return std::move(reply.value());
  if (refusal == TokenRefusal::unauthorised_only &&
      net::HttpStatus{reply.error().http_status()} ==
          net::HttpStatus::forbidden)
    return unexpected(
        Unanswered{Unanswered::Reason::rejected, reply.error().http_status()});
  if (!reply.error().auth_refused())
    return unexpected(unanswered(reply.error()));
  slot.record_use(token.value(), reply.error().http_status(), 0);
  return unexpected(
      Unanswered{Reason::store_access_lapsed, reply.error().http_status()});
}

}  // namespace libchiaking::users
