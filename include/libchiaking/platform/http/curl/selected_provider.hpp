/// @file
/// @brief Declares SelectedCurlHttpProvider, the HttpProvider that runs
/// whichever curl transport the settings and libcurl allow.
#pragma once

#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/http/curl/multi_provider.hpp>
#include <libchiaking/platform/http/curl/provider.hpp>
#include <libchiaking/platform/http/curl/transport.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <cstddef>
#include <memory>
#include <string_view>
#include <variant>

namespace libchiaking::platform {

/// @brief HttpProvider that chooses between CurlHttpProvider and
/// CurlMultiHttpProvider once, when it is created, and forwards every request
/// to the one chosen.
///
/// The choice follows HttpTransportSettings::transport and the capability
/// probe. It never leaves the library without HTTP: when the multi provider
/// cannot start, the pooled one runs instead. The choice, the probe's
/// findings and every fallback are written to the diagnostics sink.
class SelectedCurlHttpProvider final : public HttpProvider {
 public:
  /// @brief Probes libcurl and creates the chosen provider.
  ///
  /// @param settings The transport to run and its tuning.
  /// @param threading Starts the multi provider's worker and makes the
  ///     chosen provider's locks; borrowed for this provider's lifetime.
  /// @param clock The monotonic clock the chosen provider measures time on;
  ///     borrowed for this provider's lifetime.
  /// @param diagnostics Receives the choice and the multi provider's
  ///     records, borrowed for this provider's lifetime. Null discards them.
  SelectedCurlHttpProvider(const HttpTransportSettings& settings,
                           ThreadingProvider& threading,
                           const ClockProvider& clock,
                           DiagnosticsProvider* diagnostics = nullptr);

  /// @brief Creates the chosen provider from capabilities the caller
  /// supplies instead of probing.
  ///
  /// @param settings The transport to run and its tuning.
  /// @param capabilities What the multi provider may rely on.
  /// @param threading Starts the multi provider's worker and makes the
  ///     chosen provider's locks; borrowed for this provider's lifetime.
  /// @param clock The monotonic clock the chosen provider measures time on;
  ///     borrowed for this provider's lifetime.
  /// @param diagnostics Receives the choice and the multi provider's
  ///     records, borrowed for this provider's lifetime. Null discards them.
  SelectedCurlHttpProvider(const HttpTransportSettings& settings,
                           const CurlCapabilities& capabilities,
                           ThreadingProvider& threading,
                           const ClockProvider& clock,
                           DiagnosticsProvider* diagnostics = nullptr);

  /// @brief Not copyable: it owns the running provider.
  SelectedCurlHttpProvider(const SelectedCurlHttpProvider&) = delete;
  /// @brief Not copyable: it owns the running provider.
  SelectedCurlHttpProvider& operator=(const SelectedCurlHttpProvider&) = delete;
  /// @brief Stops the running provider.
  ~SelectedCurlHttpProvider() override;

  /// @brief Performs one request with the running provider.
  ///
  /// @param request The request to send. It is read during the call only.
  /// @param stop_token Cancels the request when stopped.
  /// @return The final response, or the running provider's error; see
  ///     CurlHttpProvider::perform and CurlMultiHttpProvider::perform.
  [[nodiscard]] auto perform(const HttpRequest& request,
                             const StopToken& stop_token)
      -> Expected<HttpResponse, HttpError> override;

  /// @brief Starts a request with the running provider.
  ///
  /// The multi provider runs it while the caller goes on; the pooled
  /// provider sends it when it is waited for.
  ///
  /// @param request The request to send. Copied, so it is read during the call
  ///     only.
  /// @param stop_token Cancels the request when stopped.
  /// @return The running provider's pending response.
  [[nodiscard]] auto start(const HttpRequest& request,
                           const StopToken& stop_token)
      -> PendingHttpResponse override;

  /// @brief Closes or stops reusing every pooled connection and records
  /// why.
  ///
  /// @param reason Why the frontend asked, such as "woke from sleep".
  ///     Written to the log as given, so it must not hold secrets.
  void drop_connections(std::string_view reason) noexcept;

  /// @brief The transport that runs.
  ///
  /// @return HttpTransport::pooled or HttpTransport::multi, never
  ///     automatic.
  [[nodiscard]] auto transport() const noexcept -> HttpTransport;
  /// @brief What the probe found, or what the caller supplied, less what
  /// the settings withdrew.
  ///
  /// @return The capabilities the choice was made from.
  [[nodiscard]] auto capabilities() const noexcept -> const CurlCapabilities&;

 private:
  using Running = std::variant<std::unique_ptr<CurlHttpProvider>,
                               std::unique_ptr<CurlMultiHttpProvider>>;

  static auto declared(const HttpTransportSettings& settings,
                       CurlCapabilities capabilities) -> CurlCapabilities;

  static auto choose(const HttpTransportSettings& settings,
                     const CurlCapabilities& capabilities,
                     ThreadingProvider& threading, const ClockProvider& clock,
                     DiagnosticsProvider* diagnostics) -> Running;

  ThreadingProvider& threading_;
  const ClockProvider& clock_;
  CurlCapabilities capabilities_;
  DiagnosticsProvider* diagnostics_;
  Running running_;
};

}  // namespace libchiaking::platform
