#include <libchiaking/platform/http/curl/selected_provider.hpp>

#include "src/platform/http/curl/transport_log.hpp"

#include <format>
#include <utility>

namespace libchiaking::platform {

SelectedCurlHttpProvider::SelectedCurlHttpProvider(
    const HttpTransportSettings& settings, ThreadingProvider& threading,
    const ClockProvider& clock, DiagnosticsProvider* diagnostics)
    : SelectedCurlHttpProvider(settings, CurlCapabilities::detect(), threading,
                               clock, diagnostics) {}

SelectedCurlHttpProvider::SelectedCurlHttpProvider(
    const HttpTransportSettings& settings, const CurlCapabilities& capabilities,
    ThreadingProvider& threading, const ClockProvider& clock,
    DiagnosticsProvider* diagnostics)
    : threading_(threading),
      clock_(clock),
      capabilities_(declared(settings, capabilities)),
      diagnostics_(diagnostics),
      running_(choose(settings, capabilities_, threading, clock, diagnostics)) {
  curl_detail::TransportLog(diagnostics_, threading_, clock_)
      .chosen(settings.transport, transport(), capabilities_);
}

SelectedCurlHttpProvider::~SelectedCurlHttpProvider() = default;

auto SelectedCurlHttpProvider::declared(const HttpTransportSettings& settings,
                                        CurlCapabilities capabilities)
    -> CurlCapabilities {
  if (settings.tls_handshake_blocks)
    capabilities.asynchronous_tls_handshake = false;
  return capabilities;
}

auto SelectedCurlHttpProvider::choose(const HttpTransportSettings& settings,
                                      const CurlCapabilities& capabilities,
                                      ThreadingProvider& threading,
                                      const ClockProvider& clock,
                                      DiagnosticsProvider* diagnostics)
    -> Running {
  const bool wants_multi =
      settings.transport == HttpTransport::multi ||
      (settings.transport == HttpTransport::automatic && capabilities.multi &&
       capabilities.asynchronous_tls_handshake);
  if (wants_multi) {
    auto multi = CurlMultiHttpProvider::create(settings.multi, capabilities,
                                               threading, clock, diagnostics);
    if (multi) return std::move(multi).value();
  }
  return std::make_unique<CurlHttpProvider>(threading, settings.pool);
}

auto SelectedCurlHttpProvider::perform(const HttpRequest& request,
                                       const StopToken& stop_token)
    -> Expected<HttpResponse, HttpError> {
  return std::visit(
      [&](const auto& provider) {
        return provider->perform(request, stop_token);
      },
      running_);
}

auto SelectedCurlHttpProvider::start(const HttpRequest& request,
                                     const StopToken& stop_token)
    -> PendingHttpResponse {
  return std::visit(
      [&](const auto& provider) {
        return provider->start(request, stop_token);
      },
      running_);
}

void SelectedCurlHttpProvider::drop_connections(
    std::string_view reason) noexcept {
  const curl_detail::TransportLog log(diagnostics_, threading_, clock_);
  try {
    if (const auto* pooled =
            std::get_if<std::unique_ptr<CurlHttpProvider>>(&running_)) {
      const auto closed = (*pooled)->drop_connections();
      log.write(
          LogSeverity::info,
          std::format("dropped connections ({}); {} idle connections closed",
                      reason, closed));
      return;
    }
    const auto& multi =
        std::get<std::unique_ptr<CurlMultiHttpProvider>>(running_);
    const auto running = multi->drop_connections();
    log.write(LogSeverity::info,
              std::format("dropped connections ({}); {} running transfers "
                          "finish on the old ones",
                          reason, running));
  } catch (...) {
  }
}

auto SelectedCurlHttpProvider::transport() const noexcept -> HttpTransport {
  return std::holds_alternative<std::unique_ptr<CurlMultiHttpProvider>>(
             running_)
             ? HttpTransport::multi
             : HttpTransport::pooled;
}

auto SelectedCurlHttpProvider::capabilities() const noexcept
    -> const CurlCapabilities& {
  return capabilities_;
}

}  // namespace libchiaking::platform
