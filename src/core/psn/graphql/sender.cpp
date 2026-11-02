#include "src/core/psn/graphql/sender.hpp"

#include "src/core/net/transport/http/status.hpp"
#include "src/core/net/transport/http/url.hpp"
#include "src/core/psn/auth/app_token_slot.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/hex.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace libchiaking::psn::graphql {

auto Sender::valid(const Operation& operation) noexcept -> bool {
  return !operation.name.empty() && operation.version >= 1 &&
         operation.sha256.size() == sha256_hex_length &&
         std::ranges::all_of(operation.name,
                             [](char value) {
                               return text::ascii_alnum(value) || value == '_';
                             }) &&
         std::ranges::all_of(operation.sha256, text::lower_hex_digit) &&
         operation.endpoint.starts_with("https://") &&
         operation.endpoint.find_first_of("?#") == std::string_view::npos;
}

auto Sender::url_of(const Operation& operation, const Variables& variables)
    -> net::http::Query {
  const auto extensions = json::ps::write<json::ps::body_options_with_nulls>(
      Extensions{PersistedQuery{operation.version, operation.sha256}});
  net::http::Query url{operation.endpoint};
  url.add("operationName", operation.name, net::http::Escape::form)
      .add("variables", variables.text(), net::http::Escape::form)
      .add("extensions",
           extensions ? extensions.value().view() : std::string_view{},
           net::http::Escape::form);
  return url;
}

struct Sender::Strictness : glz::opts {
  bool validate_skipped = true;
  bool validate_trailing_whitespace = true;
};

struct Sender::Envelope {
  static constexpr Strictness options = [] {
    Strictness chosen{};
    chosen.error_on_unknown_keys = false;
    return chosen;
  }();

  struct Extensions {
    json::ps::Tolerant<std::string> code;
  };
  struct Entry {
    json::ps::Tolerant<std::string> message;
    json::ps::Tolerant<Extensions> extensions;
  };

  json::ps::Tolerant<std::vector<json::ps::Tolerant<Entry>>> errors;
  std::optional<glz::raw_json> data;
};

auto Sender::entries_of(Envelope& envelope)
    -> std::vector<GraphqlError::Entry> {
  std::vector<GraphqlError::Entry> entries;
  if (!envelope.errors) return entries;
  for (auto& error : *envelope.errors) {
    GraphqlError::Entry entry;
    if (error) {
      entry.message = std::move(error->message).value_or(std::string{});
      if (error->extensions)
        entry.code = std::move(error->extensions->code).value_or(std::string{});
    }
    entries.push_back(std::move(entry));
  }
  return entries;
}

auto Sender::classify(std::string_view operation,
                      const net::http::Response& response)
    -> Expected<Result, GraphqlError> {
  const auto status = response.status();
  const auto attempts = response.attempts();
  Envelope envelope{};
  const bool readable =
      !response.oversized() &&
      !json::read_into<Envelope::options>(envelope, response.body());
  std::vector<GraphqlError::Entry> entries;
  if (readable) entries = entries_of(envelope);
  if (!net::HttpStatus{status}.is_success()) {
    const auto kind =
        net::HttpStatus{status}.is_auth_refused()   ? Kind::auth_refused
        : net::HttpStatus{status}.is_server_error() ? Kind::server_error
                                                    : Kind::rejected;
    return unexpected(GraphqlError::replied(operation, kind, status, attempts,
                                            std::move(entries)));
  }
  if (!readable)
    return unexpected(
        GraphqlError::replied(operation, Kind::invalid_json, status, attempts));
  if (envelope.data && !entries.empty())
    return unexpected(GraphqlError::replied(operation, Kind::partial, status,
                                            attempts, std::move(entries)));
  if (envelope.data)
    return Result{std::move(envelope.data->str), status, attempts};
  const auto kind = entries.empty() ? Kind::malformed : Kind::errors;
  return unexpected(GraphqlError::replied(operation, kind, status, attempts,
                                          std::move(entries)));
}

