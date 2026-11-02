#include "src/core/psn/auth/authenticator.hpp"

#include "src/core/net/transport/http/status.hpp"
#include "src/core/net/transport/http/strict_url.hpp"
#include "src/core/net/transport/http/url.hpp"
#include "src/core/psn/legacy_cloud_stream_endpoints.hpp"
#include "src/core/psn/user_agents.hpp"
#include "src/core/utils/json/ps_reply.hpp"
#include "src/core/utils/memory/constant_time.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/integer.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>

namespace libchiaking::psn::auth {

class Authenticator::AuthorizeUrl {
 public:
  AuthorizeUrl(std::string_view base, Encoding encoding)
      : query_(base), encoding_(encoding) {}

  auto add(std::string_view name, std::string_view value) -> AuthorizeUrl& {
    if (encoding_ == Encoding::percent)
      query_.add(name, value);
    else
      query_.add_verbatim(name, value);
    return *this;
  }

  auto scope(std::string_view value) -> AuthorizeUrl& {
    if (encoding_ == Encoding::percent) return add("scope", value);
    std::string joined;
    bool first = true;
    for (const auto part : std::views::split(value, ' ')) {
      if (!first) joined.append("%20");
      first = false;
      joined.append(std::string_view{part.begin(), part.end()});
    }
    return add("scope", joined);
  }

  auto origin(const Application& application) -> AuthorizeUrl& {
    return add("smcid", application.smcid)
        .add("applicationId", application.application_id);
  }

  auto mid(const Application& application) -> AuthorizeUrl& {
    return add("mid", application.mid);
  }

  auto application(const Application& application) -> AuthorizeUrl& {
    return origin(application).mid(application);
  }

  auto legacy_cloud_stream_page() -> AuthorizeUrl& {
    return add("renderMode", "mobilePortrait")
        .add("hidePageElements", "forgotPasswordLink")
        .add("displayFooter", "none")
        .add("disableLinks", "qriocityLink");
  }

  auto legacy_cloud_stream_popup() -> AuthorizeUrl& {
    return add("layout_type", "popup")
        .add("service_logo", "ps")
        .add("tp_psn", "true")
        .add("noEVBlock", "true");
  }

  auto take() && -> SecretString { return std::move(query_).take(); }

 private:
  net::http::Query query_;
  Encoding encoding_;
};

struct Authenticator::Authorized {
  SecretString credential;
  std::uint32_t expires_in{};
  std::uint16_t http_status{};
};

struct Authenticator::AuthorizationFlow {
  const net::http::Endpoint* endpoint{};
  Grant grant;
  SecretString url;
  std::string_view redirect_uri;
  std::string_view user_agent;
  HeaderOrder order{HeaderOrder::cookie_first};
  std::string_view token_label;

