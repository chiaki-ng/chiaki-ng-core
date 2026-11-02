/// @file
/// @brief Declares how a TlsTrustMode maps onto libcurl's native certificate
/// store options, which differ by platform.
///
/// On Apple platforms the build defines `LIBCHIAKING_CURL_NATIVE_SYSTEM_TRUST`
/// and "system trust" means the operating system's own trust store; elsewhere
/// it means the CA bundle libcurl was built with.
#pragma once

#include <libchiaking/platform/http/provider.hpp>

#include <curl/curl.h>

namespace libchiaking::platform::curl_detail {

#ifdef LIBCHIAKING_CURL_NATIVE_SYSTEM_TRUST
/// @brief Whether system trust is the operating system's native store; true
/// in this build.
inline constexpr bool native_system_trust = true;
#else
/// @brief Whether system trust is the operating system's native store; false
/// in this build, so libcurl's built-in CA bundle is used.
inline constexpr bool native_system_trust = false;
#endif

/// @brief Tells whether a trust mode needs libcurl's native CA option.
///
/// @param mode The requested trust.
/// @return True for system trust when it is native, and always for system
///     trust plus custom authorities; false for custom authorities only.
constexpr bool uses_native_ca(TlsTrustMode mode) noexcept {
  if (mode == TlsTrustMode::system) return native_system_trust;
  return mode == TlsTrustMode::system_plus_custom_ca_pem;
}

/// @brief Picks the `CURLOPT_SSL_OPTIONS` bits for an HTTP request.
///
/// With a native system store, partial chains are refused so that a chain
/// must reach a trusted root.
///
/// @param mode The requested trust.
/// @return `CURLSSLOPT_NATIVE_CA`, with `CURLSSLOPT_NO_PARTIALCHAIN` when
///     system trust is native, or 0 when the native store is not used.
constexpr long http_ssl_options(TlsTrustMode mode) noexcept {
  if (!uses_native_ca(mode)) return 0;
  if (native_system_trust)
    return CURLSSLOPT_NATIVE_CA | CURLSSLOPT_NO_PARTIALCHAIN;
  return CURLSSLOPT_NATIVE_CA;
}

}  // namespace libchiaking::platform::curl_detail
