#include "src/core/psn/auth/sign_in.hpp"

#include "src/core/net/transport/http/client.hpp"
#include "src/core/net/transport/http/status.hpp"
#include "src/core/net/transport/http/strict_url.hpp"
#include "src/core/net/transport/http/url.hpp"
#include "src/core/psn/auth/app_token_slot.hpp"
#include "src/core/psn/auth/authorization.hpp"
#include "src/core/psn/auth/duid.hpp"
#include "src/core/psn/legacy_cloud_stream_endpoints.hpp"
#include "src/core/psn/online_id.hpp"
#include "src/core/utils/json/ps_reply.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/base64.hpp"
#include "src/core/utils/text/uuid.hpp"

#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/http/provider.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <utility>

namespace libchiaking::psn::auth {

class SignIn::Impl {
 public:
  enum class Audience : std::uint8_t {
    remote_play,
    app,
  };

  class Realm;

  enum class GrantType : std::uint8_t {
    authorization_code,
    sso_token,
    refresh_token,
  };

  Impl(platform::HttpProvider& http, const platform::ClockProvider& clock,
       platform::StopToken stop, platform::DiagnosticsProvider* log)
      : client_(http, clock, stop, log),
        unlogged_(http, clock, std::move(stop)),
        authorization_(client_) {}

  [[nodiscard]] auto code(const Realm& realm, const Npsso& npsso,
                          std::string_view duid,
                          std::string_view correlation_id)
      -> Expected<AuthCode, SignInError> {
    if (client_.stop_requested())
      return report(authorize_endpoint,
                    failed(SignInErrorKind::cancelled, 0,
                           "stop was requested before the request"));
    auto query = realm.authorize_query(duid, correlation_id);
    if (!query)
      return report(authorize_endpoint,
                    failure_of(Authorization::Fault::invalid_request));
    const auto sent = authorization_.request(
        authorize_endpoint, std::move(*query).take(), npsso, kamaji_user_agent,
        Authorization::HeaderOrder::user_agent_first);
    if (!sent) return report(authorize_endpoint, failure_of(sent.error()));
    const auto status = sent.value().status();
    if (std::ranges::find(redirect_statuses, status) == redirect_statuses.end())
      return report(authorize_endpoint,
                    net::HttpStatus{status} == net::HttpStatus::ok
                        ? failed(SignInErrorKind::rejected, status,
                                 "the server asked for an interactive sign-in")
                        : status_failure(status));
    auto parsed = realm.redirect_code(
        text::trim(sent.value().header("Location").value_or("")), status);
    if (!parsed) return report(authorize_endpoint, unexpected(parsed.error()));
    return parsed;
  }

  template <class Access, class Refresh, class Credential>
  [[nodiscard]] auto tokens(const Realm& realm, GrantType grant,
                            const Credential& credential,
                            std::string_view app_duid = {})
      -> Expected<IssuedTokens<Access, Refresh>, SignInError> {
    if (client_.stop_requested())
      return report(token_endpoint,
                    failed(SignInErrorKind::cancelled, 0,
                           "stop was requested before the request"));
    if (credential.refusal())
      return report(token_endpoint, failed(SignInErrorKind::invalid_request, 0,
                                           "the credential is not usable"));
    const bool refreshing = grant == GrantType::refresh_token;
    const bool remote_play = realm.for_remote_play();
    net::http::Request request{token_endpoint, remote_play && refreshing
                                                   ? legacy_token_url
                                                   : token_url};
    request.form(realm.token_form(grant, credential.view(), app_duid));
    request.carries(credential);
    if (!remote_play || refreshing) request.authorize(realm.client_basic());
    const auto sent = client_.send(std::move(request));
    if (!sent)
      return report(token_endpoint,
                    failure_of(Authorization::fault_of(sent.error())));
    if (net::HttpStatus{sent.value().status()} != net::HttpStatus::ok)
      return report(token_endpoint, status_failure(sent.value().status()));
    auto issued = realm.issued_tokens<Access, Refresh>(sent.value());
    if (!issued) return report(token_endpoint, unexpected(issued.error()));
    return issued;
  }

