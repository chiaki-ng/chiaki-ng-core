/// @file
/// @brief Declares RecordingPlatform, the view of the host's platform each
/// bounded context gets, whose HTTP calls are recorded in the HTTP call log
/// under that context's area.
#pragma once

#include "libchiaking/detail/diagnostics/http_call_log.hpp"
#include "libchiaking/platform/factory.hpp"

#include <chrono>
#include <cstdint>

namespace libchiaking::diagnostics {

/// @brief A PlatformFactory that hands out the host's providers unchanged,
/// except that HTTP goes through a recorder tagging every call with one
/// HttpArea.
///
/// ChiakiNg makes one for each of the users, consoles, catalogue and play
/// contexts, so that the log can tell which context made each call. It
/// refers to the host's platform and the log, both of which must outlive
/// it. Thread-safety is that of the wrapped providers and of HttpCallLog;
/// the wrapper itself has no mutable state.
class RecordingPlatform final : public platform::PlatformFactory {
 public:
  /// @brief Wraps a platform for one context.
  ///
  /// @param platform The host's platform; whether it supplies HTTP, and its
  ///     HTTP provider, are looked up once, now.
  /// @param log Where calls are recorded.
  /// @param area The context every call is recorded under.
  RecordingPlatform(platform::PlatformFactory& platform, HttpCallLog& log,
                    HttpArea area) noexcept;
  RecordingPlatform(const RecordingPlatform&) = delete;
  auto operator=(const RecordingPlatform&) -> RecordingPlatform& = delete;

  /// @brief Returns the recording HTTP provider, even when the host has
  /// none.
  ///
  /// @return The recorder; without a host provider every request fails
  ///     with HttpError::unsupported and nothing is recorded.
  auto recorded_http() noexcept -> platform::HttpProvider& { return recorder_; }

  /// @brief Describes the host platform.
  ///
  /// @return The host's PlatformInfo.
  auto info() const noexcept -> platform::PlatformInfo override;
  /// @brief Returns the host's threading provider.
  ///
  /// @return The host's provider, unwrapped.
  auto threading() noexcept -> platform::ThreadingProvider& override;
  /// @brief Returns the host's clock.
  ///
  /// @return The host's provider, unwrapped.
  auto clock() noexcept -> platform::ClockProvider& override;
  /// @brief Returns the host's wall clock.
  ///
  /// @return The host's provider, unwrapped.
  auto wall_clock() noexcept -> platform::ClockProvider& override;
  /// @brief Returns the host's random source.
  ///
  /// @return The host's provider, unwrapped.
  auto random() noexcept -> platform::SecureRandomProvider& override;
  /// @brief Returns the host's network provider.
  ///
  /// @return The host's provider, unwrapped.
  auto network() noexcept -> platform::NetworkProvider& override;
  /// @brief Returns the host's crypto provider.
  ///
  /// @return The host's provider, which is a stand-in when it has none.
  auto crypto() noexcept -> platform::CryptoProvider& override;
  /// @brief Returns the recording HTTP provider.
  ///
  /// @return The recorder, the same object as recorded_http(). info()
  ///     reports PlatformCapability::http only when the host supplies it.
  auto http() noexcept -> platform::HttpProvider& override;
  /// @brief Returns the host's WebSocket provider; WebSocket traffic is not
  /// recorded.
  ///
  /// @return The host's provider, which is a stand-in when it has none.
  auto websocket() noexcept -> platform::WebSocketProvider& override;
  /// @brief Returns the host's gateway (UPnP) provider.
  ///
  /// @return The host's provider, which is a stand-in when it has none.
  auto gateway() noexcept -> platform::GatewayProvider& override;
  /// @brief Returns the host's DTLS provider.
  ///
  /// @return The host's provider, which is a stand-in when it has none.
  auto secure_datagram() noexcept -> platform::SecureDatagramProvider& override;
  /// @brief Returns the host's diagnostics provider.
  ///
  /// @return The host's provider, which is a stand-in when it has none.
  auto diagnostics() noexcept -> platform::DiagnosticsProvider& override;

 private:
  /// @brief The HTTP provider that performs each request on the host's
  /// provider and records it.
  class Recorder final : public platform::HttpProvider {
   public:
    /// @brief Wraps the host's HTTP provider.
    ///
    /// @param inner The host's provider, or null when it has none.
    /// @param log Where calls are recorded; must outlive the recorder.
    /// @param area The context calls are recorded under.
    /// @param clock The monotonic clock each call's duration is measured on.
    /// @param wall The wall clock each call's start time is read from.
    Recorder(platform::HttpProvider* inner, HttpCallLog& log, HttpArea area,
             const platform::ClockProvider& clock,
             const platform::ClockProvider& wall) noexcept;

    /// @brief Performs a request and records it with its outcome, start
    /// time and duration.
    ///
    /// Blocks for as long as the host's provider does. Recording happens
    /// after the call, whatever its outcome, and does not change it.
    /// Safe to call from several threads when the host's provider is.
    ///
    /// @param request The request.
    /// @param stop Cancels the request; passed to the host's provider.
    /// @return The host provider's response or error.
    /// @retval platform::HttpError::unsupported The host has no HTTP
    ///     provider; nothing is recorded.
    auto perform(const platform::HttpRequest& request,
                 const platform::StopToken& stop)
        -> Expected<platform::HttpResponse, platform::HttpError> override;

    /// @brief Starts a request on the host's provider and records it when
    /// it is collected.
    ///
    /// A request dropped before it is collected is recorded as cancelled.
    ///
    /// @param request The request; copied for the record.
    /// @param stop Cancels the request; passed to the host's provider.
    /// @return The pending response; it answers
    ///     platform::HttpError::unsupported, recording nothing, when the
    ///     host has no HTTP provider.
    auto start(const platform::HttpRequest& request,
               const platform::StopToken& stop)
        -> platform::PendingHttpResponse override;

   private:
    /// @brief A started request that is recorded when collected.
    class Recording;

    /// @brief Reads the wall clock as a start time.
    ///
    /// @return The current wall-clock time.
    auto wall_now() const noexcept -> HttpCall::Clock::time_point;
    /// @brief Measures the time since a reading of the monotonic clock.
    ///
    /// @param begun An earlier reading of the monotonic clock.
    /// @return The time elapsed since then.
    auto since(std::uint64_t begun) const noexcept -> std::chrono::microseconds;

    /// @brief The host's provider, or null.
    platform::HttpProvider* inner_;
    /// @brief Where calls are recorded.
    HttpCallLog& log_;
    /// @brief The context calls are recorded under.
    HttpArea area_;
    /// @brief The monotonic clock durations are measured on.
    const platform::ClockProvider& clock_;
    /// @brief The wall clock start times are read from.
    const platform::ClockProvider& wall_;
  };

  /// @brief The host's platform.
  platform::PlatformFactory& platform_;
  /// @brief The recording HTTP provider.
  Recorder recorder_;
};

}  // namespace libchiaking::diagnostics
