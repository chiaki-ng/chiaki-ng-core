/// @file
/// @brief Declares ServerClock, PS's wall clock as learned from the `Date`
/// headers of PS's replies, and ServerClockHttp, the HTTP provider that feeds
/// it.
///
/// A cloud launch stamps the datacenter measurements it reports to GK with
/// PS's time, not the device's, which may be wrong. The library therefore
/// keeps PS's time as an offset from the monotonic clock, refreshed from
/// every PS reply seen during the launch.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace libchiaking::psn {

/// @brief PS's wall clock, estimated from the last PS reply's `Date`
/// header.
///
/// The estimate is the difference between that header's time and the
/// monotonic clock when it was observed, so it advances with the monotonic
/// clock between replies and is unaffected by changes to the device's own wall
/// clock. Its precision is the header's, one second, and the reply's transit
/// time is not corrected for. Until a PS reply has been seen, or after
/// reset(), it knows no time.
///
/// Each signed-in user's core owns one (users::ServerTime) and it outlives the
/// cloud launches that read it. All members are lock-free and may be called
/// from any thread at once.
class ServerClock {
 public:
  /// @brief Makes a clock that knows no time yet.
  ///
  /// @param monotonic The host's monotonic clock; must outlive this object.
  explicit ServerClock(const platform::ClockProvider& monotonic) noexcept
      : monotonic_(monotonic) {}

  /// @brief Reads an HTTP `Date` header in the RFC 7231 IMF-fixdate form,
  /// such as `Sun, 06 Nov 1994 08:49:37 GMT`.
  ///
  /// The day name is not checked against the date; the zone must be `GMT` or
  /// `UTC` in any case; a leap second (60) is accepted.
  ///
  /// @param value The header value, already trimmed.
  /// @return Milliseconds since the Unix epoch, UTC; nothing when the text is
  ///     not in that form, names an unknown month, names a day the month does
  ///     not have such as 31 February, or lies before 1970.
  static auto parse_http_date(std::string_view value)
      -> std::optional<std::int64_t>;
  /// @brief Tells whether a host belongs to PS.
  ///
  /// @param host The host name, without port.
  /// @return True when it is, ignoring case, one of trusted_host_suffixes or a
  ///     subdomain of one; false otherwise, including for an empty host.
  static auto is_trusted_host(std::string_view host) -> bool;
  /// @brief Extracts the host of an HTTPS URL.
  ///
  /// @param url The URL.
  /// @return The host, a view into `url`; empty when the URL is not a valid
  ///     request target or its scheme is not `https`.
  static auto url_host(std::string_view url) -> std::string_view;

  /// @brief Sets PS's time to a known value at this moment.
  ///
  /// @param server_ms PS's time now, in milliseconds since the Unix epoch.
  void record(std::int64_t server_ms) noexcept;
  /// @brief Learns PS's time from a reply, when it came from PS.
  ///
  /// The reply's final URL (the request URL when there was no redirect) must
  /// be HTTPS on a PS host; the first `Date` header that parses is
  /// recorded. Other replies, and replies without a readable `Date`, leave
  /// the clock unchanged.
  ///
  /// @param request The request that was sent.
  /// @param response The reply it got.
  void observe(const platform::HttpRequest& request,
               const platform::HttpResponse& response) noexcept;
  /// @brief Estimates PS's time now.
  ///
  /// @return Milliseconds since the Unix epoch by PS's clock; nothing when
  ///     no PS time has been learned since construction or reset().
  auto now_ms() const noexcept -> std::optional<std::int64_t>;
  /// @brief Forgets PS's time, so now_ms() answers nothing until the next
  /// PS reply.
  void reset() noexcept;

 private:
  /// @brief The domains whose hosts count as PS's.
  static constexpr std::array<std::string_view, 5> trusted_host_suffixes{
      "playstation.net", "playstation.com", "sony.com", "gaikai.com",
      "sonyentertainmentnetwork.com"};

  /// @brief The month abbreviations of an HTTP date, January first, compared
  /// ignoring case.
  static constexpr std::array<std::string_view, 12> month_names{
      "jan", "feb", "mar", "apr", "may", "jun",
      "jul", "aug", "sep", "oct", "nov", "dec"};
  /// @brief A forward reader over the text of an HTTP date, used only by
  /// parse_http_date().
  class Cursor;

  /// @brief The offset_ms_ value that means no PS time is known.
  static constexpr std::int64_t unset =
      std::numeric_limits<std::int64_t>::min();

  /// @brief Reads the monotonic clock.
  ///
  /// @return The monotonic time in milliseconds, truncated from its
  ///     microseconds.
  auto monotonic_ms() const noexcept -> std::int64_t;

  /// @brief The host's monotonic clock the offset is measured against.
  const platform::ClockProvider& monotonic_;
  /// @brief PS's time minus the monotonic time, in milliseconds; `unset`
  /// until a PS time is recorded.
  std::atomic<std::int64_t> offset_ms_{unset};
};

/// @brief An HTTP provider that passes every request to another and shows each
/// reply to a ServerClock.
///
/// A cloud launch wraps its HTTP provider in one so every PS reply during the
/// launch keeps the clock current. It holds references only; it must not
/// outlive the provider or the clock, and is as thread-safe as the inner
/// provider.
class ServerClockHttp final : public platform::HttpProvider {
 public:
  /// @brief Wraps a provider.
  ///
  /// @param inner The provider that sends the requests; must outlive this
  ///     object.
  /// @param clock The clock that learns from the replies; must outlive this
  ///     object.
  ServerClockHttp(platform::HttpProvider& inner, ServerClock& clock) noexcept
      : inner_(inner), clock_(clock) {}

  /// @brief Sends a request through the inner provider, then shows a
  /// successful reply to the clock.
  ///
  /// Blocks for as long as the inner provider does.
  ///
  /// @param request The request.
  /// @param stop Cancels the request.
  /// @return The inner provider's reply or failure, unchanged.
  auto perform(const platform::HttpRequest& request,
               const platform::StopToken& stop)
      -> Expected<platform::HttpResponse, platform::HttpError> override;

 private:
  /// @brief The provider that sends the requests.
  platform::HttpProvider& inner_;
  /// @brief The clock the replies are shown to.
  ServerClock& clock_;
};

}  // namespace libchiaking::psn