  [[nodiscard]] auto account(const BearerToken& token)
      -> Expected<Account, SignInError> {
    if (client_.stop_requested())
      return report(account_endpoint,
                    failed(SignInErrorKind::cancelled, 0,
                           "stop was requested before the request"));
    if (token.refusal())
      return report(account_endpoint,
                    failed(SignInErrorKind::invalid_request, 0,
                           "the access token is not usable"));
    SecretString url{legacy_token_url};
    url.push_back('/');
    net::http::PercentEncoding::append(url, token.view());
    net::http::Request request{account_endpoint, std::move(url)};
    request.carries(token);
    request.authorize(Realm::remote_play().client_basic())
        .header("Content-Type", "application/json");
    authorization_.log(
        LogSeverity::debug, account_endpoint.label,
        std::format("GET {}/*** auth={}",
                    net::http::Scrubber::log_target(legacy_token_url, false),
                    BearerKind::label));
    const auto sent = unlogged_.send(std::move(request));
    if (!sent)
      return report(account_endpoint,
                    failure_of(Authorization::fault_of(sent.error())));
    if (net::HttpStatus{sent.value().status()} != net::HttpStatus::ok)
      return report(account_endpoint, status_failure(sent.value().status()));
    auto parsed = account_of(sent.value());
    if (!parsed) return report(account_endpoint, unexpected(parsed.error()));
    return parsed;
  }

 private:
  using LogSeverity = platform::LogSeverity;

  static constexpr std::string_view token_url =
      "https://ca.account.sony.com/api/authz/v3/oauth/token";

  static constexpr std::string_view legacy_token_url =
      "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token";

  static_assert(token_url.starts_with(Authorization::account_api));

  static constexpr std::size_t response_limit = 1024U * 1024U;

  static constexpr std::size_t location_max_bytes = 8192;

  static constexpr std::size_t encoded_max_bytes = 6144;

  static constexpr std::size_t decoded_max_bytes = 2048;

  static constexpr std::uint64_t expires_in_max_seconds = 315'360'000;

  static constexpr std::array<std::uint16_t, 4> redirect_statuses{302, 303, 307,
                                                                  308};

  struct ClientBasicKind {
    static constexpr std::string_view label = "client basic";
    static constexpr net::http::CredentialPlacement placement{"Authorization",
                                                              "Basic "};
    static constexpr platform::HttpCredential credential =
        platform::HttpCredential::client_credentials;
  };

  using ClientBasic = net::http::Credential<ClientBasicKind>;

  struct TokenReply {
    json::ps::Tolerant<SecretString> access_token;
    json::ps::Tolerant<SecretString> refresh_token;
    json::ps::Tolerant<std::uint64_t> expires_in;
    glz::raw_json token_type;
    json::ps::Tolerant<std::uint64_t> refresh_token_expires_in;
  };

  struct AccountReply {
    json::ps::Tolerant<std::string> user_id;
    json::ps::Tolerant<std::string> online_id;
  };

  static constexpr net::http::Endpoint authorize_endpoint{
      .label = "psn.auth.sign_in.authorize",
      .maximum_response_bytes = response_limit,
  };

  static constexpr net::http::Endpoint token_endpoint{
      .label = "psn.auth.sign_in.token",
      .method = net::http::Method::post,
      .maximum_response_bytes = response_limit,
  };

  static constexpr net::http::Endpoint account_endpoint{
      .label = "psn.auth.sign_in.account",
      .maximum_response_bytes = response_limit,
  };

  static auto failed(SignInErrorKind kind, std::uint16_t http_status = 0,
                     std::string_view cause = {}) -> Unexpected<SignInError> {
    return unexpected(SignInError{kind, http_status, cause});
  }

  static auto failure_of(Authorization::Fault fault)
      -> Unexpected<SignInError> {
    switch (fault) {
      case Authorization::Fault::invalid_npsso:
      case Authorization::Fault::invalid_request:
        return failed(SignInErrorKind::invalid_request, 0,
                      "the npsso, DUID or correlation id is not usable");
      case Authorization::Fault::cancelled:
        return failed(SignInErrorKind::cancelled, 0,
                      "the request was cancelled");
      case Authorization::Fault::transport:
        break;
    }
    return failed(SignInErrorKind::transport, 0,
                  "the request did not complete");
  }

