/// @file
/// @brief Declares Sender, which sends one PSN GraphQL persisted query and
/// classifies the reply, and the Variables and Result it takes and gives.
///
/// Every operation is a GET to the gateway with `operationName`, `variables`
/// (JSON) and `extensions` (`{"persistedQuery":{"version","sha256Hash"}}`) in
/// the query. Anonymous calls look like the store web site; authenticated ones
/// add the app token and the Apollo client headers.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/graphql/error.hpp"
#include "src/core/psn/graphql/operation.hpp"
#include "src/core/psn/graphql/stats.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <libchiaking/detail/expected.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::psn::auth {
/// @brief Where a session keeps the app token.
class AppTokenSlot;
}  // namespace libchiaking::psn::auth

namespace libchiaking::psn::graphql {

/// @brief A successful GraphQL reply. Plain data.
struct Result {
  /// @brief The reply's `data`, as raw JSON.
  std::string data;
  /// @brief The reply's status, a 2xx.
  std::uint16_t http_status{};
  /// @brief HTTP attempts made.
  int attempts{};

  /// @brief Decodes the data leniently.
  ///
  /// @tparam Reply The shape the data is decoded into.
  /// @return The decoded data, or an empty Reply when it does not decode.
  template <class Reply>
  auto reply() const -> Reply {
    return json::ps::read_reply<Reply>(data).value_or(Reply{});
  }
};

/// @brief An operation's variables as the JSON text sent; a valid set is an
/// object. An immutable value.
class Variables {
 public:
  /// @brief Writes variables, nulls kept.
  ///
  /// @tparam Fields A glaze-writable struct.
  /// @param fields The variables.
  /// @return The variables; empty text, which Sender refuses, when they could
  ///     not be written.
  template <class Fields>
  static auto of(const Fields& fields) -> Variables {
    auto written = json::ps::write<json::ps::body_options_with_nulls>(fields);
    return Variables{written ? std::string{written.value().view()}
                             : std::string{}};
  }
  /// @brief Makes empty variables.
  ///
  /// @return `{}`.
  static auto none() -> Variables { return Variables{"{}"}; }

  /// @brief Reads the JSON text.
  ///
  /// @return The text; valid while this object lives.
  auto text() const noexcept -> std::string_view { return text_; }
  /// @brief Tells whether the text is a JSON object.
  ///
  /// @return True when it starts with `{`.
  auto names_an_object() const noexcept -> bool {
    return text_.starts_with('{');
  }

 private:
  /// @brief Wraps written text.
  ///
  /// @param text The JSON.
  explicit Variables(std::string text) noexcept : text_(std::move(text)) {}

  /// @brief The JSON text.
  std::string text_;
};

/// @brief How one call presents itself. Plain data; the views must outlive the
/// call.
struct Call {
  /// @brief The `User-Agent`; none when empty.
  std::string_view user_agent;
  /// @brief The `x-psn-store-locale-override`, such as `en-GB`; none when
  /// empty.
  std::string_view locale;
};

/// @brief Sends GraphQL operations through an HTTP client, records each sent
/// call in the session's statistics, and logs failures as `[PSN] graphql`.
///
/// Holds references to the client and the statistics, which must outlive it.
/// Each call blocks for one request, retries included as the endpoint
/// allows. As thread-safe as the client.
class Sender {
 public:
  /// @brief Prepares the calls.
  ///
  /// @param client The client the requests go through.
  /// @param stats Where sent calls are recorded; may be null.
  explicit Sender(net::http::Client& client, Stats* stats = nullptr) noexcept
      : client_(client), stats_(stats) {}

  /// @brief Sends an anonymous operation.
  ///
  /// @param operation The operation; must not need the app token.
  /// @param endpoint The endpoint the request is recorded under, with its
  ///     size limit and retries.
  /// @param variables The variables.
  /// @param call The user agent and locale.
  /// @return The reply's data.
  /// @retval GraphqlError::Kind::missing_credential The operation needs the
  ///     app token.
  /// @retval GraphqlError The failures of send().
  [[nodiscard]] auto send_public(const Operation& operation,
                                 const net::http::Endpoint& endpoint,
                                 const Variables& variables, const Call& call)
      -> Expected<Result, GraphqlError>;
  /// @brief Sends an operation with the app token.
  ///
  /// The token is leased from the slot and, once a reply arrives, its use is
  /// recorded there, so a 401 or 403 invalidates it.
  ///
  /// @param operation The operation; must need the app token.
  /// @param endpoint The endpoint the request is recorded under.
  /// @param variables The variables.
  /// @param call The user agent and locale.
  /// @param credentials The session's app token slot.
  /// @return The reply's data.
  /// @retval GraphqlError::Kind::invalid_request The operation is anonymous.
  /// @retval GraphqlError::Kind::missing_credential The token could not be
  ///     leased.
  /// @retval GraphqlError The failures of send().
  [[nodiscard]] auto send_authenticated(const Operation& operation,
                                        const net::http::Endpoint& endpoint,
                                        const Variables& variables,
                                        const Call& call,
                                        auth::AppTokenSlot& credentials)
      -> Expected<Result, GraphqlError>;