  auto clone() const -> AuthorizationFlow {
    return AuthorizationFlow{endpoint,   grant, url.clone(), redirect_uri,
                             user_agent, order, token_label};
  }
};

struct Authenticator::IssuedToken {
  SecretString token;
  Grant served;
  AuthorizationFlow flow;
  bool cached{};
};

struct Authenticator::AuthorizeCheckBody {
  std::string_view client_id;
  std::string_view scope;
  std::string_view redirect_uri;
  std::string_view response_type;
  std::string_view service_entity;
  std::string_view duid;
};

auto Authenticator::unreserved(char value) noexcept -> bool {
  return text::ascii_alnum(value) || value == '-' || value == '_' ||
         value == '.' || value == '~';
}

auto Authenticator::valid_client_id(std::string_view id) noexcept -> bool {
  return !id.empty() && id.size() <= gk_id_max_length &&
         std::ranges::all_of(id, unreserved);
}

auto Authenticator::supported(StreamingService service,
                              StreamingPlatform platform) noexcept -> bool {
  if (service == StreamingService::cloud_stream)
    return platform == StreamingPlatform::ps5;
  return service == StreamingService::legacy_cloud_stream &&
         (platform == StreamingPlatform::ps3 ||
          platform == StreamingPlatform::ps4 ||
          platform == StreamingPlatform::ps5);
}

auto Authenticator::visible(std::string_view value) noexcept -> bool {
  return std::ranges::all_of(value, text::ascii_visible);
}

auto Authenticator::trimmed(std::string_view value) noexcept
    -> std::string_view {
  return text::trim_http_space(value);
}

auto Authenticator::gk_route(StreamingService service) noexcept
    -> const GkRoute& {
  static constexpr GkRoute cloud_stream{
      Authorization::authorize_url, cloud_stream_redirect_uri,
      cloud_stream_http_user_agent, qlite_application,
      cloud_stream_client_scope,    false};
  static constexpr GkRoute legacy_cloud_stream{
      authorize_url,
      legacy_cloud_stream_redirect_uri,
      legacy_cloud_stream_http_user_agent,
      legacy_cloud_stream_application,
      legacy_cloud_stream_gk_client_scope,
      true};
  return service == StreamingService::cloud_stream ? cloud_stream
                                                   : legacy_cloud_stream;
}

auto Authenticator::location_expires_in(std::string_view location)
    -> std::uint32_t {
  const auto text = net::http::StrictUrl::first_secret_value(
      location, "expires_in", expires_in_digits + 2);
  const auto digits = text.view();
  if (digits.empty() || digits.size() > expires_in_digits) return 0;
  const auto value = text::parse_integer<std::uint64_t>(digits);
  if (!value) return 0;
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(
      *value, std::numeric_limits<std::uint32_t>::max()));
}

auto Authenticator::redirect_code(std::string_view location,
                                  std::string_view redirect_uri)
    -> std::optional<SecretString> {
  const auto query = net::http::StrictUrl::redirect_query(
      location, redirect_uri, location_limit);
  if (!query) return std::nullopt;
  auto code = net::http::StrictUrl::only_secret_value(
      query.value(), "code", "error",
      {.encoded_bytes = location_limit, .decoded_bytes = code_capacity - 1});
  if (!code) return std::nullopt;
  return std::move(code.value());
}

auto Authenticator::redirect_token(std::string_view location)
    -> std::optional<SecretString> {
  auto value = net::http::StrictUrl::first_secret_value(
      location, "access_token", location_limit + 1);
  if (value.size() >= token_capacity) return std::nullopt;
  return value;
}

auto Authenticator::grant_text(const Grant& grant) -> std::string {
  return std::format("client={} scope=\"{}\"", grant.client_id, grant.scope);
}

auto Authenticator::failure_of(Authorization::Fault fault) noexcept
    -> AuthError {
  switch (fault) {
    case Authorization::Fault::invalid_npsso:
      return AuthError{AuthErrorKind::invalid_npsso};
    case Authorization::Fault::invalid_request:
      return AuthError{AuthErrorKind::invalid_request};
    case Authorization::Fault::cancelled:
      return AuthError{AuthErrorKind::cancelled};
    case Authorization::Fault::transport:
      break;
  }
  return AuthError{AuthErrorKind::transport};
}

auto Authenticator::authorization(
    const net::http::Endpoint& endpoint, std::string_view client_id,
    std::string_view scope, ResponseType response, AuthorizeUrl url,
    std::string_view redirect_uri, std::string_view user_agent,
    HeaderOrder order, std::string_view token_label) -> AuthorizationFlow {
  return AuthorizationFlow{
      &endpoint,
      Grant{std::string{client_id}, std::string{scope}, response},
      std::move(url).take(),
      redirect_uri,
      user_agent,
      order,
      token_label};
}

auto Authenticator::commerce_flow(std::string_view scope) -> AuthorizationFlow {
  AuthorizeUrl url{authorize_url, Encoding::percent};
  url.add("response_type", "token")
      .scope(scope)
      .add("client_id", legacy_cloud_stream_commerce_client_id)
      .add("redirect_uri", legacy_cloud_stream_redirect_uri)
      .add("service_entity", Authorization::service_entity)
      .add("prompt", "none");
  return authorization(
      commerce_token_endpoint, legacy_cloud_stream_commerce_client_id, scope,
      ResponseType::token, std::move(url), legacy_cloud_stream_redirect_uri,
      kamaji_user_agent, HeaderOrder::cookie_first,
      scope == owned_entitlements_scope ? owned_entitlements_token_label
                                        : std::string_view{});
}