void Sender::log_failure(net::http::Client& client, std::string_view label,
                         const GraphqlError& error) {
  auto line = std::format("[PSN] graphql {} {}", label, error.name());
  if (error.kind() == Kind::missing_credential)
    line += std::format(" credential {}", error.credential_name());
  if (error.http_status() != 0)
    line += std::format(" HTTP {}", error.http_status());
  if (!error.code().empty()) line += std::format(" {}", error.code());
  line += std::format(" after {} attempts", error.attempts());
  client.log(platform::LogSeverity::warning, line);
}

auto Sender::refused(const Operation& operation,
                     const net::http::Endpoint& endpoint, GraphqlError error)
    -> Expected<Result, GraphqlError> {
  const auto label = endpoint.label.empty() ? operation.name : endpoint.label;
  log_failure(client_, label, error);
  return unexpected(std::move(error));
}

auto Sender::send_public(const Operation& operation,
                         const net::http::Endpoint& endpoint,
                         const Variables& variables, const Call& call)
    -> Expected<Result, GraphqlError> {
  if (operation.auth != Auth::none)
    return refused(
        operation, endpoint,
        GraphqlError::missing_credential(operation.name, std::nullopt));
  return send(operation, endpoint, variables, call, nullptr);
}

auto Sender::send_authenticated(const Operation& operation,
                                const net::http::Endpoint& endpoint,
                                const Variables& variables, const Call& call,
                                auth::AppTokenSlot& credentials)
    -> Expected<Result, GraphqlError> {
  if (operation.auth != Auth::required)
    return refused(operation, endpoint,
                   GraphqlError::unsent(operation.name, Kind::invalid_request));
  return send(operation, endpoint, variables, call, &credentials);
}

auto Sender::send(const Operation& operation,
                  const net::http::Endpoint& endpoint,
                  const Variables& variables, const Call& call,
                  auth::AppTokenSlot* credentials)
    -> Expected<Result, GraphqlError> {
  const auto label = endpoint.label.empty() ? operation.name : endpoint.label;
  std::optional<auth::AppToken> token;
  std::optional<auth::AuthError> refusal;
  const bool credentialed = credentials != nullptr;
  if (credentialed) {
    if (auto leased = credentials->lease())
      token.emplace(std::move(leased.value()));
    else
      refusal = leased.error();
  }
  const auto fail = [&](GraphqlError error) -> Expected<Result, GraphqlError> {
    log_failure(client_, label, error);
    return unexpected(std::move(error));
  };
  if (credentialed && !token)
    return fail(GraphqlError::missing_credential(operation.name, refusal));
  if (client_.stop_requested())
    return fail(GraphqlError::unsent(operation.name, Kind::cancelled));
  if (!valid(operation) || !variables.names_an_object())
    return fail(GraphqlError::unsent(operation.name, Kind::invalid_request));

  net::http::Request request{endpoint, url_of(operation, variables)};
  if (!call.user_agent.empty()) request.header("User-Agent", call.user_agent);
  if (token) {
    request.header("X-Psn-App-Ver",
                   std::format("{}/{}", app_name, app_version));
    request.header("apollographql-client-name", app_name);
    request.header("apollographql-client-version", app_version);
    request.header("disable_query_whitelist", "false");
  }
  if (!call.locale.empty())
    request.header("x-psn-store-locale-override", call.locale);
  request.header("Content-Type", "application/json");
  request.header("Accept", "application/json");
  if (token) request.authorize(*token);

  auto sent = client_.send(std::move(request));
  auto outcome = sent
                     ? classify(operation.name, sent.value())
                     : Expected<Result, GraphqlError>{unexpected(
                           GraphqlError::failed(operation.name, sent.error()))};
  const auto http_status = sent ? sent.value().status() : std::uint16_t{};
  const auto attempts =
      outcome ? outcome.value().attempts : outcome.error().attempts();
  if (stats_ != nullptr)
    stats_->record(operation.name, http_status, attempts,
                   outcome ? nullptr : &outcome.error());
  if (token && http_status != 0)
    credentials->record_use(*token, http_status, std::max(attempts - 1, 0));
  if (!outcome) log_failure(client_, label, outcome.error());
  return outcome;
}

}  // namespace libchiaking::psn::graphql
