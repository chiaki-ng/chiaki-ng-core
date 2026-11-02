#include "src/core/users/ports/catalog_calls.hpp"

#include "src/core/psn/rest/settings/user_settings.hpp"
#include "src/core/psn/session_internals.hpp"
#include "src/core/users/ports/authorised_request.hpp"

#include <utility>

namespace libchiaking::users {

CatalogCalls::CatalogCalls(PsUser& user) noexcept
    : user_(PsUser::Core::of(user)) {}

auto CatalogCalls::answered_membership(net::http::Client& http,
                                       Tier remembered_from)
    -> Expected<MembershipAnswer, Unanswered> {
  return request().answered_membership(http, remembered_from);
}

auto CatalogCalls::request() const noexcept -> AuthorisedRequest {
  return AuthorisedRequest{user_};
}

auto CatalogCalls::store_app(net::http::Client& http)
    -> Expected<psn::graphql::AuthenticatedClient, Unanswered> {
  using Reason = Unanswered::Reason;
  if (user_.off_psn()) return unexpected(Unanswered{Reason::signed_out});
  if (auto token = user_.app_token_unless_refused(http.stop_token()); !token)
    return unexpected(token.error());
  const psn::SessionInternals held{user_.session()};
  return psn::graphql::AuthenticatedClient{http, held.app_token(),
                                           &held.graphql()};
}

auto CatalogCalls::old_service_store(net::http::Client& http,
                                     std::size_t maximum_response_bytes)
    -> Expected<psn::rest::KamajiClient, Unanswered> {
  if (user_.off_psn())
    return unexpected(Unanswered{Unanswered::Reason::signed_out});
  const auto opened = user_.in_flight().old_service_store.answer(
      AuthorisedRequest::question(maximum_response_bytes), http.stop_token(),
      [&] { return opened_old_service_store(http, maximum_response_bytes); });
  if (!opened) return unexpected(opened.error());
  return opened.value()->with_same_session(
      http.limited(maximum_response_bytes));
}

auto CatalogCalls::library_records(
    net::http::Client& http, const psn::rest::EntitlementPageObserver& observe,
    std::size_t maximum_response_bytes)
    -> Expected<psn::rest::Entitlements, Unanswered> {
  return user_.in_flight().library_records.answer(
      AuthorisedRequest::question(maximum_response_bytes), http.stop_token(),
      [&] {
        return request().asked_of_library<psn::rest::Entitlements>(
            http, [&](psn::auth::Authenticator& auth,
                      const psn::auth::BearerToken& token) {
              const psn::rest::EntitlementPageObserver recorded =
                  [&](int page,
                      const Expected<std::uint16_t, psn::rest::CommerceError>&
                          outcome) {
                    if (outcome || !outcome.error().failure())
                      auth.record_use(token,
                                      outcome ? outcome.value()
                                              : outcome.error().http_status());
                    if (observe) observe(page, outcome);
                  };
              return psn::rest::CommerceClient{
                  http.limited(maximum_response_bytes)}
                  .internal_entitlements(
                      psn::auth::BearerToken{
                          token.secret().clone(),
                          AuthorisedRequest::library_credential},
                      recorded);
            });
      });
}

auto CatalogCalls::library_concepts(
    net::http::Client& http, std::string_view package_type,
    std::size_t maximum_response_bytes,
    const psn::rest::LibraryPageObserver& observe)
    -> Expected<psn::rest::EntitlementConcepts, Unanswered> {
  return request()
      .asked_of_app_library<psn::rest::EntitlementConcepts,
                            psn::rest::LibraryEntitlements>(
          http, maximum_response_bytes,
          [package_type,
           &observe](psn::rest::LibraryEntitlements& entitlements) {
            return entitlements.concepts_of(package_type, observe);
          });
}

auto CatalogCalls::library_ps5_games(
    net::http::Client& http, std::size_t maximum_response_bytes,
    const psn::rest::LibraryPageObserver& observe)
    -> Expected<psn::rest::GameEntitlements, Unanswered> {
  return request()
      .asked_of_app_library<psn::rest::GameEntitlements,
                            psn::rest::LibraryEntitlements>(
          http, maximum_response_bytes,
          [&observe](psn::rest::LibraryEntitlements& entitlements) {
            return entitlements.owned_ps5_games(observe);
          });
}

auto CatalogCalls::library_ps5_game(net::http::Client& http,
                                    std::string_view id,
                                    std::size_t maximum_response_bytes)
    -> Expected<std::optional<psn::rest::GameEntitlement>, Unanswered> {
  return request()
      .asked_of_app_library<std::optional<psn::rest::GameEntitlement>,
                            psn::rest::LibraryEntitlements>(
          http, maximum_response_bytes,
          [id](psn::rest::LibraryEntitlements& entitlements) {
            return entitlements.owned_ps5_game(id);
          });
}

auto CatalogCalls::library_record(net::http::Client& http, std::string_view id,
                                  std::size_t maximum_response_bytes)
    -> Expected<psn::rest::EntitlementLookup, Unanswered> {
  return user_.in_flight().library_record.answer(
      AuthorisedRequest::question(maximum_response_bytes, id),
      http.stop_token(), [&] {
        return request().asked_of_library<psn::rest::EntitlementLookup>(
            http, [&](psn::auth::Authenticator& auth,
                      const psn::auth::BearerToken& token) {
              auto lookup =
                  psn::rest::CommerceClient{
                      http.limited(maximum_response_bytes)}
                      .internal_entitlement(
                          psn::auth::BearerToken{
                              token.secret().clone(),
                              AuthorisedRequest::library_credential},
                          id);
              if (lookup)
                auth.record_use(token, lookup.value().http_status);
              else if (!lookup.error().failure())
                auth.record_use(token, lookup.error().http_status());
              return lookup;
            });
      });
}

auto CatalogCalls::cloud_game_product(
    net::http::Client& http, const psn::rest::CloudGameQuestion& question,
    std::size_t maximum_response_bytes)
    -> Expected<psn::rest::CloudGameReply, Unanswered> {
  return request()
      .asked_of_app_library<psn::rest::CloudGameReply,
                            psn::rest::CloudGameProducts>(
          http, maximum_response_bytes,
          [&question](psn::rest::CloudGameProducts& products) {
            return products.first_listed(question);
          });
}

auto CatalogCalls::cloud_streaming(net::http::Client& http,
                                   CloudStreamingCache::Freshness freshness)
    -> CloudStreaming {
  if (user_.off_psn())
    return CloudStreaming::unknown(Unanswered{Unanswered::Reason::signed_out});
  auto& held = user_.cloud_streaming();
  if (freshness == CloudStreamingCache::Freshness::held_will_do)
    if (auto kept = held.held()) return std::move(*kept);
  const auto round = held.round();
  auto read = user_.in_flight().cloud_streaming.answer(
      {}, http.stop_token(), [&] { return read_cloud_streaming(http); });
  auto verdict =
      read ? std::move(read.value()) : CloudStreaming::unknown(read.error());
  held.keep(verdict, round);
  return verdict;
}

auto CatalogCalls::held_cloud_streaming() const
    -> std::optional<CloudStreaming> {
  if (user_.off_psn()) return std::nullopt;
  return user_.cloud_streaming().held();
}

auto CatalogCalls::settle_region() -> std::optional<Region> {
  if (auto known = user_.region()) return known;
  const auto context = user_.account_context();
  const auto language = context.language();
  if (!context.legal_country() || !language) return std::nullopt;
  user_.learn_region(Region{*context.legal_country(), *language});
  return user_.region();
}

auto CatalogCalls::played_titles(net::http::Client& http, std::size_t offset,
                                 std::size_t maximum_response_bytes)
    -> Expected<psn::rest::PlayedTitlesPage, Unanswered> {
  return request().played_titles(http, offset, maximum_response_bytes);
}

auto CatalogCalls::gallery_capture(net::http::Client& http,
                                   std::string_view gallery_id,
                                   std::size_t maximum_response_bytes)
    -> Expected<psn::rest::GalleryCapture, Unanswered> {
  return request().asked_of_console_directory<psn::rest::GalleryCapture>(
      http, [&](psn::auth::BearerToken token) {
        auto capped = http.limited(maximum_response_bytes);
        psn::rest::GameMedia media{capped, std::move(token)};
        auto found = media.capture(gallery_id);
        if (!found) return found;
        if (auto signed_address = media.download_address(gallery_id))
          found->download_url = std::move(*signed_address);
        return found;
      });
}

auto CatalogCalls::cloud_capture_download(net::http::Client& http,
                                          std::size_t maximum_response_bytes)
    -> Expected<bool, Unanswered> {
  return request().asked_of_console_directory<bool>(
      http, [&](psn::auth::BearerToken token) {
        auto capped = http.limited(maximum_response_bytes);
        return psn::rest::UserSettings{capped, std::move(token)}.switched_on(
            psn::rest::UserSettings::cloud_capture_download);
      });
}

auto CatalogCalls::read_cloud_streaming(net::http::Client& http)
    -> Expected<CloudStreaming, Unanswered> {
  using psn::rest::SubscriptionsClient;
  const auto feature = request()
                           .asked_of_app_library<psn::rest::CloudGameFeature,
                                                 SubscriptionsClient>(
                               http, net::http::default_maximum_response_bytes,
                               [](SubscriptionsClient& subscriptions) {
                                 return subscriptions.cloud_game();
                               });
  if (!feature) return unexpected(feature.error());
  auto benefits = benefits_of(feature.value());
  if (!benefits.refusal_needs_wording())
    return CloudStreaming::of(std::move(benefits));
  const auto plus =
      request()
          .asked_of_app_library<std::optional<psn::rest::PlusServiceState>,
                                SubscriptionsClient>(
              http, net::http::default_maximum_response_bytes,
              [](SubscriptionsClient& subscriptions) {
                return subscriptions.first_plus_service();
              });
  if (!plus && plus.error().reason == Unanswered::Reason::cancelled)
    return unexpected(plus.error());
  if (!plus || !plus.value()) return CloudStreaming::of(std::move(benefits));
  return CloudStreaming::of(std::move(benefits),
                            plus_service_of(*plus.value()));
}

auto CatalogCalls::benefits_of(const psn::rest::CloudGameFeature& feature) const
    -> CloudBenefits {
  std::vector<CloudBenefits::Owner> owners;
  for (const auto& owner : feature.owners) {
    std::vector<BenefitPlanId> plans;
    for (const auto& plan : owner.benefit_plan_ids)
      if (auto id = BenefitPlanId::parse(plan)) plans.push_back(std::move(*id));
    owners.push_back(
        CloudBenefits::Owner{.account_id = owner.owner_account_id,
                             .tier_label = owner.tier_label,
                             .plans = std::move(plans),
                             .has_est_benefit = owner.has_est_benefit,
                             .has_sgod_benefit = owner.has_sgod_benefit});
  }
  const auto read_at =
      CloudBenefits::Time{std::chrono::milliseconds{static_cast<std::int64_t>(
          user_.wall().now_microseconds() / microseconds_per_millisecond)}};
  return CloudBenefits{authorisation_of(feature.authorization),
                       feature.justification, std::move(owners), read_at};
}

auto CatalogCalls::authorisation_of(const std::optional<bool>& stated) noexcept
    -> CloudBenefits::Authorisation {
  using Authorisation = CloudBenefits::Authorisation;
  if (!stated) return Authorisation::unstated;
  return *stated ? Authorisation::granted : Authorisation::denied;
}

auto CatalogCalls::plus_service_of(const psn::rest::PlusServiceState& state)
    -> PlusService {
  return PlusService{state.active, state.status, state.tier_label};
}

auto CatalogCalls::opened_old_service_store(net::http::Client& http,
                                            std::size_t maximum_response_bytes)
    -> Expected<std::shared_ptr<const psn::rest::KamajiClient>, Unanswered> {
  using Reason = Unanswered::Reason;
  const auto duid = user_.device().view();
  psn::auth::Authenticator auth{
      http, user_.session().npsso().clone(),
      &psn::SessionInternals{user_.session()}.tokens()};
  const auto code = auth.legacy_cloud_stream_catalog_code(duid);
  if (!code) return unexpected(request().unanswered(code.error()));
  psn::rest::KamajiClient store{http.limited(maximum_response_bytes)};
  const auto created = store.create_session(
      psn::auth::AuthCode{code.value().secret().clone(),
                          AuthorisedRequest::old_service_credential},
      duid);
  if (!created) {
    const auto& failure = created.error().failure();
    return unexpected(Unanswered{failure && failure->is_cancelled()
                                     ? Reason::cancelled
                                     : Reason::unavailable,
                                 created.error().http_status()});
  }
  const auto& session = created.value();
  if (net::HttpStatus{session.http_status} != net::HttpStatus::ok &&
      net::HttpStatus{session.http_status}.is_server_error())
    return unexpected(Unanswered{Reason::unavailable, session.http_status});
  if (session.accepted)
    user_.learn_region(Region{session.country, session.language});
  if (net::HttpStatus{session.http_status} != net::HttpStatus::ok) {
    user_.refuse_sign_in();
    return unexpected(
        Unanswered{Reason::old_service_refused, session.http_status});
  }
  if (!session.accepted || !store.has_session()) {
    user_.refuse_sign_in();
    return unexpected(Unanswered{Reason::sign_in_expired, session.http_status});
  }
  return std::make_shared<const psn::rest::KamajiClient>(std::move(store));
}

}  // namespace libchiaking::users