auto Authenticator::checkout_flow(std::string_view duid) -> AuthorizationFlow {
  AuthorizeUrl url{authorize_url, Encoding::verbatim};
  url.origin(legacy_cloud_stream_application)
      .add("response_type", "token")
      .scope(legacy_cloud_stream_checkout_scope)
      .add("client_id", legacy_cloud_stream_commerce_client_id)
      .add("redirect_uri", legacy_cloud_stream_redirect_uri)
      .add("grant_type", "authorization_code")
      .add("service_entity", Authorization::service_entity)
      .add("prompt", "none")
      .legacy_cloud_stream_page()
      .mid(legacy_cloud_stream_application)
      .add("duid", duid)
      .legacy_cloud_stream_popup();
  return authorization(
      commerce_token_endpoint, legacy_cloud_stream_commerce_client_id,
      legacy_cloud_stream_checkout_scope, ResponseType::token, std::move(url),
      legacy_cloud_stream_redirect_uri, legacy_cloud_stream_http_user_agent,
      HeaderOrder::user_agent_first, legacy_cloud_stream_checkout_token_label);
}

auto Authenticator::legacy_cloud_stream_code_flow(std::string_view duid,
                                                  std::string_view scope,
                                                  bool sign_in_page,
                                                  Encoding encoding)
    -> AuthorizationFlow {
  AuthorizeUrl url{authorize_url, encoding};
  url.origin(legacy_cloud_stream_application)
      .add("response_type", "code")
      .scope(scope)
      .add("client_id", legacy_cloud_stream_client_id)
      .add("redirect_uri", legacy_cloud_stream_redirect_uri)
      .add("service_entity", Authorization::service_entity)
      .add("prompt", "none");
  if (sign_in_page) url.legacy_cloud_stream_page();
  url.mid(legacy_cloud_stream_application)
      .add("duid", duid)
      .legacy_cloud_stream_popup();
  return authorization(
      legacy_cloud_stream_code_endpoint, legacy_cloud_stream_client_id, scope,
      ResponseType::code, std::move(url), legacy_cloud_stream_redirect_uri,
      legacy_cloud_stream_http_user_agent, HeaderOrder::user_agent_first);
}

auto Authenticator::bounded(AuthorizationFlow flow)
    -> std::optional<AuthorizationFlow> {
  if (!Authorization::sendable(flow.url.view())) return std::nullopt;
  return flow;
}

auto Authenticator::gk_client_flow(StreamingService service,
                                   std::string_view client_id, const Duid& duid)
    -> std::optional<AuthorizationFlow> {
  if (!valid_client_id(client_id)) return std::nullopt;
  const auto& route = gk_route(service);
  AuthorizeUrl url{route.authorize_url, Encoding::verbatim};
  url.add("response_type", "code")
      .add("client_id", client_id)
      .add("redirect_uri", route.redirect_uri)
      .add("service_entity", Authorization::service_entity)
      .add("prompt", "none")
      .add("duid", duid.view())
      .application(route.application)
      .scope(route.client_scope);
  if (route.sign_in_page)
    url.legacy_cloud_stream_page().legacy_cloud_stream_popup();
  return bounded(
      authorization(gk_client_code_endpoint, client_id, route.client_scope,
                    ResponseType::code, std::move(url), route.redirect_uri,
                    route.user_agent, HeaderOrder::user_agent_first));
}