  static auto status_failure(std::uint16_t status) -> Unexpected<SignInError> {
    if (net::HttpStatus{status} == net::HttpStatus::bad_request ||
        net::HttpStatus{status}.is_auth_refused())
      return failed(SignInErrorKind::rejected, status,
                    "the server refused the credential");
    if (net::HttpStatus{status} == net::HttpStatus::too_many_requests)
      return failed(SignInErrorKind::rate_limited, status,
                    "the server is rate limiting sign-in");
    return failed(SignInErrorKind::unavailable, status,
                  "the server answered with an unexpected status");
  }

  template <class Reply>
  [[nodiscard]] static auto reply_of(const net::http::Response& response)
      -> std::optional<Reply> {
    if (response.oversized()) return std::nullopt;
    auto reply = json::ps::read_reply<Reply>(text::c_string(response.body()));
    if (!reply) return std::nullopt;
    return std::move(reply.value());
  }

  static auto lifetime(std::uint64_t seconds) noexcept
      -> std::optional<std::chrono::seconds> {
    if (seconds == 0 || seconds > expires_in_max_seconds) return std::nullopt;
    return std::chrono::seconds{seconds};
  }

  static auto bearer(const glz::raw_json& type) -> bool {
    const auto named = json::ps::read_reply<std::string>(type.str);
    return named && text::ieq(text::c_string(named.value()), "bearer");
  }

 public:
  class Realm {
    constexpr Realm(Audience audience, std::string_view client_id,
                    std::string_view client_secret,
                    std::string_view redirect_uri,
                    std::string_view scope) noexcept
        : audience_(audience),
          client_id_(client_id),
          client_secret_(client_secret),
          redirect_uri_(redirect_uri),
          scope_(scope) {}

   public:
    static auto remote_play() noexcept -> const Realm& {
      static constexpr Realm realm{
          Audience::remote_play, "ba495a24-818c-472b-b12d-ff231c1b5745",
          "mvaiZkRsAsI1IBkY",
          "https://remoteplay.dl.playstation.net/remoteplay/redirect",
          "psn:clientapp referenceDataService:countryConfig.read "
          "pushNotification:webSocket.desktop.connect "
          "sessionManager:remotePlaySession.system.update"};
      return realm;
    }

    static auto app() noexcept -> const Realm& {
      static constexpr Realm realm{
          Audience::app, app_client_id, "ucPjka5tntB2KqsP",
          "com.scee.psxandroid.scecompcall://redirect", app_scope};
      return realm;
    }

    auto for_remote_play() const noexcept -> bool {
      return audience_ == Audience::remote_play;
    }

    [[nodiscard]] auto redirect_code(std::string_view location,
                                     std::uint16_t status) const
        -> Expected<AuthCode, SignInError> {
      const auto query = net::http::StrictUrl::redirect_query(
          location, redirect_uri_, location_max_bytes);
      if (!query &&
          query.error() == net::http::StrictUrl::RedirectFault::elsewhere)
        return failed(SignInErrorKind::malformed, status,
                      "the redirect does not lead to the expected address");
      if (!query)
        return failed(SignInErrorKind::malformed, status,
                      "the redirect has no query");
      auto code = net::http::StrictUrl::only_secret_value(
          query.value(), "code", "error",
          {.encoded_bytes = encoded_max_bytes,
           .decoded_bytes = decoded_max_bytes});
      if (code) return AuthCode{std::move(code.value())};
      switch (code.error()) {
        case net::http::StrictUrl::QueryFault::undecodable:
          return failed(SignInErrorKind::malformed, status,
                        "the redirect query cannot be decoded");
        case net::http::StrictUrl::QueryFault::refused:
          return failed(SignInErrorKind::rejected, status,
                        "the redirect reports an error");
        case net::http::StrictUrl::QueryFault::ambiguous:
          return failed(
              SignInErrorKind::malformed, status,
              "the redirect carries more than one code or an empty one");
        case net::http::StrictUrl::QueryFault::unusable:
          break;
      }
      return failed(SignInErrorKind::malformed, status,
                    "the redirect carries no usable authorization code");
    }

