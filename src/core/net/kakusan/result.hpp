/// @file
/// @brief Helpers over the Kakusan error vocabulary: building a failed Result
/// and
/// naming an error on the wire.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "src/core/net/kakusan/protocol_error.hpp"

#include <cstdint>
#include <string_view>

namespace libchiaking::net::kakusan {

/// @brief Wraps a Kakusan error as the failure of a Result.
///
/// @param error The error.
/// @return The unexpected value.
constexpr auto fail(Error error) noexcept -> Unexpected<Error> {
  return Unexpected<Error>{error};
}

/// @brief The ERROR.code text an error is sent as (RCP WIRE section 7).
///
/// @param error The error.
/// @return Its upper-case registry name; PROTOCOL for the local-only errors
///     crypto_unavailable, internal and invalid_argument.
constexpr auto wire_code(Error error) noexcept -> std::string_view {
  switch (error) {
    case Error::invalid_block:
      return "INVALID_BLOCK";
    case Error::expired:
      return "EXPIRED";
    case Error::version:
      return "VERSION";
    case Error::identity_changed:
      return "IDENTITY_CHANGED";
    case Error::transcript:
      return "TRANSCRIPT";
    case Error::pairing_failed:
      return "PAIRING_FAILED";
    case Error::no_path:
      return "NO_PATH";
    case Error::no_capacity:
      return "NO_CAPACITY";
    case Error::source_join_failed:
      return "SOURCE_JOIN_FAILED";
    case Error::codec:
      return "CODEC";
    case Error::timeout:
      return "TIMEOUT";
    case Error::protocol:
      return "PROTOCOL";
    case Error::source_format:
      return "SOURCE_FORMAT";
    case Error::congestion:
      return "CONGESTION";
    case Error::source_ended:
      return "SOURCE_ENDED";
    case Error::crypto_unavailable:
    case Error::internal:
    case Error::invalid_argument:
      return "PROTOCOL";
  }
  return "PROTOCOL";
}

}  // namespace libchiaking::net::kakusan