auto Authenticator::gk_server_flow(StreamingService service,
                                   StreamingPlatform platform,
                                   std::string_view client_id, const Duid& duid)
    -> std::optional<AuthorizationFlow> {
  if (!valid_client_id(client_id)) return std::nullopt;
  const auto& route = gk_route(service);
  const bool ps3 = service == StreamingService::legacy_cloud_stream &&
                   platform == StreamingPlatform::ps3;
  const auto scope = service == StreamingService::cloud_stream
                         ? cloud_stream_server_scope
                     : ps3 ? legacy_cloud_stream_ps3_store_scope
                           : legacy_cloud_stream_gk_server_scope;
  AuthorizeUrl url{route.authorize_url, Encoding::verbatim};
  url.add("response_type", "code")
      .add("redirect_uri", route.redirect_uri)
      .add("service_entity", Authorization::service_entity)
      .add("prompt", "none")
      .add("client_id", client_id)
      .application(route.application)
      .scope(scope);
  if (!ps3) url.add("duid", duid.view());
  if (route.sign_in_page)
    url.legacy_cloud_stream_page().legacy_cloud_stream_popup();
  return bounded(authorization(gk_server_code_endpoint, client_id, scope,
                               ResponseType::code, std::move(url),
                               route.redirect_uri, route.user_agent,
                               HeaderOrder::user_agent_first));
}

Authenticator::Authenticator(net::http::Client& client, Npsso npsso,
                             TokenCache* cache)
    : client_(client),
      authorization_(client),
      npsso_(std::move(npsso)),
      cache_(cache) {}

Authenticator::~Authenticator() = default;

void Authenticator::report(std::string_view label,
                           const AuthError& error) const {
  authorization_.report(label, error.kind() == AuthErrorKind::cancelled,
                        error.name(), error.http_status(),
                        std::format(" auth={}", NpssoKind::label));
}

auto Authenticator::authorize(const AuthorizationFlow& flow, const Npsso& npsso)
    -> Expected<Authorized, AuthError> {
  auto sent = authorization_.request(*flow.endpoint, flow.url.clone(), npsso,
                                     flow.user_agent, flow.order);
  if (!sent) return unexpected(failure_of(sent.error()));
  const auto& response = sent.value();
  const auto status = response.status();
  if (net::HttpStatus{status} != net::HttpStatus::found)
    return unexpected(AuthError{net::HttpStatus{status}.is_server_error()
                                    ? AuthErrorKind::server_error
                                    : AuthErrorKind::rejected,
                                status});
  const auto header = response.header("Location");
  const auto location =
      header ? trimmed(text::c_string(*header)) : std::string_view{};
  if (location.empty() || location.size() > location_limit)
    return unexpected(AuthError{AuthErrorKind::malformed, status});
  if (!net::http::StrictUrl::first_secret_value(location, "error",
                                                error_capacity)
           .empty())
    return unexpected(AuthError{AuthErrorKind::refused, status});
  const bool token = flow.grant.response == ResponseType::token;
  auto credential = token ? redirect_token(location)
                          : redirect_code(location, flow.redirect_uri);
  if (!credential || credential->empty() || !visible(credential->view()))
    return unexpected(AuthError{AuthErrorKind::malformed, status});
  return Authorized{std::move(*credential), location_expires_in(location),
                    status};
}

auto Authenticator::exchange(const AuthorizationFlow& flow, const Npsso& npsso,
                             std::string_view occasion)
    -> Expected<Exchanged, AuthError> {
  authorization_.log(LogSeverity::debug, flow.endpoint->label,
                     std::format("{}; exchanging {} {}", occasion,
                                 NpssoKind::label, grant_text(flow.grant)));
  auto authorized = authorize(flow, npsso);
  if (!authorized) return unexpected(authorized.error());
  auto& value = authorized.value();
  return Exchanged{BearerToken{std::move(value.credential)}, value.expires_in,
                   value.http_status};
}

auto Authenticator::lease(const AuthorizationFlow& flow)
    -> Expected<Lease, AuthError> {
  if (cache_ == nullptr)
    return exchange(flow, npsso_, "no token cache")
        .transform([&flow](Exchanged exchanged) {
          return Lease{std::move(exchanged.token), flow.grant, false};
        });
  auto acquired =
      cache_->acquire(flow.grant, flow.token_label, client_.stop_token(),
                      [this, &flow](const Npsso& npsso) {
                        return exchange(flow, npsso, "token cache miss");
                      });
  if (acquired && acquired.value().cached)
    authorization_.log(
        LogSeverity::debug, flow.endpoint->label,
        std::format("token cache hit {} served by {}", grant_text(flow.grant),
                    grant_text(acquired.value().grant)));
  return acquired;
}

