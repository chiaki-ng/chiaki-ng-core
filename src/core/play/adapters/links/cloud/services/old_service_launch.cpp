#include "src/core/play/adapters/links/cloud/services/old_service_launch.hpp"

#include "libchiaking/detail/catalog/store_locale.hpp"
#include "src/core/net/transport/http/status.hpp"
#include "src/core/net/transport/http/url.hpp"
#include "src/core/psn/legacy_cloud_stream_endpoints.hpp"
#include "src/core/psn/user_agents.hpp"
#include "src/core/utils/json/json.hpp"

#include <algorithm>
#include <utility>
#include <variant>

namespace libchiaking::play {

auto OldServiceLaunch::upgrade_url(
    std::span<const std::string> missing_elements) -> std::string {
  std::string elements;
  constexpr std::size_t elements_capacity = 511;
  for (const auto& name : missing_elements) {
    if (!elements.empty() && elements.size() < elements_capacity)
      elements += ',';
    elements += name.substr(
        0, elements_capacity - std::min(elements.size(), elements_capacity));
  }
  if (elements.empty()) return {};
  return "https://id.sonyentertainmentnetwork.com/id/upgrade_account_ca/"
         "?entry=upgrade_account&pr_referer=upgrade&redirect_uri=" +
         net::http::PercentEncoding::component_truncated(
             psn::legacy_cloud_stream_redirect_uri, 256) +
         "&applicationId=psnow&refererPage=websso"
         "&service_logo=ps&tp_console=true&disableLinks=SENLink&renderMode="
         "mobilePortrait&noEVBlock=true"
         "&displayFooter=none&hidePageElements=SENLogo&layout_type=popup&"
         "missing_"
         "elements=" +
         net::http::PercentEncoding::component_truncated(elements, 768) +
         "&response_type=code"
         "&service_entity=urn:service-entity:psn&smcid=pc:psnow&tp_psn=true&tp_"
         "social=true"
         "&elements_visibility_upgrade=no_cancel";
}

auto OldServiceLaunch::Error::privacy_required(
    std::uint16_t http_status, std::span<const std::string> missing_elements)
    -> Error {
  Error error{Stage::account_attributes, Cause::privacy_required, http_status};
  error.privacy_upgrade_url_ = upgrade_url(missing_elements);
  return error;
}

auto OldServiceLaunch::Error::not_free(std::string display_price) -> Error {
  Error error{Stage::checkout_preview, Cause::not_free};
  error.display_price_ = std::move(display_price);
  return error;
}

OldServiceLaunch::OldServiceLaunch(net::http::Client& client,
                                   users::PsUser& user)
    : OldServiceLaunch(client, user, LaunchReport{}, Options{}) {}

OldServiceLaunch::OldServiceLaunch(net::http::Client& client,
                                   users::PsUser& user, LaunchReport report)
    : OldServiceLaunch(client, user, report, Options{}) {}

OldServiceLaunch::OldServiceLaunch(net::http::Client& client,
                                   users::PsUser& user, LaunchReport report,
                                   Options options)
    : client_(client), user_(user), report_(report), options_(options) {}

auto OldServiceLaunch::platform_of(catalog::Platform platform) noexcept
    -> psn::StreamingPlatform {
  switch (platform) {
    case catalog::Platform::ps3:
      return psn::StreamingPlatform::ps3;
    case catalog::Platform::ps4:
      return psn::StreamingPlatform::ps4;
    case catalog::Platform::ps5:
      break;
  }
  return psn::StreamingPlatform::ps5;
}

auto OldServiceLaunch::takes_known_licence(
    const catalog::ClearedLaunch& cleared) noexcept -> bool {
  return cleared.held_as.has_value();
}

auto OldServiceLaunch::repeats_title_steps_after_refusal(
    const catalog::ClearedLaunch& cleared, unsigned round) noexcept -> bool {
  return round == 0 && takes_known_licence(cleared);
}

auto OldServiceLaunch::prepare(const catalog::ClearedLaunch& cleared,
                               unsigned round)
    -> Expected<OldServiceTitle, Error> {
  store_.reset();
  checkout_.reset();
  Request request;
  request.game_identifier = cleared.product.text();
  if (cleared.held_as) request.entitlement_id = cleared.held_as->text();
  request.platform = platform_of(cleared.platform);
  request.store_country = cleared.store.country();
  request.store_language = cleared.store.language();
  request.foreign_catalog = cleared.origin == catalog::ListOrigin::public_store;
  request.skip_account_attributes_check =
      options_.skip_account_attributes_check;
  request.force_resolve = round != 0 || !takes_known_licence(cleared);
  return run(request);
}

auto OldServiceLaunch::run(const Request& request)
    -> Expected<OldServiceTitle, Error> {
  auto title = request.force_resolve || request.entitlement_id.empty()
                   ? resolve(request)
                   : Expected<OldServiceTitle, Error>{OldServiceTitle{
                         request.entitlement_id, request.platform}};
  if (!title) return title;
  stage_ = Stage::account_session;
  if (!store_) store_.emplace(client_.limited(json::max_document_bytes));
  auto account = user_.old_service_account_session(
      client_, *store_, title.value().platform(), [this](Step step) {
        if (step == Step::account_code)
          announce(LaunchActivity::opening_account_session, "Authorizing", 4);
        else
          announce(LaunchActivity::opening_account_session, "Creating Session",
                   5);
      });
  if (!account) return unexpected(refused(account.error()));
  if (!account.value().accepted)
    return unexpected(error(Cause::rejected, account.value().http_status));
  return title;
}

auto OldServiceLaunch::resolve(const Request& request)
    -> Expected<OldServiceTitle, Error> {
  stage_ = Stage::anonymous_oauth;
  auto store = user_.old_service_title_store(client_, [this](Step step) {
    stage_ = stage_of(step);
    announce(LaunchActivity::opening_title_session, "Cloud Auth", 1);
  });
  if (!store) return unexpected(refused(store.error()));
  store_.emplace(std::move(store.value()));
  auto resolved = resolve_title(request);
  if (!resolved) return resolved;
  return license(request, std::move(resolved.value()));
}

auto OldServiceLaunch::stage_of(Step step) noexcept -> Stage {
  switch (step) {
    case Step::title_code:
      return Stage::anonymous_oauth;
    case Step::title_store:
      return Stage::anonymous_session;
    case Step::checkout_token:
      return Stage::commerce_oauth;
    case Step::account_attributes:
      return Stage::account_attributes;
    case Step::licence_status:
      return Stage::entitlement;
    case Step::preview:
      return Stage::checkout_preview;
    case Step::buy:
      return Stage::checkout;
    case Step::account_code:
    case Step::account_session:
      break;
  }
  return Stage::account_session;
}

auto OldServiceLaunch::refused(const users::OldServiceRefusal& refusal)
    -> Error {
  stage_ = stage_of(refusal.step());
  if (refusal.is_signed_out()) return error(Cause::invalid_request);
  if (const auto authorisation = refusal.authorisation())
    return auth_error(*authorisation);
  if (const auto failure = refusal.failure()) return failed(*failure);
  if (std::holds_alternative<psn::rest::AccountsError>(refusal.cause()))
    return Error::privacy_required(refusal.http_status(),
                                   refusal.missing_account_attributes());
  if (std::holds_alternative<users::OldServiceRefusal::NoStoreSession>(
          refusal.cause()))
    return error(Cause::invalid_response);
  const auto store = refusal.store();
  if (refusal.step() == Step::preview) {
    if (store && store->reason() == psn::rest::StoreError::Reason::ineligible)
      return error(Cause::store_region, store->http_status());
    if (store && store->reason() == psn::rest::StoreError::Reason::refused)
      return error(Cause::subscription_required, store->http_status());
    return error(Cause::invalid_response);
  }
  if (refusal.step() == Step::buy)
    return error(Cause::rejected, refusal.http_status());
  return error(Cause::invalid_response, refusal.http_status());
}

void OldServiceLaunch::announce(LaunchActivity activity, std::string_view label,
                                int step) const {
  report_.step(activity, net::protocol::remote_play::FailureStage::cloud_title,
               label, step, progress_steps);
}

auto OldServiceLaunch::error(Cause cause, std::uint16_t status) const -> Error {
  return {stage_, cause, status};
}

auto OldServiceLaunch::failed(const net::http::Failure& failure) const
    -> Error {
  switch (failure.kind()) {
    case net::http::Failure::Kind::cancelled:
      return error(Cause::cancelled);
    case net::http::Failure::Kind::invalid_request:
      return error(Cause::invalid_request);
    case net::http::Failure::Kind::transport:
    case net::http::Failure::Kind::too_large:
      break;
  }
  return error(Cause::transport);
}

auto OldServiceLaunch::auth_error(const psn::auth::AuthError& failure) const
    -> Error {
  switch (failure.kind()) {
    case psn::auth::AuthErrorKind::cancelled:
      return error(Cause::cancelled);
    case psn::auth::AuthErrorKind::transport:
      return error(Cause::transport);
    case psn::auth::AuthErrorKind::invalid_request:
    case psn::auth::AuthErrorKind::invalid_npsso:
      return error(Cause::invalid_request);
    default:
      break;
  }
  return error(Cause::invalid_response, failure.http_status());
}

auto OldServiceLaunch::resolve_title(const Request& request)
    -> Expected<OldServiceTitle, Error> {
  stage_ = Stage::title;
  announce(LaunchActivity::resolving_title, "Resolving Game", 2);
  std::string country{
      request.store_country.empty()
          ? "US"
          : request.store_country.substr(0, store_component_limit)};
  std::string language{
      request.store_language.empty()
          ? "en"
          : request.store_language.substr(0, store_component_limit)};
  const bool legacy = OldServiceTitle::is_legacy(request.game_identifier);
  if (request.foreign_catalog && legacy) {
    country = catalog::StoreLocale::shared_store_country(country);
    language = "en";
  }
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto document = store_->title_container<OldServiceTitle::Reply>(
        country, language, request.game_identifier);
    if (!document) {
      const auto& refused = document.error();
      if (refused.failure()) return unexpected(failed(*refused.failure()));
      const auto status = refused.http_status();
      if (net::HttpStatus{status} == net::HttpStatus::not_found) {
        const auto fallback =
            catalog::StoreLocale::shared_store_country(country);
        if (!attempt && request.foreign_catalog && !legacy &&
            (country != fallback || language != "en")) {
          country = fallback;
          language = "en";
          continue;
        }
        return unexpected(error(Cause::not_found, status));
      }
      if (refused.reason() == psn::rest::StoreError::Reason::refused)
        return unexpected(error(Cause::rejected, status));
      return unexpected(error(Cause::invalid_response, status));
    }
    auto title =
        OldServiceTitle::decode(document.value(), request.game_identifier);
    if (!title)
      return unexpected(
          error(Cause::no_entitlement, net::HttpStatus::ok.code()));
    return std::move(*title);
  }
  return unexpected(error(Cause::not_found));
}