 private:
  /// @brief A GraphQL failure's kind.
  using Kind = GraphqlError::Kind;
  /// @brief The length of a query hash in hex digits.
  static constexpr std::size_t sha256_hex_length = 64;
  /// @brief The Apollo client name authenticated calls send.
  static constexpr std::string_view app_name = "PlayStationApp";
  /// @brief The app version authenticated calls claim.
  static constexpr std::string_view app_version = "26.9.2";

  /// @brief Checks an operation before it is sent.
  ///
  /// @param operation The operation.
  /// @return True when the name is non-empty letters, digits and `_`, the
  ///     version is at least 1, the hash is 64 lowercase hex digits and the
  ///     endpoint is HTTPS with no query or fragment.
  static auto valid(const Operation& operation) noexcept -> bool;
  /// @brief The `persistedQuery` extension.
  struct PersistedQuery {
    /// @brief Field names are written in camel case.
    using keys = json::ps::CamelCaseKeys;
    /// @brief The protocol version.
    std::int64_t version{};
    /// @brief The query's hash.
    std::string_view sha256_hash;
  };
  /// @brief The `extensions` sent.
  struct Extensions {
    /// @brief Field names are written in camel case.
    using keys = json::ps::CamelCaseKeys;
    /// @brief The persisted query.
    PersistedQuery persisted_query;
  };

  /// @brief Builds the request URL.
  ///
  /// @param operation The operation.
  /// @param variables The variables.
  /// @return The endpoint with `operationName`, `variables` and `extensions`,
  ///     form-escaped.
  static auto url_of(const Operation& operation, const Variables& variables)
      -> net::http::Query;
  /// @brief The JSON options the envelope is read with.
  struct Strictness;
  /// @brief The reply's `data` (raw) and `errors`.
  struct Envelope;

  /// @brief Takes the error entries out of an envelope.
  ///
  /// @param envelope The envelope; its errors are moved from.
  /// @return One entry per `errors` element, empty fields for unreadable
  ///     ones.
  static auto entries_of(Envelope& envelope)
      -> std::vector<GraphqlError::Entry>;

  /// @brief Judges a reply.
  ///
  /// @param operation The operation's name.
  /// @param response The reply.
  /// @return The data, for a 2xx reply with `data` and no `errors`.
  /// @retval GraphqlError::Kind::auth_refused HTTP 401 or 403.
  /// @retval GraphqlError::Kind::server_error A 5xx.
  /// @retval GraphqlError::Kind::rejected Another non-2xx.
  /// @retval GraphqlError::Kind::invalid_json An unreadable or oversized 2xx
  ///     body.
  /// @retval GraphqlError::Kind::partial Both `data` and `errors`.
  /// @retval GraphqlError::Kind::errors `errors` without `data`.
  /// @retval GraphqlError::Kind::malformed Neither.
  [[nodiscard]] static auto classify(std::string_view operation,
                                     const net::http::Response& response)
      -> Expected<Result, GraphqlError>;
  /// @brief Sends an operation, records it and logs a failure.
  ///
  /// @param operation The operation.
  /// @param endpoint The endpoint.
  /// @param variables The variables.
  /// @param call The user agent and locale.
  /// @param credentials The app token slot, or null for an anonymous call.
  /// @return The reply's data.
  /// @retval GraphqlError::Kind::missing_credential No token could be leased.
  /// @retval GraphqlError::Kind::cancelled The stop token had fired; nothing
  ///     sent.
  /// @retval GraphqlError::Kind::invalid_request The operation is not valid()
  ///     or the variables are not an object; nothing sent.
  /// @retval GraphqlError The transport failure, or classify()'s verdict.
  [[nodiscard]] auto send(const Operation& operation,
                          const net::http::Endpoint& endpoint,
                          const Variables& variables, const Call& call,
                          auth::AppTokenSlot* credentials)
      -> Expected<Result, GraphqlError>;
  /// @brief Logs and returns a failure found before sending.
  ///
  /// @param operation The operation.
  /// @param endpoint The endpoint, whose label is logged.
  /// @param error The failure.
  /// @return The failure.
  [[nodiscard]] auto refused(const Operation& operation,
                             const net::http::Endpoint& endpoint,
                             GraphqlError error)
      -> Expected<Result, GraphqlError>;
  /// @brief Logs a failure at warning level: label, kind, credential state,
  /// HTTP status, GraphQL code and attempts.
  ///
  /// @param client The client whose log is used.
  /// @param label The endpoint label or operation name.
  /// @param error The failure.
  static void log_failure(net::http::Client& client, std::string_view label,
                          const GraphqlError& error);

  /// @brief The client the requests go through.
  net::http::Client& client_;
  /// @brief Where sent calls are recorded; may be null.
  Stats* stats_;
};

}  // namespace libchiaking::psn::graphql
