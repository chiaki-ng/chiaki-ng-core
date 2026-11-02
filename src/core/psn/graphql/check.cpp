#include "src/core/psn/graphql/check.hpp"

#include <magic_enum/magic_enum.hpp>

#include <utility>

namespace libchiaking::psn::graphql {

auto CheckError::credential_status(
    const std::optional<auth::AuthError>& refusal) noexcept -> CheckStatus {
  if (!refusal) return CheckStatus::no_app_token;
  switch (refusal->kind()) {
    case auth::AuthErrorKind::refused:
      return CheckStatus::auth_refused;
    case auth::AuthErrorKind::expired:
      return CheckStatus::app_token_expired;
    default:
      break;
  }
  return CheckStatus::no_app_token;
}

auto CheckError::status_of(const GraphqlError& error) noexcept -> CheckStatus {
  switch (error.kind()) {
    case Kind::cancelled:
      return CheckStatus::cancelled;
    case Kind::auth_refused:
      return CheckStatus::auth_refused;
    case Kind::partial:
    case Kind::invalid_json:
    case Kind::malformed:
      return CheckStatus::malformed;
    case Kind::not_found:
      return CheckStatus::not_found;
    case Kind::invalid_request:
      return CheckStatus::invalid_request;
    case Kind::errors:
    case Kind::rejected:
      return CheckStatus::rejected;
    case Kind::missing_credential:
      return credential_status(error.refusal());
    case Kind::transport:
    case Kind::server_error:
      break;
  }
  return CheckStatus::unavailable;
}

CheckError::CheckError(CheckStatus status, std::string_view operation) noexcept
    : status_(status), operation_(operation) {}

CheckError::CheckError(GraphqlError cause)
    : status_(status_of(cause)),
      operation_(cause.operation()),
      cause_(std::move(cause)) {}

auto CheckError::http_status() const noexcept -> std::uint16_t {
  return cause_ ? cause_->http_status() : std::uint16_t{};
}

auto CheckError::attempts() const noexcept -> int {
  return cause_ ? cause_->attempts() : 0;
}

auto CheckError::summary() const -> std::string {
  const auto written = json::ps::write<json::ps::body_options_with_nulls>(
      Document{.status = status_name(status_),
               .operation = operation_,
               .graphql_status =
                   attempts() != 0 ? cause_->name() : std::string_view{},
               .http_status = http_status(),
               .code = cause_ ? cause_->code() : std::string_view{},
               .attempts = attempts()});
  return written ? std::string{written.value().view()} : std::string{};
}

auto CheckError::status_name(CheckStatus status) noexcept -> std::string_view {
  return magic_enum::enum_name(status);
}

}  // namespace libchiaking::psn::graphql
