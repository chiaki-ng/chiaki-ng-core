/// @file
/// @brief Declares the libcurl set-up shared by the stock HTTP and WebSocket
/// providers: global initialisation, request validation, cancellation, TLS
/// trust and proxy settings, and the mapping of libcurl errors.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/http/provider.hpp>

#include <curl/curl.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace libchiaking::platform::curl_detail {

/// @brief Initialises libcurl once for the process, on first use.
///
/// The first call runs `curl_global_init`; it is thread-safe through static
/// initialisation, and later calls only report the first result.
///
/// @return True when libcurl initialised successfully.
[[nodiscard]] bool ready() noexcept;

/// @brief Tells whether text contains a NUL, which would cut a C string short.
///
/// @param value The text.
/// @return True when it contains a NUL byte.
[[nodiscard]] bool contains_nul(std::string_view value) noexcept;

/// @brief Checks that a header can be sent without changing the request's
/// meaning.
///
/// @param name The header name; must be a non-empty RFC 9110 token.
/// @param value The value; must have no NUL, CR or LF, so that it cannot
///     inject another header.
/// @return True when both are acceptable.
[[nodiscard]] bool acceptable_header(std::string_view name,
                                     std::string_view value) noexcept;

/// @brief Reads a URL's scheme with libcurl's own URL parser, so that the
/// check agrees with what libcurl will connect to.
///
/// @param url The URL.
/// @return The scheme in lower case, such as `https`; or nothing when the
///     URL is empty, contains NUL or does not parse.
[[nodiscard]] auto url_scheme(std::string_view url)
    -> std::optional<std::string>;

/// @brief Converts a timeout to the `long` libcurl's millisecond options take.
///
/// @param value The timeout.
/// @return The milliseconds, or nothing when negative or too large for
///     `long`.
[[nodiscard]] auto milliseconds_option(std::chrono::milliseconds value) noexcept
    -> std::optional<long>;

/// @brief libcurl's progress callback (`CURLOPT_XFERINFOFUNCTION`), aborting
/// the transfer once its stop token fires.
///
/// libcurl calls it on the transferring thread about once a second and
/// whenever data moves, so a stop takes effect within about a second.
///
/// @param stop_token The `const StopToken*` set as
///     `CURLOPT_XFERINFODATA`.
/// @return 1, aborting with `CURLE_ABORTED_BY_CALLBACK`, when a stop was
///     requested; 0 otherwise.
[[nodiscard]] int stop_on_request(void* stop_token, curl_off_t, curl_off_t,
                                  curl_off_t, curl_off_t) noexcept;

/// @brief Hands the custom certificate authorities to libcurl, when the
/// trust mode uses them.
///
/// libcurl copies the PEM, so the configuration need not outlive the call.
///
/// @param curl The transfer.
/// @param tls The trust settings.
/// @return True when nothing was needed or libcurl accepted the
///     authorities.
[[nodiscard]] bool trust_custom_authorities(
    CURL* curl, const TlsConfiguration& tls) noexcept;

/// @brief Applies the proxy policy to a transfer.
///
/// The system policy leaves libcurl's defaults, which honour the proxy
/// environment variables; disabled sets an empty proxy; explicit sets the
/// given proxy URL.
///
/// @param curl The transfer.
/// @param tls The settings holding the proxy policy.
/// @return True when libcurl accepted the setting.
[[nodiscard]] bool route_through_proxy(CURL* curl,
                                       const TlsConfiguration& tls) noexcept;

/// @brief Checks trust and proxy settings before any transfer is made.
///
/// @tparam Error The provider's error enumeration, with `proxy_failed` and
///     `tls_trust_failed`.
/// @param tls The settings.
/// @return Nothing when the settings are usable.
/// @retval Error::proxy_failed An explicit proxy has an empty URL or one
///     containing NUL.
/// @retval Error::tls_trust_failed Custom authorities are required but none
///     were given.
template <class Error>
[[nodiscard]] auto usable_tls(const TlsConfiguration& tls) noexcept
    -> Expected<void, Error> {
  if (tls.proxy_policy == HttpProxyPolicy::explicit_proxy &&
      (tls.explicit_proxy_url.empty() || contains_nul(tls.explicit_proxy_url)))
    return unexpected(Error::proxy_failed);
  if (tls.trust_mode != TlsTrustMode::system && tls.custom_ca_pem.empty())
    return unexpected(Error::tls_trust_failed);
  return {};
}

/// @brief Applies trust and proxy settings to a transfer.
///
/// @tparam Error The provider's error enumeration, with `proxy_failed` and
///     `tls_trust_failed`.
/// @param curl The transfer.
/// @param tls The settings, already checked by usable_tls().
/// @param ssl_options The `CURLOPT_SSL_OPTIONS` bits, or 0 to leave them.
/// @return Nothing when libcurl accepted every setting.
/// @retval Error::tls_trust_failed libcurl refused the authorities or the
///     TLS options.
/// @retval Error::proxy_failed libcurl refused the proxy setting.
template <class Error>
[[nodiscard]] auto apply_tls(CURL* curl, const TlsConfiguration& tls,
                             long ssl_options) noexcept
    -> Expected<void, Error> {
  if (!trust_custom_authorities(curl, tls))
    return unexpected(Error::tls_trust_failed);
  if (ssl_options != 0 &&
      curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, ssl_options) != CURLE_OK)
    return unexpected(Error::tls_trust_failed);
  if (!route_through_proxy(curl, tls)) return unexpected(Error::proxy_failed);
  return {};
}

/// @brief Maps a libcurl failure onto the provider's error vocabulary.
///
/// @tparam Error The provider's error enumeration, with `cancelled`,
///     `timed_out`, `invalid_url`, `tls_trust_failed`, `proxy_failed`,
///     `disconnected` and `backend_failure`.
/// @param code The libcurl result; not `CURLE_OK`.
/// @return cancelled for an aborted callback, timed_out for a timeout,
///     invalid_url for a malformed URL or unsupported scheme,
///     tls_trust_failed for certificate and handshake failures, proxy_failed
///     for proxy failures, disconnected for resolution, connection, send and
///     receive failures, and backend_failure for anything else.
template <class Error>
[[nodiscard]] constexpr auto failure_of(CURLcode code) noexcept -> Error {
  switch (code) {
    case CURLE_ABORTED_BY_CALLBACK:
      return Error::cancelled;
    case CURLE_OPERATION_TIMEDOUT:
      return Error::timed_out;
    case CURLE_URL_MALFORMAT:
    case CURLE_UNSUPPORTED_PROTOCOL:
      return Error::invalid_url;
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
    case CURLE_SSL_CONNECT_ERROR:
      return Error::tls_trust_failed;
    case CURLE_PROXY:
    case CURLE_COULDNT_RESOLVE_PROXY:
      return Error::proxy_failed;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
      return Error::disconnected;
    default:
      return Error::backend_failure;
  }
}

}  // namespace libchiaking::platform::curl_detail
