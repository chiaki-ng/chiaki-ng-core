#include "src/core/net/transport/http/failure.hpp"

namespace libchiaking::net::http {

auto error_name(platform::HttpError error) noexcept -> std::string_view {
  switch (error) {
    case platform::HttpError::cancelled:
      return "cancelled";
    case platform::HttpError::timed_out:
      return "timed_out";
    case platform::HttpError::invalid_url:
      return "invalid_url";
    case platform::HttpError::tls_trust_failed:
      return "tls_trust_failed";
    case platform::HttpError::proxy_failed:
      return "proxy_failed";
    case platform::HttpError::disconnected:
      return "disconnected";
    case platform::HttpError::response_too_large:
      return "response_too_large";
    case platform::HttpError::unsupported:
      return "unsupported";
    case platform::HttpError::backend_failure:
      return "backend_failure";
  }
  return "backend_failure";
}

Failure::Failure(Kind kind, std::optional<platform::HttpError> error,
                 int attempts) noexcept
    : kind_(kind), error_(error), attempts_(attempts) {}

auto Failure::cancelled(int attempts,
                        std::optional<platform::HttpError> error) noexcept
    -> Failure {
  return {Kind::cancelled, error, attempts};
}

auto Failure::transport(platform::HttpError error, int attempts) noexcept
    -> Failure {
  switch (error) {
    case platform::HttpError::cancelled:
      return {Kind::cancelled, error, attempts};
    case platform::HttpError::response_too_large:
      return {Kind::too_large, error, attempts};
    case platform::HttpError::invalid_url:
      return {Kind::invalid_request, error, attempts};
    default:
      return {Kind::transport, error, attempts};
  }
}

auto Failure::invalid_request() noexcept -> Failure {
  return {Kind::invalid_request, std::nullopt, 0};
}

auto Failure::name() const noexcept -> std::string_view {
  switch (kind_) {
    case Kind::cancelled:
      return "cancelled";
    case Kind::too_large:
      return "too_large";
    case Kind::invalid_request:
      return "invalid_request";
    case Kind::transport:
      break;
  }
  return error_ ? error_name(*error_) : std::string_view{"failure"};
}

}  // namespace libchiaking::net::http