auto OldServiceLaunch::check_attributes() -> Expected<void, Error> {
  stage_ = Stage::account_attributes;
  const auto checked = checkout_->account_attributes();
  if (!checked) return unexpected(refused(checked.error()));
  return {};
}

auto OldServiceLaunch::entitlement_status(const OldServiceTitle& title)
    -> Expected<std::uint16_t, Error> {
  stage_ = Stage::entitlement;
  const auto reply = checkout_->licence_status(title.entitlement());
  if (!reply) return unexpected(refused(reply.error()));
  return reply.value();
}

auto OldServiceLaunch::license(const Request& request, OldServiceTitle title)
    -> Expected<OldServiceTitle, Error> {
  stage_ = Stage::commerce_oauth;
  announce(LaunchActivity::taking_free_licence, "Checking License", 3);
  auto checkout = user_.old_service_checkout(client_);
  if (!checkout) return unexpected(refused(checkout.error()));
  checkout_.emplace(std::move(checkout.value()));
  if (!request.skip_account_attributes_check) {
    auto checked = check_attributes();
    if (!checked) return unexpected(checked.error());
  }
  const auto reply = entitlement_status(title);
  if (!reply) return unexpected(reply.error());
  const auto status = reply.value();
  if (net::HttpStatus{status} == net::HttpStatus::ok) return title;
  if (net::HttpStatus{status} != net::HttpStatus::not_found)
    return unexpected(error(Cause::rejected, status));
  if (request.foreign_catalog &&
      OldServiceTitle::is_legacy(request.game_identifier))
    return title;
  const auto acquired = [&]() -> Expected<void, Error> {
    if (title.is_outside_every_plan()) {
      stage_ = Stage::title;
      return unexpected(
          error(Cause::outside_streaming_plan, net::HttpStatus::ok.code()));
    }
    auto previewed = preview(title);
    if (!previewed) return unexpected(previewed.error());
    title = std::move(previewed.value());
    return buy(title);
  }();
  if (!acquired &&
      !(request.foreign_catalog && !acquired.error().is_cancelled()))
    return unexpected(acquired.error());
  return title;
}

auto OldServiceLaunch::preview(const OldServiceTitle& title)
    -> Expected<OldServiceTitle, Error> {
  stage_ = Stage::checkout_preview;
  announce(LaunchActivity::taking_free_licence, "Acquiring License", 3);
  auto preview = checkout_->preview(*store_, title.checkout_sku());
  if (!preview) return unexpected(refused(preview.error()));
  if (!preview.value().free)
    return unexpected(
        Error::not_free(std::move(preview.value().display_price)));
  OldServiceTitle priced{title};
  if (!preview.value().sku.empty()) priced.replace_sku(preview.value().sku);
  return priced;
}

auto OldServiceLaunch::buy(const OldServiceTitle& title)
    -> Expected<void, Error> {
  stage_ = Stage::checkout;
  const auto bought = checkout_->buy(*store_, title.sku());
  if (!bought) return unexpected(refused(bought.error()));
  return {};
}

}  // namespace libchiaking::play
