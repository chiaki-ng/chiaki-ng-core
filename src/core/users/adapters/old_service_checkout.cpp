#include "src/core/users/adapters/old_service_checkout.hpp"

#include "src/core/users/adapters/ps_user_core.hpp"

#include "src/core/net/transport/http/status.hpp"
#include "src/core/psn/session_internals.hpp"
#include "src/core/psn/user_agents.hpp"
#include "src/core/users/adapters/authorisation_refusal.hpp"
#include "src/core/utils/json/json.hpp"
#include "src/core/utils/retry/with_renewal.hpp"

#include <utility>

namespace libchiaking::users {

OldServiceRefusal::OldServiceRefusal(Step step, Cause cause)
    : step_(step), cause_(std::move(cause)) {}

auto OldServiceRefusal::from(Step step, const psn::rest::StoreError& error)
    -> OldServiceRefusal {
  if (error.failure()) return {step, *error.failure()};
  return {step, error};
}

auto OldServiceRefusal::from(Step step, psn::rest::AccountsError error)
    -> OldServiceRefusal {
  if (error.failure()) return {step, *error.failure()};
  return {step, std::move(error)};
}

auto OldServiceRefusal::is_cancelled() const noexcept -> bool {
  if (const auto* refused = std::get_if<psn::auth::AuthError>(&cause_))
    return refused->kind() == psn::auth::AuthErrorKind::cancelled;
  if (const auto* failed = std::get_if<net::http::Failure>(&cause_))
    return failed->is_cancelled();
  return false;
}

auto OldServiceRefusal::expires_sign_in() const noexcept -> bool {
  const auto* refused = std::get_if<psn::auth::AuthError>(&cause_);
  return refused != nullptr && AuthorisationRefusal::expires_sign_in(*refused);
}

auto OldServiceRefusal::http_status() const noexcept -> std::uint16_t {
  if (const auto* refused = std::get_if<psn::auth::AuthError>(&cause_))
    return refused->http_status();
  if (const auto* store = std::get_if<psn::rest::StoreError>(&cause_))
    return store->http_status();
  if (const auto* attributes = std::get_if<psn::rest::AccountsError>(&cause_))
    return attributes->http_status();
  if (const auto* session = std::get_if<NoStoreSession>(&cause_))
    return session->http_status;
  return 0;
}

auto OldServiceRefusal::authorisation() const noexcept
    -> std::optional<psn::auth::AuthError> {
  if (const auto* refused = std::get_if<psn::auth::AuthError>(&cause_))
    return *refused;
  return std::nullopt;
}

auto OldServiceRefusal::failure() const noexcept
    -> std::optional<net::http::Failure> {
  if (const auto* failed = std::get_if<net::http::Failure>(&cause_))
    return *failed;
  return std::nullopt;
}

auto OldServiceRefusal::store() const noexcept
    -> std::optional<psn::rest::StoreError> {
  if (const auto* refused = std::get_if<psn::rest::StoreError>(&cause_))
    return *refused;
  return std::nullopt;
}

auto OldServiceRefusal::missing_account_attributes() const noexcept
    -> std::span<const std::string> {
  if (const auto* attributes = std::get_if<psn::rest::AccountsError>(&cause_))
    return attributes->missing_elements();
  return {};
}

OldServiceCheckout::OldServiceCheckout(PsUser::Core& user,
                                       net::http::Client& http)
    : user_(&user),
      auth_(std::make_unique<psn::auth::Authenticator>(
          http, user.session().npsso().clone(),
          &psn::SessionInternals{user.session()}.tokens())),
      commerce_(http.limited(json::max_document_bytes),
                psn::legacy_cloud_stream_http_user_agent),
      accounts_(http.limited(json::max_document_bytes),
                psn::legacy_cloud_stream_http_user_agent) {}

OldServiceCheckout::OldServiceCheckout(OldServiceCheckout&&) noexcept = default;

OldServiceCheckout::~OldServiceCheckout() = default;

auto OldServiceCheckout::refused(const psn::auth::AuthError& error)
    -> OldServiceRefusal {
  OldServiceRefusal refusal{Step::checkout_token, error};
  if (refusal.expires_sign_in()) user_->refuse_sign_in();
  return refusal;
}

auto OldServiceCheckout::token() -> Expected<void, OldServiceRefusal> {
  if (token_) return {};
  if (unobtainable_) return unexpected(*unobtainable_);
  auto issued =
      auth_->legacy_cloud_stream_checkout_token(user_->device().view());
  if (!issued) {
    unobtainable_.emplace(refused(issued.error()));
    return unexpected(*unobtainable_);
  }
  token_.emplace(std::move(issued.value()));
  return {};
}

auto OldServiceCheckout::bearer() const -> psn::auth::BearerToken {
  return psn::auth::BearerToken{token_->secret().clone(), bearer_credential};
}

void OldServiceCheckout::used(std::uint16_t status) {
  auth_->record_use(*token_, status);
}

auto OldServiceCheckout::renewable(std::uint16_t status) const noexcept
    -> bool {
  return net::HttpStatus{status}.is_auth_refused() && auth_->renewable(*token_);
}

auto OldServiceCheckout::renewed(std::uint16_t refused_status)
    -> Expected<void, OldServiceRefusal> {
  auto fresh = auth_->refresh(*token_, refused_status);
  if (!fresh) return unexpected(refused(fresh.error()));
  token_.emplace(std::move(fresh.value()));
  return {};
}

auto OldServiceCheckout::account_attributes()
    -> Expected<psn::rest::AttributesAccepted, OldServiceRefusal> {
  if (auto held = token(); !held) return unexpected(held.error());
  const auto ask = [this] {
    auto checked = accounts_.validate_attributes(bearer());
    if (checked)
      used(checked.value().http_status);
    else if (!checked.error().failure())
      used(checked.error().http_status());
    return checked;
  };
  using Refused = decltype(ask())::error_type;
  auto answered = retry::with_renewal(
      ask,
      [this](const Refused& refused) {
        return !refused.failure() && renewable(refused.http_status());
      },
      [this](const Refused& refused) {
        return renewed(refused.http_status());
      });
  if (!answered) return unexpected(answered.error());
  return std::move(answered.value()).transform_error([](Refused refused) {
    return OldServiceRefusal::from(Step::account_attributes,
                                   std::move(refused));
  });
}

auto OldServiceCheckout::licence_status(std::string_view entitlement)
    -> Expected<std::uint16_t, OldServiceRefusal> {
  if (auto held = token(); !held) return unexpected(held.error());
  const auto ask =
      [this, entitlement]() -> Expected<std::uint16_t, OldServiceRefusal> {
    auto lookup = commerce_.internal_entitlement(bearer(), entitlement);
    if (!lookup && lookup.error().failure())
      return unexpected(
          OldServiceRefusal{Step::licence_status, *lookup.error().failure()});
    const auto replied =
        lookup ? lookup.value().http_status : lookup.error().http_status();
    used(replied);
    return replied;
  };
  auto reply = ask();
  if (!reply || !renewable(reply.value())) return reply;
  if (auto fresh = renewed(reply.value()); !fresh)
    return unexpected(fresh.error());
  return ask();
}

auto OldServiceCheckout::preview(psn::rest::KamajiClient& store,
                                 std::string_view sku)
    -> Expected<psn::rest::CheckoutPreview, OldServiceRefusal> {
  if (auto held = token(); !held) return unexpected(held.error());
  auto previewed = store.buynow_preview(bearer(), sku);
  if (!previewed) {
    if (!previewed.error().failure()) used(previewed.error().http_status());
    return unexpected(
        OldServiceRefusal::from(Step::preview, previewed.error()));
  }
  used(net::HttpStatus::ok.code());
  return std::move(previewed.value());
}

auto OldServiceCheckout::buy(psn::rest::KamajiClient& store,
                             std::string_view sku)
    -> Expected<void, OldServiceRefusal> {
  if (auto held = token(); !held) return unexpected(held.error());
  const auto bought = store.buynow(bearer(), sku);
  if (!bought) {
    if (!bought.error().failure()) used(bought.error().http_status());
    return unexpected(OldServiceRefusal::from(Step::buy, bought.error()));
  }
  used(net::HttpStatus::ok.code());
  return {};
}

}  // namespace libchiaking::users
