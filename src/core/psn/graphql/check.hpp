/// @file
/// @brief Declares CheckError, a GraphQL failure as the users context reports
/// it: a CheckStatus that says what the caller can do about it, with the
/// underlying GraphqlError kept for diagnostics.
#pragma once

#include "src/core/psn/graphql/error.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace libchiaking::psn::graphql {

/// @brief What a failed GraphQL check means to its caller.
enum class CheckStatus : std::uint8_t {
  /// @brief No failure; used only to name success in summaries.
  ok,
  /// @brief The user is off PSN, so nothing was asked.
  no_session,
  /// @brief No app token is held, or one could not be leased for another
  /// reason.
  no_app_token,
  /// @brief The app token expires within its margin.
  app_token_expired,
  /// @brief The gateway or an earlier call refused the app token.
  auth_refused,
  /// @brief The request did not complete or the gateway answered 5xx.
  unavailable,
  /// @brief The gateway answered with GraphQL errors or another error status.
  rejected,
  /// @brief The reply could not be read, or was partial.
  malformed,
  /// @brief The thing asked about does not exist.
  not_found,
  /// @brief The request could not be built or was refused before sending.
  invalid_request,
  /// @brief The stop token fired.
  cancelled,
};

/// @brief A failed GraphQL check. An immutable value; safe to copy and share.
///
/// Satisfies ResultCache's error requirements (cancelled(), is_cancelled()).
class CheckError {
 public:
  /// @brief Makes a failure with no GraphQL cause.
  ///
  /// @param status What it means.
  /// @param operation The operation's name, a constant; may be empty.
  explicit CheckError(CheckStatus status,
                      std::string_view operation = {}) noexcept;
  /// @brief Classifies a GraphQL failure.
  ///
  /// @param cause The failure; kept.
  explicit CheckError(GraphqlError cause);

  /// @brief Makes the failure of a cancelled check.
  ///
  /// @return A cancelled failure.
  static auto cancelled() noexcept -> CheckError {
    return CheckError{CheckStatus::cancelled};
  }

  /// @brief Tells whether the check was cancelled.
  ///
  /// @return True for cancelled.
  auto is_cancelled() const noexcept -> bool {
    return status_ == CheckStatus::cancelled;
  }
  /// @brief Reads what the failure means.
  ///
  /// @return The status.
  auto status() const noexcept -> CheckStatus { return status_; }
  /// @brief Reads the operation.
  ///
  /// @return Its name; empty when unknown.
  auto operation() const noexcept -> std::string_view { return operation_; }
  /// @brief Reads the GraphQL failure.
  ///
  /// @return It, or nothing.
  auto cause() const noexcept -> const std::optional<GraphqlError>& {
    return cause_;
  }
  /// @brief Reads the reply's status.
  ///
  /// @return The cause's status, or 0.
  auto http_status() const noexcept -> std::uint16_t;
  /// @brief Reads how many HTTP attempts were made.
  ///
  /// @return The cause's attempts, or 0.
  auto attempts() const noexcept -> int;
  /// @brief Describes the failure as JSON for diagnostics: `status`,
  /// `operation`, `graphqlStatus`, `httpStatus`, `code`, `attempts`.
  ///
  /// @return The document; empty when it could not be written.
  auto summary() const -> std::string;

  /// @brief Names a status.
  ///
  /// @param status The status.
  /// @return The enumerator's name.
  static auto status_name(CheckStatus status) noexcept -> std::string_view;

 private:
  /// @brief A GraphQL failure's kind.
  using Kind = GraphqlError::Kind;

  /// @brief The summary document.
  struct Document {
    /// @brief Field names are written in camel case.
    using keys = json::ps::CamelCaseKeys;
    /// @brief The status's name.
    std::string_view status;
    /// @brief The operation's name.
    std::string_view operation;
    /// @brief The cause's name; empty when nothing was sent.
    std::string_view graphql_status;
    /// @brief The reply's status.
    std::int64_t http_status{};
    /// @brief The first GraphQL error's code.
    std::string_view code;
    /// @brief HTTP attempts made.
    std::int64_t attempts{};
  };

  /// @brief Classifies an unusable app token.
  ///
  /// @param refusal Why it could not be leased.
  /// @return auth_refused for refused, app_token_expired for expired,
  ///     no_app_token otherwise.
  static auto credential_status(
      const std::optional<auth::AuthError>& refusal) noexcept -> CheckStatus;
  /// @brief Classifies a GraphQL failure.
  ///
  /// @param error The failure.
  /// @return cancelled; auth_refused; malformed for partial, invalid_json and
  ///     malformed; not_found; invalid_request; rejected for errors and
  ///     rejected; the credential's status for missing_credential;
  ///     unavailable for transport and server errors.
  static auto status_of(const GraphqlError& error) noexcept -> CheckStatus;

  /// @brief What the failure means.
  CheckStatus status_;
  /// @brief The operation's name, a constant.
  std::string_view operation_;
  /// @brief The GraphQL failure.
  std::optional<GraphqlError> cause_;
};

}  // namespace libchiaking::psn::graphql