    [[nodiscard]] auto authorize_query(std::string_view duid,
                                       std::string_view correlation_id) const
        -> std::optional<net::http::Query> {
      net::http::Query url{Authorization::authorize_url};
      url.add("client_id", client_id_)
          .add("redirect_uri", redirect_uri_)
          .add("scope", scope_)
          .add("response_type", "code")
          .add("access_type", "offline")
          .add("service_entity", Authorization::service_entity);
      if (audience_ != Audience::remote_play) return url;
      const auto device = Duid::parse(duid);
      const auto correlation = text::Uuid::parse(correlation_id);
      if (!device || !correlation) return std::nullopt;
      url.add("duid", device->view())
          .add("smcid", "remoteplay")
          .add("layout_type", "popup")
          .add("PlatformPrivacyWs1", "minimal")
          .add("no_captcha", "true")
          .add("cid", correlation->view());
      return url;
    }

    auto token_form(GrantType grant, std::string_view credential,
                    std::string_view app_duid) const -> net::http::Form {
      const bool refreshing = grant == GrantType::refresh_token;
      net::http::Form form;
      if (grant == GrantType::sso_token) {
        form.add("token_format", "jwt")
            .add("grant_type", "sso_token")
            .add("npsso", credential)
            .add("scope", scope_)
            .add("service_entity", Authorization::service_entity)
            .add("duid", app_duid);
        return form;
      }
      form.add("grant_type",
               refreshing ? "refresh_token" : "authorization_code");
      form.add(refreshing ? "refresh_token" : "code", credential);
      if (audience_ == Audience::remote_play) {
        form.add("redirect_uri", redirect_uri_).add("scope", scope_);
        if (!refreshing)
          form.add("client_id", client_id_)
              .add("client_secret", client_secret_);
        return form;
      }
      if (refreshing)
        form.add("scope", scope_);
      else
        form.add("redirect_uri", redirect_uri_);
      form.add("token_format", "jwt");
      return form;
    }

    auto client_basic() const -> ClientBasic {
      SecretString pair{client_id_};
      pair.push_back(':');
      pair.append(client_secret_);
      SecretString encoded;
      text::base64::append(encoded, pair.bytes(),
                           text::base64::Alphabet::standard);
      return ClientBasic{std::move(encoded)};
    }

    template <class Access, class Refresh>
    [[nodiscard]] auto issued_tokens(const net::http::Response& response) const
        -> Expected<IssuedTokens<Access, Refresh>, SignInError> {
      const auto status = response.status();
      auto reply = reply_of<TokenReply>(response);
      if (!reply)
        return failed(SignInErrorKind::malformed, status,
                      "the token reply is not a JSON object");
      const bool typed = !reply->token_type.str.empty();
      const bool kept_type = audience_ == Audience::remote_play
                                 ? bearer(reply->token_type)
                                 : !typed || bearer(reply->token_type);
      const bool refresh_optional = audience_ == Audience::app;
      const bool has_refresh =
          reply->refresh_token && Refresh::admits(reply->refresh_token->view());
      if (!reply->access_token ||
          !Access::admits(reply->access_token->view()) ||
          (!has_refresh && !refresh_optional) || !reply->expires_in ||
          !kept_type)
        return failed(SignInErrorKind::malformed, status,
                      "the token reply lacks a usable token, type or lifetime");
      const auto expires_in = lifetime(*reply->expires_in);
      if (!expires_in)
        return failed(SignInErrorKind::malformed, status,
                      "the token lifetime is out of range");
      const auto refresh_expires_in =
          has_refresh && reply->refresh_token_expires_in
              ? lifetime(*reply->refresh_token_expires_in)
              : std::nullopt;
      return IssuedTokens<Access, Refresh>{
          Access{std::move(*reply->access_token)},
          has_refresh ? Refresh{std::move(*reply->refresh_token)}
                      : Refresh{SecretString{}},
          *expires_in, refresh_expires_in};
    }

   private:
    Audience audience_;
    std::string_view client_id_;
    std::string_view client_secret_;
    std::string_view redirect_uri_;
    std::string_view scope_;
  };

