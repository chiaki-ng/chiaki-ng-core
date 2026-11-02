/// @file
/// @brief Declares ProtocolRefusal, which turns a failure of the Kakusan
/// protocol into the Refusal frontends see.
#pragma once

#include "libchiaking/detail/kakusan/values.hpp"
#include "src/core/net/kakusan/protocol_error.hpp"

namespace libchiaking::kakusan {

/// @brief Names a Kakusan protocol failure as a Refusal.
///
/// The protocol's errors stay inside the library; every Kakusan call that
/// fails because of one reports the Refusal this gives.
class ProtocolRefusal {
 public:
  /// @brief Translates an error from the protocol.
  /// @param error The protocol's error.
  /// @return The refusal; net::kakusan::Error::crypto_unavailable and
  ///     net::kakusan::Error::internal both become Refusal::Kind::internal.
  static constexpr auto of(net::kakusan::Error error) noexcept -> Refusal {
    using Told = net::kakusan::Error;
    using Kind = Refusal::Kind;
    switch (error) {
      case Told::invalid_block:
        return Kind::invalid_text;
      case Told::expired:
        return Kind::expired;
      case Told::version:
        return Kind::unsupported_version;
      case Told::identity_changed:
        return Kind::identity_changed;
      case Told::transcript:
        return Kind::tampered;
      case Told::pairing_failed:
        return Kind::verification_failed;
      case Told::no_path:
        return Kind::unreachable;
      case Told::no_capacity:
        return Kind::no_free_seat;
      case Told::source_join_failed:
        return Kind::console_refused_seat;
      case Told::codec:
        return Kind::malformed;
      case Told::timeout:
        return Kind::timed_out;
      case Told::protocol:
        return Kind::protocol;
      case Told::source_format:
        return Kind::stream_not_shareable;
      case Told::congestion:
        return Kind::connection_too_slow;
      case Told::source_ended:
        return Kind::session_ended;
      case Told::invalid_argument:
        return Kind::invalid_argument;
      case Told::crypto_unavailable:
      case Told::internal:
        break;
    }
    return Kind::internal;
  }
};

}  // namespace libchiaking::kakusan