auto Authenticator::token(AuthorizationFlow flow)
    -> Expected<BearerToken, AuthError> {
  auto leased = lease(flow);
  if (!leased) {
    report(flow.endpoint->label, leased.error());
    return unexpected(leased.error());
  }
  auto& held = leased.value();
  remember(IssuedToken{held.token.secret().clone(), std::move(held.grant),
                       std::move(flow), held.cached});
  return std::move(held.token);
}

auto Authenticator::code(const AuthorizationFlow& flow)
    -> Expected<AuthCode, AuthError> {
  auto authorized = authorize(flow, npsso_);
  if (!authorized) {
    report(flow.endpoint->label, authorized.error());
    return unexpected(authorized.error());
  }
  return AuthCode{std::move(authorized.value().credential)};
}

auto Authenticator::issued(const BearerToken& token) const noexcept
    -> IssuedToken* {
  const auto wanted = std::as_bytes(std::span{token.view()});
  auto newest_first = issued_ | std::views::reverse;
  const auto found =
      std::ranges::find_if(newest_first, [wanted](const auto& held) {
        return memory::constant_time_equal(
            std::as_bytes(std::span{held->token.view()}), wanted);
      });
  return found == newest_first.end() ? nullptr : found->get();
}

void Authenticator::remember(IssuedToken issued) {
  const auto key = GrantKey::of(issued.flow.grant);
  std::erase_if(issued_, [&issued, &key](const auto& held) {
    return held->flow.endpoint == issued.flow.endpoint &&
           GrantKey::of(held->flow.grant) == key;
  });
  issued_.push_back(std::make_unique<IssuedToken>(std::move(issued)));
}

auto Authenticator::library_grant() -> Grant {
  return Grant{std::string{legacy_cloud_stream_commerce_client_id},
               std::string{owned_entitlements_scope}, ResponseType::token};
}

auto Authenticator::old_service_checkout_grant() -> Grant {
  return Grant{std::string{legacy_cloud_stream_commerce_client_id},
               std::string{legacy_cloud_stream_checkout_scope},
               ResponseType::token};
}

auto Authenticator::commerce_token(std::string_view scope)
    -> Expected<BearerToken, AuthError> {
  return token(commerce_flow(scope));
}

auto Authenticator::legacy_cloud_stream_checkout_token(std::string_view duid)
    -> Expected<BearerToken, AuthError> {
  return token(checkout_flow(duid));
}

auto Authenticator::legacy_cloud_stream_catalog_code(std::string_view duid)
    -> Expected<AuthCode, AuthError> {
  return code(legacy_cloud_stream_code_flow(
      duid, legacy_cloud_stream_store_scope, true, Encoding::percent));
}

auto Authenticator::legacy_cloud_stream_title_code(std::string_view duid)
    -> Expected<AuthCode, AuthError> {
  return code(legacy_cloud_stream_code_flow(
      duid, legacy_cloud_stream_store_scope, true, Encoding::verbatim));
}

auto Authenticator::legacy_cloud_stream_account_code(std::string_view duid,
                                                     StreamingPlatform platform)
    -> Expected<AuthCode, AuthError> {
  return code(legacy_cloud_stream_code_flow(
      duid,
      platform == StreamingPlatform::ps3 ? legacy_cloud_stream_ps3_store_scope
                                         : legacy_cloud_stream_store_scope,
      false, Encoding::verbatim));
}

auto Authenticator::gk_codes(StreamingService service,
                             StreamingPlatform platform, const GkClientIds& ids,
                             std::string_view duid)
    -> Expected<GkCodes, AuthError> {
  return gk_codes(service, platform, ids, duid, {});
}