 private:
  [[nodiscard]] static auto account_of(const net::http::Response& response)
      -> Expected<Account, SignInError> {
    const auto status = response.status();
    const auto reply = reply_of<AccountReply>(response);
    if (!reply)
      return failed(SignInErrorKind::malformed, status,
                    "the account reply is not a JSON object");
    const auto id = AccountId::parse(
        text::c_string(reply->user_id.value_or(std::string{})));
    auto online_id = OnlineId::parse(
        text::c_string(reply->online_id.value_or(std::string{})));
    if (!id || !online_id)
      return failed(SignInErrorKind::malformed, status,
                    "the account reply lacks a user id or an online id");
    return Account{*id, std::move(*online_id).take()};
  }

  auto report(const net::http::Endpoint& endpoint,
              Unexpected<SignInError> failure) const
      -> Unexpected<SignInError> {
    const auto error = failure.error().at(endpoint.label);
    authorization_.report(
        endpoint.label, error.kind() == SignInErrorKind::cancelled,
        error.name(), error.http_status(), std::format(": {}", error.cause()));
    return unexpected(error);
  }

  net::http::Client client_;
  net::http::Client unlogged_;
  Authorization authorization_;
};

SignIn::SignIn(platform::HttpProvider& http,
               const platform::ClockProvider& clock, platform::StopToken stop,
               platform::DiagnosticsProvider* log)
    : impl_(std::make_unique<Impl>(http, clock, std::move(stop), log)) {}

SignIn::~SignIn() = default;

auto SignIn::remote_play_tokens(const Npsso& npsso, std::string_view duid,
                                std::string_view correlation_id)
    -> Expected<RemotePlayTokens, SignInError> {
  const auto code =
      impl_->code(Impl::Realm::remote_play(), npsso, duid, correlation_id);
  if (!code) return unexpected(code.error());
  return impl_->tokens<BearerToken, RemotePlayRefreshToken>(
      Impl::Realm::remote_play(), Impl::GrantType::authorization_code,
      code.value());
}

auto SignIn::app_tokens(const Npsso& npsso, std::string_view duid)
    -> Expected<AppTokens, SignInError> {
  const auto device = Duid::parse(duid);
  if (!device)
    return unexpected(SignInError{SignInErrorKind::invalid_request}.at(
        "psn.auth.sign_in.token"));
  SecretString app_duid{"00000007000801a8"};
  const auto unique = device->view().substr(16);
  while (app_duid.size() < 122) {
    app_duid.append(
        unique.substr(0, std::min(unique.size(), 122 - app_duid.size())));
  }
  return impl_->tokens<AppToken, AppRefreshToken>(
      Impl::Realm::app(), Impl::GrantType::sso_token, npsso, app_duid.view());
}

auto SignIn::refresh(const RemotePlayRefreshToken& token)
    -> Expected<RemotePlayTokens, SignInError> {
  return impl_->tokens<BearerToken, RemotePlayRefreshToken>(
      Impl::Realm::remote_play(), Impl::GrantType::refresh_token, token);
}

auto SignIn::refresh(const AppRefreshToken& token)
    -> Expected<AppTokens, SignInError> {
  return impl_->tokens<AppToken, AppRefreshToken>(
      Impl::Realm::app(), Impl::GrantType::refresh_token, token);
}

auto SignIn::account(const BearerToken& token)
    -> Expected<Account, SignInError> {
  return impl_->account(token);
}

auto SignIn::sign_in(const Npsso& npsso, std::string_view duid,
                     std::string_view correlation_id)
    -> Expected<SignedIn, SignInError> {
  auto remote_play = remote_play_tokens(npsso, duid, correlation_id);
  if (!remote_play) return unexpected(remote_play.error());
  auto identity = account(remote_play.value().access);
  if (!identity) return unexpected(identity.error());
  auto app = app_tokens(npsso, duid);
  if (!app) return unexpected(app.error());
  return SignedIn{std::move(identity.value()), std::move(remote_play.value()),
                  std::move(app.value())};
}

}  // namespace libchiaking::psn::auth
