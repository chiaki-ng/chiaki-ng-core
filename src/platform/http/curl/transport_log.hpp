/// @file
/// @brief Declares TransportLog, which records how the curl HTTP transport
/// was chosen and every way it runs short of the full design.
#pragma once

#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/http/curl/transport.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace libchiaking::platform::curl_detail {

/// @brief Writes the curl transport's choice and degradations to a
/// DiagnosticsProvider.
///
/// Records name hosts and request labels only, never URLs, queries or
/// headers. Events that can repeat are limited to one record per interval
/// for each kind, and the next record counts the ones held back. Safe to use
/// from several threads at once; a null sink makes every call do nothing.
class TransportLog {
 public:
  /// @brief Runtime events that can repeat, each limited separately.
  enum class Repeating : std::uint8_t {
    /// @brief The transfer loop went too long without a turn.
    stall,
    /// @brief A request gave up waiting on a stalled loop.
    stall_timeout,
    /// @brief Requests waited for a free connection.
    connection_queue,
    /// @brief Host names were resolved on callers' threads.
    caller_lookups,
  };

  /// @brief How often each repeating event may be written by default.
  static constexpr std::chrono::seconds default_interval{30};

  /// @brief Starts a log that writes to a sink.
  ///
  /// @param sink Receives the records, borrowed for the log's lifetime.
  ///     Null discards them.
  /// @param threading Makes the lock that guards the limiter; borrowed for
  ///     the log's lifetime.
  /// @param clock The monotonic clock intervals are measured on; borrowed for
  ///     the log's lifetime.
  /// @param interval Least time between two records of one repeating kind.
  TransportLog(DiagnosticsProvider* sink, ThreadingProvider& threading,
               const ClockProvider& clock,
               std::chrono::microseconds interval = default_interval);

  /// @brief Not copyable: it holds the limiter's state.
  TransportLog(const TransportLog&) = delete;
  /// @brief Not copyable: it holds the limiter's state.
  TransportLog& operator=(const TransportLog&) = delete;

  /// @brief Writes one record.
  ///
  /// @param severity How important it is.
  /// @param message The text, prefixed with the transport's tag.
  void write(LogSeverity severity, std::string_view message) const noexcept;

  /// @brief Writes a repeating event unless one of its kind was written
  /// within the interval, in which case it is only counted.
  ///
  /// @param kind Which event.
  /// @param severity How important it is.
  /// @param compose Builds the text; called only when the record is
  ///     written.
  template <class Compose>
  void repeat(Repeating kind, LogSeverity severity,
              Compose&& compose) noexcept {
    const auto held_back = admit(kind);
    if (!held_back) return;
    try {
      std::string message = compose();
      if (*held_back != 0) message += suppressed_note(*held_back);
      write(severity, message);
    } catch (...) {
    }
  }

  /// @brief Records which transport runs and what libcurl offers.
  ///
  /// Writes nothing when pooled was asked for and runs, so a platform
  /// using the default transport starts with an empty log.
  ///
  /// @param requested The transport the settings asked for.
  /// @param running The transport that runs.
  /// @param capabilities What the probe found.
  void chosen(HttpTransport requested, HttpTransport running,
              const CurlCapabilities& capabilities) const noexcept;

  /// @brief Records each way a multi provider will run short of the full
  /// design: no wakeup, resolving on callers' threads, no multiplexing, or
  /// a DNS strategy forced against the probe.
  ///
  /// @param settings The provider's settings.
  /// @param capabilities What the provider may rely on.
  /// @param dns The DNS strategy the provider settled on.
  void multi_limits(const CurlMultiSettings& settings,
                    const CurlCapabilities& capabilities,
                    CurlDnsStrategy dns) const noexcept;

  /// @brief The name used in records for a transport.
  ///
  /// @param transport The transport.
  /// @return Its lower-case name.
  [[nodiscard]] static auto name(HttpTransport transport) noexcept
      -> std::string_view;
  /// @brief The name used in records for a DNS strategy.
  ///
  /// @param dns The strategy.
  /// @return Its lower-case name.
  [[nodiscard]] static auto name(CurlDnsStrategy dns) noexcept
      -> std::string_view;

 private:
  /// @brief One repeating kind's limiter state.
  struct Window {
    /// @brief When a record of this kind was last written, as the clock's
    /// reading.
    std::optional<std::chrono::microseconds> last;
    /// @brief How many were held back since.
    std::size_t held_back = 0;
  };

  /// @brief Decides whether a repeating event is written now.
  ///
  /// @param kind Which event.
  /// @return How many were held back before it when it is written, or
  ///     nothing when it is held back.
  [[nodiscard]] auto admit(Repeating kind) noexcept
      -> std::optional<std::size_t>;
  /// @brief The note appended when records were held back.
  ///
  /// @param count How many.
  /// @return The note.
  [[nodiscard]] static auto suppressed_note(std::size_t count) -> std::string;
  /// @brief Describes capabilities in one line.
  ///
  /// @param capabilities What the probe found.
  /// @return The description.
  [[nodiscard]] static auto describe(const CurlCapabilities& capabilities)
      -> std::string;

  /// @brief Receives the records, or null.
  DiagnosticsProvider* sink_;
  /// @brief The monotonic clock intervals are measured on.
  const ClockProvider& clock_;
  /// @brief Least time between two records of one kind.
  std::chrono::microseconds interval_;
  /// @brief Guards windows_.
  Mutex guard_;
  /// @brief One limiter per repeating kind.
  std::array<Window, 4> windows_;
};

}  // namespace libchiaking::platform::curl_detail
