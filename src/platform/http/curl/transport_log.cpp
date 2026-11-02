#include "src/platform/http/curl/transport_log.hpp"

#include <format>

namespace libchiaking::platform::curl_detail {

TransportLog::TransportLog(DiagnosticsProvider* sink,
                           ThreadingProvider& threading,
                           const ClockProvider& clock,
                           std::chrono::microseconds interval)
    : sink_(sink), clock_(clock), interval_(interval), guard_(threading) {}

void TransportLog::write(LogSeverity severity,
                         std::string_view message) const noexcept {
  if (sink_ == nullptr) return;
  try {
    const std::string line = std::format("http transport: {}", message);
    sink_->write(LogRecord{.severity = severity, .message = line});
  } catch (...) {
  }
}

void TransportLog::chosen(HttpTransport requested, HttpTransport running,
                          const CurlCapabilities& capabilities) const noexcept {
  if (requested == HttpTransport::pooled && running == HttpTransport::pooled)
    return;
  try {
    const auto summary = describe(capabilities);
    if (requested == HttpTransport::multi && running != HttpTransport::multi) {
      write(LogSeverity::error,
            std::format(
                "multi was requested but could not start; {} runs instead ({})",
                name(running), summary));
      return;
    }
    if (requested == HttpTransport::automatic &&
        running == HttpTransport::pooled) {
      const std::string_view reason =
          !capabilities.multi ? "no usable multi handle was found"
          : !capabilities.asynchronous_tls_handshake
              ? "the TLS backend handshakes in blocking mode"
              : "the multi provider could not start";
      write(LogSeverity::warning,
            std::format("automatic chose pooled because {} ({})", reason,
                        summary));
      return;
    }
    write(LogSeverity::info,
          std::format("{} runs ({})", name(running), summary));
  } catch (...) {
  }
}

void TransportLog::multi_limits(const CurlMultiSettings& settings,
                                const CurlCapabilities& capabilities,
                                CurlDnsStrategy dns) const noexcept {
  try {
    if (!capabilities.wakeup)
      write(LogSeverity::warning,
            std::format("libcurl cannot be woken, so new requests wait up to "
                        "{} ms while transfers run",
                        settings.busy_poll.count()));
    if (!capabilities.asynchronous_tls_handshake)
      write(LogSeverity::warning,
            "the TLS backend handshakes in blocking mode, so every new "
            "connection stalls the other transfers until its handshake ends");
    if (dns == CurlDnsStrategy::caller)
      write(LogSeverity::warning,
            "host names are resolved on callers' threads and handed to libcurl "
            "with CURLOPT_RESOLVE");
    if (settings.http2 == CurlHttp2Policy::off)
      write(LogSeverity::info,
            "HTTP/2 is turned off, so connections are reused but not "
            "multiplexed");
    else if (!capabilities.http2)
      write(LogSeverity::info,
            "libcurl has no HTTP/2, so connections are reused but not "
            "multiplexed");
    if (settings.dns == CurlDnsStrategy::curl && !capabilities.asynchronous_dns)
      write(LogSeverity::warning,
            "dns is forced to curl but libcurl resolves synchronously, so a "
            "slow lookup stalls every transfer");
    if (settings.dns == CurlDnsStrategy::caller &&
        capabilities.asynchronous_dns)
      write(LogSeverity::warning,
            "dns is forced to caller although libcurl resolves asynchronously");
  } catch (...) {
  }
}

auto TransportLog::name(HttpTransport transport) noexcept -> std::string_view {
  switch (transport) {
    case HttpTransport::pooled:
      return "pooled";
    case HttpTransport::multi:
      return "multi";
    case HttpTransport::automatic:
      return "automatic";
  }
  return "unknown";
}

auto TransportLog::name(CurlDnsStrategy dns) noexcept -> std::string_view {
  switch (dns) {
    case CurlDnsStrategy::curl:
      return "curl";
    case CurlDnsStrategy::caller:
      return "caller";
    case CurlDnsStrategy::automatic:
      return "automatic";
  }
  return "unknown";
}

auto TransportLog::admit(Repeating kind) noexcept
    -> std::optional<std::size_t> {
  const std::chrono::microseconds now{
      static_cast<std::chrono::microseconds::rep>(clock_.now_microseconds())};
  const Lock lock{guard_};
  auto& window = windows_[static_cast<std::size_t>(kind)];
  if (window.last && now - *window.last < interval_) {
    ++window.held_back;
    return std::nullopt;
  }
  const std::size_t held_back = window.held_back;
  window.last = now;
  window.held_back = 0;
  return held_back;
}

auto TransportLog::suppressed_note(std::size_t count) -> std::string {
  return std::format("; {} more since the last report", count);
}

auto TransportLog::describe(const CurlCapabilities& capabilities)
    -> std::string {
  auto answer = [](bool value) { return value ? "yes" : "no"; };
  return std::format(
      "libcurl {}, multi {}, wakeup {}, asynchronous dns {}, http2 {}, "
      "asynchronous tls handshake {}",
      capabilities.version.empty() ? "unknown" : capabilities.version,
      answer(capabilities.multi), answer(capabilities.wakeup),
      answer(capabilities.asynchronous_dns), answer(capabilities.http2),
      answer(capabilities.asynchronous_tls_handshake));
}

}  // namespace libchiaking::platform::curl_detail
