#include "src/core/users/ports/play_calls.hpp"

#include "src/core/users/ports/authorised_request.hpp"

#include <utility>

namespace libchiaking::users {

PlayCalls::PlayCalls(PsUser& user) noexcept : user_(PsUser::Core::of(user)) {}

void PlayCalls::membership_rejected_by_service() {
  AuthorisedRequest{user_}.membership_rejected_by_service();
}

auto PlayCalls::cloud_device() const
    -> Expected<DeviceId, AuthorisationRefusal> {
  if (user_.off_psn()) return unexpected(AuthorisationRefusal::signed_out());
  return user_.device().clone();
}

auto PlayCalls::cloud_device_check(net::http::Client& http,
                                   psn::StreamingService service)
    -> Expected<void, AuthorisationRefusal> {
  if (user_.off_psn()) return unexpected(AuthorisationRefusal::signed_out());
  return user_.in_flight().cloud_device_check.answer(
      std::to_string(static_cast<unsigned>(service)), http.stop_token(),
      [&]() -> Expected<void, AuthorisationRefusal> {
        psn::auth::Authenticator auth{http, user_.session().npsso().clone()};
        const auto checked =
            auth.authorize_check(service, user_.device().view());
        if (!checked) return unexpected(unauthorised(checked.error()));
        return {};
      });
}

auto PlayCalls::cloud_streaming_codes(
    net::http::Client& http, psn::StreamingService service,
    psn::StreamingPlatform platform, const psn::auth::GkClientIds& ids,
    const std::function<void(psn::auth::GkStep)>& before)
    -> Expected<psn::auth::GkCodes, AuthorisationRefusal> {
  if (user_.off_psn()) return unexpected(AuthorisationRefusal::signed_out());
  psn::auth::Authenticator auth{http, user_.session().npsso().clone()};
  auto codes =
      auth.gk_codes(service, platform, ids, user_.device().view(), before);
  if (!codes) return unexpected(unauthorised(codes.error()));
  return std::move(codes.value());
}

auto PlayCalls::old_service_title_store(
    net::http::Client& http,
    const std::function<void(OldServiceRefusal::Step)>& before)
    -> Expected<psn::rest::KamajiClient, OldServiceRefusal> {
  using Step = OldServiceRefusal::Step;
  if (user_.off_psn())
    return unexpected(
        OldServiceRefusal{Step::title_code, OldServiceRefusal::SignedOut{}});
  const auto duid = user_.device().view();
  psn::auth::Authenticator auth{http, user_.session().npsso().clone()};
  if (before) before(Step::title_code);
  const auto code = auth.legacy_cloud_stream_title_code(duid);
  if (!code)
    return unexpected(old_service_refused(Step::title_code, code.error()));
  if (before) before(Step::title_store);
  psn::rest::KamajiClient store{http.limited(json::max_document_bytes)};
  const auto created = store.create_session(
      psn::auth::AuthCode{code.value().secret().clone(),
                          AuthorisedRequest::old_service_credential},
      duid);
  if (!created)
    return unexpected(
        OldServiceRefusal::from(Step::title_store, created.error()));
  if (!store.has_session())
    return unexpected(OldServiceRefusal{
        Step::title_store,
        OldServiceRefusal::NoStoreSession{created.value().http_status}});
  return store;
}

auto PlayCalls::old_service_account_session(
    net::http::Client& http, psn::rest::KamajiClient& store,
    psn::StreamingPlatform platform,
    const std::function<void(OldServiceRefusal::Step)>& before)
    -> Expected<psn::rest::KamajiSession, OldServiceRefusal> {
  using Step = OldServiceRefusal::Step;
  if (user_.off_psn())
    return unexpected(
        OldServiceRefusal{Step::account_code, OldServiceRefusal::SignedOut{}});
  const auto duid = user_.device().view();
  psn::auth::Authenticator auth{http, user_.session().npsso().clone()};
  if (before) before(Step::account_code);
  const auto code = auth.legacy_cloud_stream_account_code(duid, platform);
  if (!code)
    return unexpected(old_service_refused(Step::account_code, code.error()));
  if (before) before(Step::account_session);
  auto created = store.create_session(
      psn::auth::AuthCode{code.value().secret().clone(),
                          AuthorisedRequest::old_service_credential},
      duid);
  if (!created)
    return unexpected(
        OldServiceRefusal::from(Step::account_session, created.error()));
  return std::move(created.value());
}

auto PlayCalls::old_service_checkout(net::http::Client& http)
    -> Expected<OldServiceCheckout, OldServiceRefusal> {
  if (user_.off_psn())
    return unexpected(OldServiceRefusal{OldServiceRefusal::Step::checkout_token,
                                        OldServiceRefusal::SignedOut{}});
  return OldServiceCheckout{user_, http};
}

auto PlayCalls::remote_play_access(const platform::StopToken& stop)
    -> Expected<RemotePlayAccess, Unanswered> {
  using Reason = Unanswered::Reason;
  if (user_.off_psn()) return unexpected(Unanswered{Reason::signed_out});
  if (auto restored = user_.restore_remote_play_access(stop); !restored)
    return unexpected(restored.error());
  auto token = user_.remote_play_token();
  if (!token) return unexpected(Unanswered{Reason::remote_play_access_missing});
  return RemotePlayAccess{
      user_, psn::auth::BearerToken{token->secret().clone(),
                                    AuthorisedRequest::remote_play_credential}};
}

auto PlayCalls::unauthorised(const psn::auth::AuthError& error)
    -> AuthorisationRefusal {
  AuthorisationRefusal refusal{error};
  if (refusal.expires_sign_in()) user_.refuse_sign_in();
  return refusal;
}

auto PlayCalls::old_service_refused(OldServiceRefusal::Step step,
                                    const psn::auth::AuthError& error)
    -> OldServiceRefusal {
  OldServiceRefusal refusal{step, error};
  if (refusal.expires_sign_in()) user_.refuse_sign_in();
  return refusal;
}

}  // namespace libchiaking::users
