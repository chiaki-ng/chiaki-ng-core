/// @file
/// @brief Declares SessionInternals, the psn context's window onto a Session's
/// credential caches, and the diagnostic snapshot of them.
#pragma once

#include "src/core/psn/auth/app_token_slot.hpp"
#include "src/core/psn/auth/token_cache.hpp"
#include "src/core/psn/graphql/check.hpp"
#include "src/core/psn/graphql/stats.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <glaze/json.hpp>

#include "src/core/psn/session.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::psn {

/// @brief A Session's credential statistics at one moment, secret-free. Plain
/// data.
struct SessionSnapshot {
  /// @brief When the session was made, in wall-clock milliseconds since the
  /// Unix epoch.
  std::int64_t created_at_ms{};
  /// @brief NPSSO exchanges made by the token cache.
  std::uint64_t exchanges{};
  /// @brief Token uses reported, the app token's included.
  std::uint64_t uses{};
  /// @brief Token refusals (HTTP 401 or 403) reported, the app token's
  /// included.
  std::uint64_t refusals{};
  /// @brief The token cache's tokens in grant order, then the app token when
  /// it was ever supplied or used.
  std::vector<auth::TokenStats> tokens;
  /// @brief One entry per GraphQL operation sent, by name.
  std::vector<graphql::Stats::Entry> graphql;
};

/// @brief Gives code inside the psn context, and the users context's user
/// core, the parts of a Session it must not expose publicly.
///
/// A short-lived view holding one reference; it must not outlive the Session.
/// As thread-safe as the parts it hands out, which lock themselves.
class SessionInternals {
 public:
  /// @brief Views a session.
  ///
  /// @param session The session; must outlive this object.
  explicit SessionInternals(Session& session) noexcept
      : state_(*session.state_) {}

  /// @brief Reaches the commerce token cache.
  ///
  /// @return The cache; lives as long as the session.
  auto tokens() const noexcept -> auth::TokenCache&;
  /// @brief Reaches the app token slot.
  ///
  /// @return The slot; lives as long as the session.
  auto app_token() const noexcept -> auth::AppTokenSlot&;
  /// @brief Reaches the GraphQL statistics.
  ///
  /// @return The statistics; live as long as the session.
  auto graphql() const noexcept -> graphql::Stats&;
  /// @brief Gathers the statistics.
  ///
  /// @return The snapshot.
  auto snapshot() const -> SessionSnapshot;
  /// @brief Writes the snapshot as the diagnostics JSON document, camel-cased
  /// with nulls kept: `createdAtMs`, `totals`, `tokens`, `subscription`,
  /// `graphql`.
  ///
  /// @param subscription A JSON value embedded verbatim as `subscription`,
  ///     such as the user's membership; must be valid JSON.
  /// @return The document; nothing when it could not be written, such as
  ///     for a subscription that is not valid UTF-8.
  [[nodiscard]] auto snapshot_json(std::string_view subscription = "null") const
      -> std::optional<std::string>;

 private:
  /// @brief One token as the document writes it.
  struct TokenDocument {
    /// @brief Field names are written in camel case.
    using keys = json::ps::CamelCaseKeys;
    /// @brief The token's diagnostic name.
    std::string_view label;
    /// @brief The client it was issued to.
    std::string_view client_id;
    /// @brief Its scopes.
    std::string_view scope;
    /// @brief When obtained, wall-clock milliseconds; null when never.
    std::optional<std::int64_t> obtained_at_ms;
    /// @brief When it expires, wall-clock milliseconds; null when unknown.
    std::optional<std::int64_t> expires_at_ms;
    /// @brief `expires_in`, `app` or `default`.
    std::string_view expiry_source;
    /// @brief `none`, `valid`, `expired` or `invalidated`.
    std::string_view state;
    /// @brief NPSSO exchanges for it.
    std::uint64_t exchanges{};
    /// @brief Uses reported.
    std::uint64_t uses{};
    /// @brief Refusals reported.
    std::uint64_t refusals{};
    /// @brief Renewals after a refusal.
    std::uint64_t retries{};
    /// @brief HTTP status of the last exchange.
    std::uint64_t last_exchange_status{};
    /// @brief HTTP status of the last use.
    std::uint64_t last_use_status{};
    /// @brief When last used, wall-clock milliseconds; null when never.
    std::optional<std::int64_t> last_used_at_ms;
  };
  /// @brief One GraphQL operation as the document writes it.
  struct GraphqlDocument {
    /// @brief Field names are written in camel case.
    using keys = json::ps::CamelCaseKeys;
    /// @brief The operation name.
    std::string_view operation;
    /// @brief HTTP attempts made for it.
    std::uint64_t requests{};
    /// @brief Calls that failed.
    std::uint64_t failures{};
    /// @brief `ok`, or the last failure's GraphqlError name.
    std::string_view last_status;
    /// @brief HTTP status of the last call.
    std::uint64_t last_http_status{};
    /// @brief The last failure's PS error code; empty after a success.
    std::string_view last_code;
    /// @brief When last called, wall-clock milliseconds.
    std::optional<std::int64_t> last_at_ms;
  };
  /// @brief The session-wide counts.
  struct Totals {
    /// @brief NPSSO exchanges.
    std::uint64_t exchanges{};
    /// @brief Token uses.
    std::uint64_t uses{};
    /// @brief Token refusals.
    std::uint64_t refusals{};
  };
  /// @brief The whole document.
  struct SnapshotDocument {
    /// @brief Field names are written in camel case.
    using keys = json::ps::CamelCaseKeys;
    /// @brief When the session was made, wall-clock milliseconds.
    std::int64_t created_at_ms{};
    /// @brief The session-wide counts.
    Totals totals;
    /// @brief The tokens.
    std::vector<TokenDocument> tokens;
    /// @brief The caller's JSON value, verbatim.
    glz::raw_json subscription;
    /// @brief The GraphQL operations.
    std::vector<GraphqlDocument> graphql;
  };

  /// @brief Names an expiry source for the document.
  ///
  /// @param source The source.
  /// @return `expires_in`, `app`, or `default` for the fallback.
  static auto expiry_name(auth::ExpirySource source) noexcept
      -> std::string_view;
  /// @brief Names a token state for the document.
  ///
  /// @param state The state.
  /// @return The enumerator's name.
  static auto state_name(auth::TokenState state) noexcept -> std::string_view;
  /// @brief Converts a token's statistics.
  ///
  /// @param item The statistics; must outlive the result, which views its
  ///     text.
  /// @return The document entry.
  static auto token_document(const auth::TokenStats& item) -> TokenDocument;
  /// @brief Converts an operation's statistics.
  ///
  /// @param item The statistics; must outlive the result, which views its
  ///     text.
  /// @return The document entry.
  static auto graphql_document(const graphql::Stats::Entry& item)
      -> GraphqlDocument;

  /// @brief The viewed session's state.
  Session::State& state_;
};

}  // namespace libchiaking::psn