auto Authenticator::gk_codes(StreamingService service,
                             StreamingPlatform platform, const GkClientIds& ids,
                             std::string_view duid,
                             const std::function<void(GkStep)>& before)
    -> Expected<GkCodes, AuthError> {
  const auto device = Duid::parse(duid);
  if (!supported(service, platform) || !device)
    return unexpected(AuthError{AuthErrorKind::invalid_request});
  auto client_flow = gk_client_flow(service, ids.gk_client_id, *device);
  if (!client_flow)
    return unexpected(AuthError{AuthErrorKind::invalid_request});
  auto server_flow = gk_server_flow(service, platform,
                                    service == StreamingService::cloud_stream
                                        ? ids.stream_server_client_id
                                        : ids.ps3_gk_client_id,
                                    *device);
  if (!server_flow)
    return unexpected(AuthError{AuthErrorKind::invalid_request});
  if (before) before(GkStep::client);
  auto client = code(*client_flow);
  if (!client) return unexpected(client.error());
  if (before) before(GkStep::server);
  auto server = code(*server_flow);
  if (!server) return unexpected(server.error());
  return GkCodes{std::move(client.value()), std::move(server.value())};
}

auto Authenticator::authorize_check(StreamingService service,
                                    std::string_view duid)
    -> Expected<void, AuthError> {
  if (client_.stop_requested())
    return unexpected(AuthError{AuthErrorKind::cancelled});
  if (npsso_.refusal())
    return unexpected(AuthError{AuthErrorKind::invalid_npsso});
  const auto device = Duid::parse(duid);
  if (!device || (service != StreamingService::legacy_cloud_stream &&
                  service != StreamingService::cloud_stream))
    return unexpected(AuthError{AuthErrorKind::invalid_request});
  const bool legacy_cloud_stream =
      service == StreamingService::legacy_cloud_stream;
  const auto body = json::ps::write(AuthorizeCheckBody{
      .client_id = legacy_cloud_stream ? legacy_cloud_stream_client_id
                                       : cloud_stream_client_id,
      .scope = legacy_cloud_stream ? legacy_cloud_stream_store_scope
                                   : cloud_stream_client_scope,
      .redirect_uri = legacy_cloud_stream ? legacy_cloud_stream_redirect_uri
                                          : cloud_stream_redirect_uri,
      .response_type = "code",
      .service_entity = Authorization::service_entity,
      .duid = device->view()});
  if (!body) return unexpected(AuthError{AuthErrorKind::invalid_request});
  net::http::Request request{authorize_check_endpoint, authorize_check_url};
  request.body("application/json; charset=UTF-8",
               SecretBytes{body.value().bytes()});
  request.header("User-Agent", legacy_cloud_stream
                                   ? legacy_cloud_stream_http_user_agent
                                   : cloud_stream_http_user_agent);
  request.authorize(npsso_);
  auto sent = client_.send(std::move(request));
  const auto failed = [&](AuthError error) -> Expected<void, AuthError> {
    report(authorize_check_endpoint.label, error);
    return unexpected(error);
  };
  if (!sent) return failed(failure_of(Authorization::fault_of(sent.error())));
  if (!net::HttpStatus{sent.value().status()}.is_success())
    return failed(AuthError{AuthErrorKind::rejected, sent.value().status()});
  return {};
}

auto Authenticator::renewable(const BearerToken& token) const noexcept -> bool {
  const auto* held = issued(token);
  return held != nullptr && held->cached && cache_ != nullptr &&
         !client_.stop_requested();
}

auto Authenticator::refresh(const BearerToken& refused,
                            std::uint16_t http_status)
    -> Expected<BearerToken, AuthError> {
  auto* held = issued(refused);
  if (held == nullptr)
    return unexpected(AuthError{AuthErrorKind::invalid_request});
  authorization_.log(
      LogSeverity::info, held->flow.endpoint->label,
      std::format("cached token refused with HTTP {}; refreshing once {}",
                  http_status, grant_text(held->served)));
  if (cache_ != nullptr) {
    cache_->record_retry(held->served);
    cache_->invalidate(held->served, refused);
  }
  auto renewed = token(held->flow.clone());
  if (renewed) issued(renewed.value())->cached = false;
  return renewed;
}

void Authenticator::record_use(const BearerToken& token,
                               std::uint16_t http_status) {
  if (cache_ == nullptr) return;
  if (const auto* held = issued(token))
    cache_->record_use(held->served, http_status);
}

void Authenticator::forget(const BearerToken& refused) {
  if (cache_ == nullptr || refused.empty()) return;
  if (const auto* held = issued(refused))
    cache_->invalidate(held->served, refused);
}

}  // namespace libchiaking::psn::auth
