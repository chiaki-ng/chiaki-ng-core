/// @file
/// @brief Declares GraphqlError, the failure of one PSN GraphQL call, which
/// keeps the operation, the HTTP outcome and the GraphQL `errors` entries.
#pragma once

#include "src/core/net/transport/http/failure.hpp"
#include "src/core/psn/auth/auth_error.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::psn::graphql {

/// @brief Why a GraphQL call failed. An immutable value once made; the
/// operation name it views is a compile-time constant.
class GraphqlError {
 public:
  /// @brief Where the call failed.
  enum class Kind : std::uint8_t {
    /// @brief The stop token fired, before or during the request.
    cancelled,
    /// @brief The request did not complete.
    transport,
    /// @brief The gateway refused the app token (HTTP 401 or 403).
    auth_refused,
    /// @brief Another non-2xx, non-5xx status.
    rejected,
    /// @brief A 5xx status.
    server_error,
    /// @brief A 2xx reply with `errors` and no `data`.
    errors,
    /// @brief A 2xx reply with both `data` and `errors`.
    partial,
    /// @brief A 2xx reply that is not the GraphQL envelope, or was
    /// oversized.
    invalid_json,
    /// @brief A 2xx reply with neither `data` nor `errors`, or whose `data`
    /// lacks what the operation reads.
    malformed,
    /// @brief The operation answered that the thing asked about does not
    /// exist.
    not_found,
    /// @brief The operation or variables are malformed, the wrong sender
    /// was used for an operation that needs the app token, or the client
    /// refused the request; nothing was sent.
    invalid_request,
    /// @brief No usable app token, or an anonymous sender was asked for an
    /// operation that needs one; nothing was sent.
    missing_credential,
  };

  /// @brief One entry of the reply's `errors`. Plain data.
  struct Entry {
    /// @brief The `message`; empty when absent.
    std::string message;
    /// @brief The `extensions.code`; empty when absent.
    std::string code;
  };

  /// @brief Wraps a request that did not complete.
  ///
  /// @param operation The operation's name; must be a constant.
  /// @param failure The client's failure.
  /// @return cancelled, invalid_request when the client refused the request,
  ///     or transport; with the attempts made.
  static auto failed(std::string_view operation,
                     const net::http::Failure& failure) -> GraphqlError;
  /// @brief Wraps a reply that failed.
  ///
  /// @param operation The operation's name; must be a constant.
  /// @param kind Where it failed.
  /// @param http_status The reply's status.
  /// @param attempts How many attempts were made.
  /// @param errors The reply's `errors`.
  /// @return The failure.
  static auto replied(std::string_view operation, Kind kind,
                      std::uint16_t http_status, int attempts,
                      std::vector<Entry> errors = {}) -> GraphqlError;
  /// @brief Wraps a call refused before sending.
  ///
  /// @param operation The operation's name; must be a constant.
  /// @param kind Why.
  /// @return The failure, with no status and no attempts.
  static auto unsent(std::string_view operation, Kind kind) -> GraphqlError;
  /// @brief Wraps a call that had no usable app token.
  ///
  /// @param operation The operation's name; must be a constant.
  /// @param refusal Why the token could not be leased; nothing when the
  ///     caller had no token to offer.
  /// @return The failure.
  static auto missing_credential(std::string_view operation,
                                 std::optional<auth::AuthError> refusal)
      -> GraphqlError;

  /// @brief Reads where the call failed.
  ///
  /// @return The kind.
  auto kind() const noexcept -> Kind { return kind_; }
  /// @brief Reads the operation.
  ///
  /// @return Its name.
  auto operation() const noexcept -> std::string_view { return operation_; }
  /// @brief Reads the reply's status.
  ///
  /// @return It, or 0 when there was no reply.
  auto http_status() const noexcept -> std::uint16_t { return http_status_; }
  /// @brief Reads how many HTTP attempts were made.
  ///
  /// @return The count; 0 when nothing was sent.
  auto attempts() const noexcept -> int { return attempts_; }
  /// @brief Reads the transport failure.
  ///
  /// @return It, or nothing for other kinds.
  auto failure() const noexcept -> const std::optional<net::http::Failure>& {
    return failure_;
  }
  /// @brief Reads why the app token could not be leased.
  ///
  /// @return It, or nothing.
  auto refusal() const noexcept -> const std::optional<auth::AuthError>& {
    return refusal_;
  }
  /// @brief Reads the reply's `errors`.
  ///
  /// @return The entries; valid while this object lives.
  auto errors() const noexcept -> std::span<const Entry> { return errors_; }
  /// @brief Reads the first error's code.
  ///
  /// @return It, or empty.
  auto code() const noexcept -> std::string_view;
  /// @brief Names the kind for logs and statistics.
  ///
  /// @return `cancelled`, `auth_refused`, `rejected`, `graphql_error`,
  ///     `partial`, `malformed` (also for invalid_json), `not_found`,
  ///     `invalid_request`, `no_credential`, or `unavailable` for transport
  ///     and server errors.
  auto name() const noexcept -> std::string_view;
  /// @brief Names why the app token was unusable, for logs.
  ///
  /// @return `refused`, `expired`, or `missing`.
  auto credential_name() const noexcept -> std::string_view;

 private:
  /// @brief Makes a failure with nothing else known.
  ///
  /// @param kind Where it failed.
  /// @param operation The operation's name.
  GraphqlError(Kind kind, std::string_view operation) noexcept;

  /// @brief Where it failed.
  Kind kind_;
  /// @brief The operation's name, a constant.
  std::string_view operation_;
  /// @brief The reply's status; 0 without a reply.
  std::uint16_t http_status_{};
  /// @brief HTTP attempts made.
  int attempts_{};
  /// @brief The transport failure.
  std::optional<net::http::Failure> failure_;
  /// @brief Why the app token could not be leased.
  std::optional<auth::AuthError> refusal_;
  /// @brief The reply's `errors`.
  std::vector<Entry> errors_;
};

}  // namespace libchiaking::psn::graphql
