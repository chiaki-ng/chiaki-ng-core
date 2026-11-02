/// @file
/// @brief Declares CurlHttpProvider, the stock HttpProvider built on libcurl.
#pragma once

#include <libchiaking/platform/http/curl/transport.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <cstddef>
#include <memory>

namespace libchiaking::platform {

/// @brief Stock HttpProvider that performs requests with libcurl.
///
/// It depends on libcurl and the TLS backend libcurl was built with. Peer and
/// host name verification are always on. It accepts http and https URLs.
///
/// One provider can serve requests from several threads at once. It shares a
/// DNS cache and TLS sessions between requests and keeps a small pool of idle
/// connections for reuse, all guarded by locks made by the threading provider
/// it is given. The stop token is
/// checked while a transfer runs.
///
/// A request whose TLS connection cannot be set up is sent once more after
/// the idle connections are closed, so a backend that has run out of TLS
/// sessions recovers the ones the pool was holding.
///
/// DefaultPlatform and CurlPlatformServices each create one.
class CurlHttpProvider final : public HttpProvider {
 public:
  /// @brief Creates the provider and its shared connection cache,
  /// initialising libcurl on first use.
  ///
  /// If the cache cannot be created the provider still works, without sharing
  /// between requests.
  ///
  /// @param threading Makes the locks that guard the shared cache and the
  ///     idle connections; must outlive the provider.
  /// @param settings How many idle handles and connections the pool keeps.
  explicit CurlHttpProvider(ThreadingProvider& threading,
                            const CurlPoolSettings& settings = {});
  /// @brief Closes the pooled idle connections.
  ~CurlHttpProvider() override;

  /// @brief Performs one request with libcurl, blocking until it completes.
  ///
  /// @param request The request to send. It is read during the call only.
  /// @param stop_token Cancels the transfer when stopped.
  /// @return The final response after any permitted redirects, whatever its
  ///     status code.
  /// @retval HttpError::cancelled The stop token was stopped before or during
  ///     the transfer.
  /// @retval HttpError::timed_out The connect or total timeout elapsed.
  /// @retval HttpError::invalid_url The URL is empty or malformed, is neither
  ///     http nor https, or is not https while redirects are to be followed.
  /// @retval HttpError::tls_trust_failed Certificate verification or the TLS
  ///     handshake failed, or a custom trust mode was given no CA
  ///     certificates.
  /// @retval HttpError::proxy_failed The proxy failed, or an explicit proxy
  ///     was requested with an empty or invalid URL.
  /// @retval HttpError::disconnected The host could not be resolved or
  ///     connected to, or the transfer was cut short.
  /// @retval HttpError::response_too_large The body exceeded
  ///     HttpRequest::maximum_response_bytes.
  /// @retval HttpError::backend_failure libcurl could not be set up, a header
  ///     name or value was invalid, a timeout was negative, or libcurl
  ///     reported an error not listed above.
  [[nodiscard]] auto perform(const HttpRequest& request,
                             const StopToken& stop_token)
      -> Expected<HttpResponse, HttpError> override;

  /// @brief Closes every pooled connection, so the next requests connect
  /// afresh.
  ///
  /// Call it after the device sleeps or its network changes, when pooled
  /// connections are likely dead. Transfers already running finish
  /// normally, and their connections are closed when they end instead of
  /// being pooled. Safe to call from any thread.
  ///
  /// @return How many idle pooled connections were closed.
  auto drop_connections() noexcept -> std::size_t;

 private:
  class ConnectionCache;

  std::unique_ptr<ConnectionCache> connections_;
};

}  // namespace libchiaking::platform
