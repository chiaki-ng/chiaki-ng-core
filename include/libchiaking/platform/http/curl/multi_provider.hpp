/// @file
/// @brief Declares CurlMultiHttpProvider, the HttpProvider that runs every
/// request on one libcurl multi handle.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/http/curl/transport.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <cstddef>
#include <memory>

namespace libchiaking::platform {

/// @brief HttpProvider that runs every request on one libcurl multi handle,
/// driven by a worker thread it owns.
///
/// Requests from any thread share one connection pool, capped per host and
/// in total, so a burst of requests queues inside libcurl instead of opening
/// more sockets. With HTTP/2 they share connections. perform() still blocks
/// its caller until the response is complete.
///
/// A request whose TLS connection cannot be set up is sent once more on a
/// fresh pool after the idle connections are closed, so a backend that has
/// run out of TLS sessions recovers the ones the pool was holding.
///
/// It relies only on what CurlCapabilities says libcurl can do. Without
/// `curl_multi_wakeup` new work is picked up within
/// CurlMultiSettings::busy_poll; without asynchronous DNS host names are
/// resolved on the caller's thread. Each such limit is written to the
/// diagnostics sink when the provider is created, and runtime trouble such as
/// a stalled loop is written as it happens, at most once per interval for
/// each kind.
///
/// A caller is never left waiting on a loop that has stopped turning: it
/// gives up with HttpError::timed_out after CurlMultiSettings::stall_timeout
/// without a turn, or that long after its own total timeout.
class CurlMultiHttpProvider final : public HttpProvider {
 public:
  /// @brief Probes libcurl and starts a provider.
  ///
  /// @param settings Connection limits, polling and DNS choices.
  /// @param threading Starts the worker thread and makes the provider's
  ///     locks; borrowed for the provider's lifetime.
  /// @param clock The monotonic clock stalls, timeouts and the DNS cache are
  ///     measured on; borrowed for the provider's lifetime.
  /// @param diagnostics Receives the provider's records, borrowed for its
  ///     lifetime. Null discards them.
  /// @return The running provider.
  /// @retval HttpError::backend_failure libcurl could not be initialised,
  ///     the multi or share handle could not be set up, or the worker
  ///     thread could not start.
  [[nodiscard]] static auto create(const CurlMultiSettings& settings,
                                   ThreadingProvider& threading,
                                   const ClockProvider& clock,
                                   DiagnosticsProvider* diagnostics)
      -> Expected<std::unique_ptr<CurlMultiHttpProvider>, HttpError>;

  /// @brief Starts a provider that relies only on the given capabilities.
  ///
  /// Claiming less than libcurl has makes the provider run as it would on
  /// a smaller build, such as the Nintendo Switch's; claiming more is the
  /// caller's risk.
  ///
  /// @param settings Connection limits, polling and DNS choices.
  /// @param capabilities What the provider may rely on.
  /// @param threading Starts the worker thread and makes the provider's
  ///     locks; borrowed for the provider's lifetime.
  /// @param clock The monotonic clock stalls, timeouts and the DNS cache are
  ///     measured on; borrowed for the provider's lifetime.
  /// @param diagnostics Receives the provider's records, borrowed for its
  ///     lifetime. Null discards them.
  /// @return The running provider.
  /// @retval HttpError::backend_failure The capabilities claim no multi
  ///     handle, libcurl could not be initialised, the multi or share
  ///     handle could not be set up, or the worker thread could not start.
  [[nodiscard]] static auto create(const CurlMultiSettings& settings,
                                   const CurlCapabilities& capabilities,
                                   ThreadingProvider& threading,
                                   const ClockProvider& clock,
                                   DiagnosticsProvider* diagnostics)
      -> Expected<std::unique_ptr<CurlMultiHttpProvider>, HttpError>;

  /// @brief Not copyable: the worker thread holds its address.
  CurlMultiHttpProvider(const CurlMultiHttpProvider&) = delete;
  /// @brief Not copyable: the worker thread holds its address.
  CurlMultiHttpProvider& operator=(const CurlMultiHttpProvider&) = delete;

  /// @brief Fails queued and running requests with HttpError::cancelled,
  /// stops the worker and closes every connection.
  ///
  /// @pre No perform() call is still to start.
  ~CurlMultiHttpProvider() override;

  /// @brief Queues one request on the multi handle and blocks until it
  /// completes.
  ///
  /// @param request The request to send. It is copied, so it is read during
  ///     the call only.
  /// @param stop_token Cancels the request when stopped; the transfer is
  ///     removed on the loop's next turn.
  /// @return The final response after any permitted redirects, whatever its
  ///     status code.
  /// @retval HttpError::cancelled The stop token was stopped, or the
  ///     provider is shutting down.
  /// @retval HttpError::timed_out The connect or total timeout elapsed, the
  ///     host name lookup timed out, or the loop stalled.
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

  /// @brief Queues one request on the multi handle and returns while it runs.
  ///
  /// Requests started together run side by side within the connection
  /// limits, and share connections where HTTP/2 allows. The request is
  /// checked, and its host resolved when CurlMultiSettings::dns resolves on
  /// callers' threads, before this returns.
  ///
  /// @param request The request to send. Copied, so it is read during the call
  ///     only.
  /// @param stop_token Cancels the request when stopped.
  /// @return The pending response; its wait() answers as perform() does.
  ///     Dropping it unwaited removes the transfer on the loop's next turn.
  [[nodiscard]] auto start(const HttpRequest& request,
                           const StopToken& stop_token)
      -> PendingHttpResponse override;

  /// @brief Stops reusing every open connection, so the next requests
  /// connect afresh.
  ///
  /// Call it after the device sleeps or its network changes. New requests
  /// go to a fresh multi handle at once; transfers already running finish
  /// on the old one, which is closed when the last of them ends. Safe to
  /// call from any thread.
  ///
  /// @return How many transfers were still running on the old connections.
  auto drop_connections() noexcept -> std::size_t;

  /// @brief What the provider relies on.
  ///
  /// @return The capabilities it was created with.
  [[nodiscard]] auto capabilities() const noexcept -> const CurlCapabilities&;
  /// @brief Where host names are resolved.
  ///
  /// @return CurlDnsStrategy::curl or CurlDnsStrategy::caller, never
  ///     automatic.
  [[nodiscard]] auto dns() const noexcept -> CurlDnsStrategy;

 private:
  class TransferLoop;

  explicit CurlMultiHttpProvider(std::unique_ptr<TransferLoop> loop) noexcept;

  std::unique_ptr<TransferLoop> loop_;
};

}  // namespace libchiaking::platform
