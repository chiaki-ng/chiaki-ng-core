#include "src/core/psn/auth/qr_sign_in.hpp"

#include "src/core/net/transport/http/status.hpp"
#include "src/core/net/transport/http/strict_url.hpp"
#include "src/core/net/transport/http/url.hpp"
#include "src/core/psn/rest/error_reply.hpp"
#include "src/core/utils/json/ps_reply.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/base64.hpp"
#include "src/core/utils/text/integer.hpp"

#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/http/provider.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>
#include <vector>

namespace libchiaking::psn::auth {

struct QrSignIn::TokenReply {
  json::ps::Tolerant<SecretString> access_token;
};

struct QrSignIn::TicketReply {
  json::ps::Tolerant<SecretString> authentication_ticket;
  json::ps::Tolerant<SecretString> user_code;
  json::ps::Tolerant<std::uint64_t> interval;
  json::ps::Tolerant<std::uint64_t> expires_in;
};

struct QrSignIn::ApprovalReply {
  json::ps::Tolerant<SecretString> npsso;
};

struct QrSignIn::RefusalReply {
  json::ps::Tolerant<std::int64_t> error_code;
};

auto UserCode::parse(std::string_view text) -> std::optional<UserCode> {
  if (text.size() != digits || !std::ranges::all_of(text, text::ascii_digit))
    return std::nullopt;
  return UserCode{text};
}

QrSignIn::QrSignIn(platform::HttpProvider& http,
                   const platform::ClockProvider& clock,
                   platform::StopToken stop, platform::DiagnosticsProvider* log)
    : client_(http, clock, std::move(stop), log), authorization_(client_) {}

auto QrSignIn::registration() -> Expected<HostedRegistration, SignInError> {
  auto page = sent(page_endpoint, net::http::Request{page_endpoint, page_url});
  if (!page) return unexpected(page.error());
  if (net::HttpStatus{page.value().status()} != net::HttpStatus::ok)
    return report(page_endpoint, status_failure(page.value().status()));
  auto bundle = bundle_url(page.value().body());
  if (!bundle) return report(page_endpoint, bundle.error());
  auto script = sent(bundle_endpoint,
                     net::http::Request{bundle_endpoint, bundle.value()});
  if (!script) return unexpected(script.error());
  if (net::HttpStatus{script.value().status()} != net::HttpStatus::ok)
    return report(bundle_endpoint, status_failure(script.value().status()));
  auto registered = registration_of(script.value().body());
  if (!registered) return report(bundle_endpoint, registered.error());
  return registered;
}

auto QrSignIn::application_token(const HostedRegistration& registration)
    -> Expected<ApplicationToken, SignInError> {
  if (registration.client_secret.refusal() || registration.client_id.empty())
    return report(token_endpoint,
                  SignInError{SignInErrorKind::invalid_request, 0,
                              "the registration is not usable"});
  net::http::Form form;
  form.add("grant_type", "client_credentials")
      .add("client_id", registration.client_id)
      .add("client_secret", registration.client_secret.view())
      .add("scope", registration.scope);
  net::http::Request request{token_endpoint, token_url};
  request.form(std::move(form));
  request.carries(registration.client_secret);
  auto reply = sent(token_endpoint, std::move(request));
  if (!reply) return unexpected(reply.error());
  const auto status = reply.value().status();
  if (net::HttpStatus{status} != net::HttpStatus::ok)
    return report(token_endpoint, status_failure(status));
  auto token = reply_of<TokenReply>(reply.value());
  auto admitted = token && token->access_token
                      ? ApplicationToken::admit(std::move(*token->access_token))
                      : unexpected(ApplicationToken::Refusal::empty);
  if (!admitted)
    return report(token_endpoint,
                  SignInError{SignInErrorKind::malformed, status,
                              "the token reply lacks a usable token"});
  return std::move(admitted.value());
}

auto QrSignIn::ticket(const ApplicationToken& token,
                      std::string_view device_name)
    -> Expected<Ticket, SignInError> {
  if (token.refusal())
    return report(ticket_endpoint, SignInError{SignInErrorKind::invalid_request,
                                               0, "the token is not usable"});
  const auto body = json::ps::write(TicketBody{device_name});
  if (!body)
    return report(ticket_endpoint,
                  SignInError{SignInErrorKind::invalid_request, 0,
                              "the device name cannot be written"});
  net::http::Request request{ticket_endpoint, ticket_url};
  request.authorize(token).json(body.value());
  auto reply = sent(ticket_endpoint, std::move(request));
  if (!reply) return unexpected(reply.error());
  if (net::HttpStatus{reply.value().status()} != net::HttpStatus::created)
    return report(ticket_endpoint, status_failure(reply.value().status()));
  auto issued = ticket_of(reply.value());
  if (!issued) return report(ticket_endpoint, issued.error());
  return issued;
}

auto QrSignIn::poll(const ApplicationToken& token, const TicketHandle& handle)
    -> Expected<PollAnswer, SignInError> {
  if (token.refusal() || handle.refusal())
    return report(poll_endpoint,
                  SignInError{SignInErrorKind::invalid_request, 0,
                              "the token or ticket is not usable"});
  const auto body = json::ps::write(
      PollBody{"authentication_ticket", handle.secret().clone()});
  if (!body)
    return report(poll_endpoint,
                  SignInError{SignInErrorKind::invalid_request, 0,
                              "the ticket cannot be written"});
  net::http::Request request{poll_endpoint, poll_url};
  request.authorize(token).carries(handle).json(body.value());
  auto reply = sent(poll_endpoint, std::move(request));
  if (!reply) return unexpected(reply.error());
  const auto status = reply.value().status();
  if (net::HttpStatus{status} == net::HttpStatus::too_many_requests)
    return slowed(reply.value());
  if (net::HttpStatus{status} != net::HttpStatus::ok)
    return refusal_of(reply.value());
  auto approval = reply_of<ApprovalReply>(reply.value());
  auto npsso = approval && approval->npsso
                   ? Npsso::admit(std::move(*approval->npsso))
                   : unexpected(Npsso::Refusal::empty);
  if (!npsso)
    return report(poll_endpoint,
                  SignInError{SignInErrorKind::malformed, status,
                              "the approval lacks a usable npsso"});
  return PollAnswer{TicketApproved{std::move(npsso.value())}};
}

auto QrSignIn::approval_url(const Ticket& ticket) -> SecretString {
  net::http::Query url{approval_page};
  url.add("code", ticket.user_code.view())
      .add("cid", ticket.correlation_id)
      .add("service_entity", Authorization::service_entity)
      .add("elements_visibility", "psportal");
  return std::move(url).take();
}

auto QrSignIn::sent(const net::http::Endpoint& endpoint,
                    net::http::Request request)
    -> Expected<net::http::Response, SignInError> {
  if (client_.stop_requested())
    return report(endpoint,
                  SignInError{SignInErrorKind::cancelled, 0,
                              "stop was requested before the request"});
  auto reply = client_.send(std::move(request));
  if (!reply)
    return report(endpoint, reply.error().is_cancelled()
                                ? SignInError{SignInErrorKind::cancelled, 0,
                                              "the request was cancelled"}
                                : SignInError{SignInErrorKind::transport, 0,
                                              "the request did not complete"});
  return std::move(reply.value());
}

auto QrSignIn::report(const net::http::Endpoint& endpoint, SignInError error,
                      std::string_view detail) const
    -> Unexpected<SignInError> {
  const auto located = error.at(endpoint.label);
  authorization_.report(endpoint.label,
                        located.kind() == SignInErrorKind::cancelled,
                        located.name(), located.http_status(),
                        std::format(": {}{}", located.cause(), detail));
  return unexpected(located);
}

auto QrSignIn::status_failure(std::uint16_t status) noexcept -> SignInError {
  if (net::HttpStatus{status} == net::HttpStatus::too_many_requests)
    return SignInError{SignInErrorKind::rate_limited, status,
                       "the server is rate limiting sign-in"};
  return SignInError{SignInErrorKind::unavailable, status,
                     "the server answered with an unexpected status"};
}

template <class Reply>
auto QrSignIn::reply_of(const net::http::Response& response)
    -> std::optional<Reply> {
  if (response.oversized()) return std::nullopt;
  auto reply = json::ps::read_reply<Reply>(text::c_string(response.body()));
  if (!reply) return std::nullopt;
  return std::move(reply.value());
}

auto QrSignIn::bundle_url(std::string_view page)
    -> Expected<std::string, SignInError> {
  std::optional<std::string> found;
  bool ambiguous = false;
  for (auto at = page.find("<script"); at != std::string_view::npos;
       at = page.find("<script", at + 1)) {
    const auto close = page.find('>', at);
    if (close == std::string_view::npos) break;
    const auto tag = page.substr(at, close - at);
    const auto attribute = tag.find(" src=");
    if (attribute == std::string_view::npos) continue;
    auto value = tag.substr(attribute + 5);
    if (value.empty() || (value.front() != '"' && value.front() != '\''))
      continue;
    const char quote = value.front();
    value.remove_prefix(1);
    const auto end = value.find(quote);
    if (end == std::string_view::npos) continue;
    const auto source = value.substr(0, end);
    if (!source.ends_with(bundle_suffix)) continue;
    auto absolute = resolved(source);
    if (!absolute)
      return unexpected(SignInError{
          SignInErrorKind::layout_changed, 0,
          "the sign-in page loads a bundle outside the account site"});
    if (found && *found != *absolute) ambiguous = true;
    found = std::move(absolute);
  }
  if (!found || ambiguous)
    return unexpected(
        SignInError{SignInErrorKind::layout_changed, 0,
                    "the sign-in page does not load exactly one bundle"});
  return std::move(*found);
}

auto QrSignIn::resolved(std::string_view source) -> std::optional<std::string> {
  const bool plain = std::ranges::all_of(source, [](char value) {
    return text::ascii_alnum(value) || value == '/' || value == '-' ||
           value == '_' || value == '.' || value == ':';
  });
  if (!plain || source.find("..") != std::string_view::npos)
    return std::nullopt;
  std::string absolute;
  if (source.starts_with("https://")) {
    absolute = source;
  } else if (source.starts_with("//")) {
    absolute = std::format("https:{}", source);
  } else if (source.starts_with('/')) {
    absolute = std::format("https://{}{}", account_site, source);
  } else if (source.find(':') == std::string_view::npos) {
    absolute = std::format("{}{}", page_url, source);
  } else {
    return std::nullopt;
  }
  const auto url = net::http::StrictUrl::read(
      absolute, net::http::StrictUrl::Strictness::request_target);
  if (!url || url->scheme() != "https" || url->port() ||
      !text::ieq(url->host(), account_site) || !url->tail().starts_with('/') ||
      url->tail().find(':') != std::string_view::npos)
    return std::nullopt;
  return absolute;
}

auto QrSignIn::registration_of(std::string_view bundle)
    -> Expected<HostedRegistration, SignInError> {
  constexpr std::string_view opening = "passwordFreeSignin:{";
  const auto layout = [](std::string_view cause) {
    return unexpected(SignInError{SignInErrorKind::layout_changed, 0, cause});
  };
  const auto start = bundle.find(opening);
  if (start == std::string_view::npos ||
      bundle.find(opening, start + 1) != std::string_view::npos)
    return layout("the bundle does not hold exactly one registration");
  const auto rest = bundle.substr(start + opening.size());
  const auto close = rest.find('}');
  if (close == std::string_view::npos ||
      rest.substr(0, close).find('{') != std::string_view::npos)
    return layout("the registration is not a flat object");
  const auto object = rest.substr(0, close);
  const auto grant = member(object, "grantType");
  if (!grant || *grant != "client_credentials")
    return layout("the registration does not use client credentials");
  const auto id = member(object, "clientId");
  const auto secret = member(object, "clientSecret");
  if (!id || !secret) return layout("the registration lacks its client");
  auto client_id = unfolded(*id);
  auto client_secret = unfolded(*secret);
  if (!client_id || !client_secret || client_id->empty() ||
      client_id->size() > client_id_limit ||
      !std::ranges::all_of(
          client_id->view(),
          [](char value) { return value > ' ' && value <= '~'; }) ||
      !ClientSecret::admits(client_secret->view()))
    return layout("the registration's client cannot be decoded");
  auto scope = scope_of(object);
  if (!scope) return layout("the registration lacks the ticket scope");
  return HostedRegistration{std::string{client_id->view()},
                            ClientSecret{std::move(*client_secret)},
                            std::move(*scope)};
}

auto QrSignIn::member(std::string_view object, std::string_view key)
    -> std::optional<std::string_view> {
  const auto start = member_start(object, key);
  if (!start) return std::nullopt;
  auto value = object.substr(*start);
  if (!value.starts_with('"')) return std::nullopt;
  value.remove_prefix(1);
  const auto end = value.find('"');
  if (end == std::string_view::npos) return std::nullopt;
  const auto text = value.substr(0, end);
  if (text.find('\\') != std::string_view::npos) return std::nullopt;
  return text;
}

auto QrSignIn::member_start(std::string_view object, std::string_view key)
    -> std::optional<std::size_t> {
  std::optional<std::size_t> found;
  for (auto at = object.find(key); at != std::string_view::npos;
       at = object.find(key, at + 1)) {
    const auto after = at + key.size();
    const bool opens = at == 0 || object[at - 1] == ',';
    if (!opens || after >= object.size() || object[after] != ':') continue;
    if (found) return std::nullopt;
    found = after + 1;
  }
  return found;
}

auto QrSignIn::unfolded(std::string_view encoded)
    -> std::optional<SecretString> {
  auto reversed = SecretBytes::zeroed(encoded.size());
  const auto outer = text::base64::decode_into(
      encoded, reversed.span(), text::base64::Alphabet::standard);
  if (!outer || !reversed.try_resize(outer.value())) return std::nullopt;
  std::ranges::reverse(reversed.span());
  auto decoded = SecretBytes::zeroed(reversed.size());
  const auto inner = text::base64::decode_into(
      reversed.view(), decoded.span(), text::base64::Alphabet::standard);
  if (!inner || !decoded.try_resize(inner.value())) return std::nullopt;
  return SecretString{decoded.bytes()};
}

auto QrSignIn::scope_of(std::string_view object) -> std::optional<std::string> {
  const auto start = member_start(object, "scopes");
  if (!start) return std::nullopt;
  const auto array = object.substr(*start);
  const auto end = array.find(']');
  if (!array.starts_with('[') || end == std::string_view::npos)
    return std::nullopt;
  const auto scopes =
      json::ps::read_reply<std::vector<std::string>>(array.substr(0, end + 1));
  if (!scopes ||
      std::ranges::find(scopes.value(), required_scope) == scopes.value().end())
    return std::nullopt;
  const bool plain = std::ranges::all_of(scopes.value(), [](const auto& scope) {
    return !scope.empty() && std::ranges::all_of(scope, [](char value) {
      return value > ' ' && value <= '~';
    });
  });
  if (!plain) return std::nullopt;
  return scopes.value() | std::views::join_with(' ') |
         std::ranges::to<std::string>();
}

auto QrSignIn::ticket_of(const net::http::Response& response)
    -> Expected<Ticket, SignInError> {
  const auto status = response.status();
  const auto malformed = [status](std::string_view cause) {
    return unexpected(SignInError{SignInErrorKind::malformed, status, cause});
  };
  auto reply = reply_of<TicketReply>(response);
  if (!reply) return malformed("the ticket reply is not a JSON object");
  if (!reply->authentication_ticket ||
      !TicketHandle::admits(reply->authentication_ticket->view()))
    return malformed("the ticket reply lacks a usable handle");
  auto code = UserCode::parse(reply->user_code ? reply->user_code->view()
                                               : std::string_view{});
  if (!code) return malformed("the ticket's user code is not six digits");
  const auto interval = bounded(reply->interval.value_or(0U), interval_limit);
  const auto expires_in =
      bounded(reply->expires_in.value_or(0U), lifetime_limit);
  if (!interval || !expires_in)
    return malformed("the ticket's interval or lifetime is out of range");
  const auto correlation =
      text::trim(response.header(correlation_header).value_or(""));
  if (correlation.empty() || correlation.size() > correlation_limit ||
      !std::ranges::all_of(
          correlation, [](char value) { return value > ' ' && value <= '~'; }))
    return malformed("the ticket reply lacks a correlation id");
  return Ticket{TicketHandle{std::move(*reply->authentication_ticket)},
                std::move(*code), std::string{correlation}, *interval,
                *expires_in};
}

auto QrSignIn::refusal_of(const net::http::Response& response) const
    -> Expected<PollAnswer, SignInError> {
  const auto status = response.status();
  const auto reply = reply_of<RefusalReply>(response);
  if (!reply || !reply->error_code)
    return report(poll_endpoint,
                  SignInError{SignInErrorKind::malformed, status,
                              "the refusal carries no error code"});
  const auto code = *reply->error_code;
  if (code == code_pending &&
      net::HttpStatus{status} == net::HttpStatus::bad_request)
    return PollAnswer{TicketPending{}};
  if (code == code_declined) return PollAnswer{TicketDeclined{}};
  if (code == code_verification) return PollAnswer{TicketNeedsVerification{}};
  if (std::ranges::contains(lapsed_codes, code))
    return PollAnswer{TicketLapsed{}};
  return report(poll_endpoint,
                SignInError{SignInErrorKind::unavailable, status,
                            "the server answered with an unexpected code"},
                std::format(" (error code {})", code));
}

auto QrSignIn::slowed(const net::http::Response& response) const
    -> Expected<PollAnswer, SignInError> {
  const auto header = text::trim(response.header("Retry-After").value_or(""));
  const auto seconds = text::parse_integer<std::uint32_t>(header);
  if (!seconds || *seconds > retry_after_limit)
    return report(poll_endpoint, status_failure(response.status()));
  return PollAnswer{TicketSlowDown{std::chrono::seconds{*seconds}}};
}

auto QrSignIn::bounded(std::uint64_t seconds, std::uint64_t limit) noexcept
    -> std::optional<std::chrono::seconds> {
  if (seconds == 0 || seconds > limit) return std::nullopt;
  return std::chrono::seconds{seconds};
}

}  // namespace libchiaking::psn::auth
