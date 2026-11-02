/// @file
/// @brief Declares how Winsock error codes map onto NetError, the stock
/// network provider's error vocabulary on Windows.
#pragma once

#include <winsock2.h>

#include "libchiaking/platform/net/error.hpp"

namespace libchiaking::platform::detail {

/// @brief Maps a Winsock error code onto NetError.
///
/// @param error The code, as `WSAGetLastError()` returns it.
/// @return The matching NetError: would_block for a pending non-blocking
///     operation, connection_reset also for `WSAENETRESET` and
///     `WSAESHUTDOWN`, host_unreachable also for `WSAEHOSTDOWN`,
///     invalid_argument for bad arguments, not_supported for unsupported
///     options and families, and system_failure for any code not listed.
[[nodiscard]] constexpr auto normalize_wsa_error(int error) noexcept
    -> NetError {
  switch (error) {
    case WSAEWOULDBLOCK:
    case WSAEINPROGRESS:
    case WSAEALREADY:
      return NetError::would_block;
    case WSAECONNREFUSED:
      return NetError::connection_refused;
    case WSAECONNRESET:
    case WSAENETRESET:
    case WSAESHUTDOWN:
      return NetError::connection_reset;
    case WSAECONNABORTED:
      return NetError::connection_aborted;
    case WSAENOTCONN:
      return NetError::not_connected;
    case WSAEISCONN:
      return NetError::already_connected;
    case WSAEADDRINUSE:
      return NetError::address_in_use;
    case WSAEADDRNOTAVAIL:
      return NetError::address_not_available;
    case WSAENETUNREACH:
      return NetError::network_unreachable;
    case WSAEHOSTUNREACH:
    case WSAEHOSTDOWN:
      return NetError::host_unreachable;
    case WSAENETDOWN:
      return NetError::network_down;
    case WSAEACCES:
      return NetError::permission_denied;
    case WSAEINVAL:
    case WSAEFAULT:
    case WSAEDESTADDRREQ:
      return NetError::invalid_argument;
    case WSAEMSGSIZE:
      return NetError::message_too_large;
    case WSAENOBUFS:
    case WSA_NOT_ENOUGH_MEMORY:
      return NetError::no_buffers;
    case WSAEMFILE:
      return NetError::resource_exhausted;
    case WSAENOPROTOOPT:
    case WSAEOPNOTSUPP:
    case WSAEAFNOSUPPORT:
    case WSAEPFNOSUPPORT:
    case WSAEPROTONOSUPPORT:
    case WSAESOCKTNOSUPPORT:
      return NetError::not_supported;
    case WSAETIMEDOUT:
      return NetError::timed_out;
    case WSAEINTR:
      return NetError::interrupted;
    default:
      return NetError::system_failure;
  }
}

}  // namespace libchiaking::platform::detail
