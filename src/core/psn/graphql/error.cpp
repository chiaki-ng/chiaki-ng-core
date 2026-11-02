#include "src/core/psn/graphql/error.hpp"

#include <utility>

namespace libchiaking::psn::graphql {

GraphqlError::GraphqlError(Kind kind, std::string_view operation) noexcept
    : kind_(kind), operation_(operation) {}

auto GraphqlError::failed(std::string_view operation,
                          const net::http::Failure& failure) -> GraphqlError {
  GraphqlError error{failure.is_cancelled() ? Kind::cancelled : Kind::transport,
                     operation};
  if (failure.kind() == net::http::Failure::Kind::invalid_request)
    error.kind_ = Kind::invalid_request;
  error.attempts_ = failure.attempts();
  error.failure_ = failure;
  return error;
}

auto GraphqlError::replied(std::string_view operation, Kind kind,
                           std::uint16_t http_status, int attempts,
                           std::vector<Entry> errors) -> GraphqlError {
  GraphqlError error{kind, operation};
  error.http_status_ = http_status;
  error.attempts_ = attempts;
  error.errors_ = std::move(errors);
  return error;
}

auto GraphqlError::unsent(std::string_view operation, Kind kind)
    -> GraphqlError {
  return GraphqlError{kind, operation};
}

auto GraphqlError::missing_credential(std::string_view operation,
                                      std::optional<auth::AuthError> refusal)
    -> GraphqlError {
  GraphqlError error{Kind::missing_credential, operation};
  error.refusal_ = refusal;
  return error;
}

auto GraphqlError::code() const noexcept -> std::string_view {
  return errors_.empty() ? std::string_view{}
                         : std::string_view{errors_.front().code};
}

auto GraphqlError::name() const noexcept -> std::string_view {
  switch (kind_) {
    case Kind::cancelled:
      return "cancelled";
    case Kind::auth_refused:
      return "auth_refused";
    case Kind::rejected:
      return "rejected";
    case Kind::errors:
      return "graphql_error";
    case Kind::partial:
      return "partial";
    case Kind::invalid_json:
    case Kind::malformed:
      return "malformed";
    case Kind::not_found:
      return "not_found";
    case Kind::invalid_request:
      return "invalid_request";
    case Kind::missing_credential:
      return "no_credential";
    case Kind::transport:
    case Kind::server_error:
      break;
  }
  return "unavailable";
}

auto GraphqlError::credential_name() const noexcept -> std::string_view {
  if (!refusal_) return "missing";
  switch (refusal_->kind()) {
    case auth::AuthErrorKind::refused:
      return "refused";
    case auth::AuthErrorKind::expired:
      return "expired";
    default:
      break;
  }
  return "missing";
}

}  // namespace libchiaking::psn::graphql
