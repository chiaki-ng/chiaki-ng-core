#include "src/core/psn/session_internals.hpp"

#include "src/core/utils/text/ascii.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace libchiaking::psn {

auto Session::valid_app_token(std::string_view token) noexcept -> bool {
  return auth::AppToken::admits(token);
}

auto SessionInternals::expiry_name(auth::ExpirySource source) noexcept
    -> std::string_view {
  switch (source) {
    case auth::ExpirySource::expires_in:
      return "expires_in";
    case auth::ExpirySource::app:
      return "app";
    case auth::ExpirySource::fallback:
      break;
  }
  return "default";
}

auto SessionInternals::state_name(auth::TokenState state) noexcept
    -> std::string_view {
  switch (state) {
    case auth::TokenState::valid:
      return "valid";
    case auth::TokenState::expired:
      return "expired";
    case auth::TokenState::invalidated:
      return "invalidated";
    case auth::TokenState::none:
      break;
  }
  return "none";
}

auto SessionInternals::token_document(const auth::TokenStats& item)
    -> TokenDocument {
  return TokenDocument{.label = item.label,
                       .client_id = item.client_id,
                       .scope = item.scope,
                       .obtained_at_ms = item.obtained_at_ms,
                       .expires_at_ms = item.expires_at_ms,
                       .expiry_source = expiry_name(item.expiry_source),
                       .state = state_name(item.state),
                       .exchanges = item.usage.exchanges,
                       .uses = item.usage.uses,
                       .refusals = item.usage.refusals,
                       .retries = item.usage.retries,
                       .last_exchange_status = item.usage.last_exchange_status,
                       .last_use_status = item.usage.last_use_status,
                       .last_used_at_ms = item.last_used_at_ms};
}

auto SessionInternals::graphql_document(const graphql::Stats::Entry& item)
    -> GraphqlDocument {
  return GraphqlDocument{.operation = item.operation,
                         .requests = item.requests,
                         .failures = item.failures,
                         .last_status = item.last_status,
                         .last_http_status = item.last_http_status,
                         .last_code = item.last_code,
                         .last_at_ms = item.last_at_ms};
}

class Session::State {
 public:
  State(auth::Npsso npsso, platform::ThreadingProvider& threading,
        const platform::ClockProvider& clock,
        const platform::ClockProvider& wall)
      : tokens(std::move(npsso), threading, clock, wall),
        app_token(threading, clock, wall),
        graphql(threading, wall),
        created_at_ms(auth::Timeline{clock, wall}.wall_ms()) {}

  auth::TokenCache tokens;
  auth::AppTokenSlot app_token;
  graphql::Stats graphql;

  std::int64_t created_at_ms;
};

auto Session::open(std::string_view npsso,
                   platform::ThreadingProvider& threading,
                   const platform::ClockProvider& clock,
                   const platform::ClockProvider& wall)
    -> Expected<std::shared_ptr<Session>, Error> {
  if (!valid_npsso(npsso)) return unexpected(Error::invalid_npsso);
  return std::make_shared<Session>(auth::Npsso{npsso}, threading, clock, wall);
}

auto Session::valid_npsso(std::string_view npsso) noexcept -> bool {
  return auth::Npsso::admits(npsso);
}

Session::Session(auth::Npsso npsso, platform::ThreadingProvider& threading,
                 const platform::ClockProvider& clock,
                 const platform::ClockProvider& wall)
    : state_(
          std::make_unique<State>(std::move(npsso), threading, clock, wall)) {}

Session::~Session() = default;

auto Session::npsso() const noexcept -> const auth::Npsso& {
  return state_->tokens.npsso();
}
auto Session::matches(std::string_view npsso) const noexcept -> bool {
  return state_->tokens.matches(npsso);
}
auto SessionInternals::tokens() const noexcept -> auth::TokenCache& {
  return state_.tokens;
}
auto SessionInternals::app_token() const noexcept -> auth::AppTokenSlot& {
  return state_.app_token;
}
auto SessionInternals::graphql() const noexcept -> graphql::Stats& {
  return state_.graphql;
}

auto Session::supply_app_token(std::string_view token,
                               std::chrono::system_clock::time_point expires_at)
    -> Expected<void, Error> {
  if (!valid_app_token(token)) return unexpected(Error::invalid_app_token);
  if (expires_at < std::chrono::system_clock::time_point{})
    return unexpected(Error::invalid_expiry);
  state_->app_token.supply(auth::AppToken{token}, expires_at);
  return {};
}

void Session::clear_app_token() noexcept { state_->app_token.clear(); }

void Session::clear() {
  state_->tokens.clear();
  state_->app_token.reset();
  state_->graphql.clear();
}

auto SessionInternals::snapshot() const -> SessionSnapshot {
  auto cached = state_.tokens.snapshot();
  auto app = state_.app_token.stats();
  const auto app_usage = app ? app->usage : auth::TokenUsage{};
  SessionSnapshot view;
  view.created_at_ms = state_.created_at_ms;
  view.exchanges = cached.exchanges;
  view.uses = cached.uses + app_usage.uses;
  view.refusals = cached.refusals + app_usage.refusals;
  view.tokens = std::move(cached.tokens);
  if (app) view.tokens.push_back(std::move(*app));
  view.graphql = state_.graphql.entries();
  return view;
}

auto SessionInternals::snapshot_json(std::string_view subscription) const
    -> std::optional<std::string> {
  const auto view = snapshot();
  SnapshotDocument document{
      .created_at_ms = view.created_at_ms,
      .totals = {view.exchanges, view.uses, view.refusals},
      .tokens = {},
      .subscription = glz::raw_json{std::string{subscription}},
      .graphql = {}};
  std::ranges::transform(view.tokens, std::back_inserter(document.tokens),
                         token_document);
  std::ranges::transform(view.graphql, std::back_inserter(document.graphql),
                         graphql_document);
  const auto written =
      json::ps::write<json::ps::body_options_with_nulls>(document);
  if (!written) return std::nullopt;
  return std::string{written.value().view()};
}

auto Session::snapshot_json() -> std::optional<std::string> {
  return SessionInternals{*this}.snapshot_json();
}

}  // namespace libchiaking::psn
