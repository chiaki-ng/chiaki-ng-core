/// @file
/// @brief Declares ProbeDiagnostics, the log a network search keeps of its
/// own probes, so a search that finds nothing can be told apart from one
/// that never reached the network.
#pragma once

#include "libchiaking/platform/diagnostics/provider.hpp"
#include "src/core/consoles/adapters/search/beacon.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace libchiaking::consoles {

/// @brief Counts a search's probes and replies and logs what helps explain
/// a silent network.
///
/// Logs the search's set-up once, each distinct failed probe once (up to
/// distinct_error_limit), and, when no console has answered, a summary of
/// the counts 10 s after the start, at 60 s, and every 300 s after that.
/// With no log, nothing is counted.
///
/// Not safe to share between threads; the search's driving thread uses it.
/// The log must outlive it.
class ProbeDiagnostics {
 public:
  /// @brief Most distinct failed probes logged one by one; later ones are
  /// only counted.
  static constexpr std::size_t distinct_error_limit = 64;

  /// @brief Makes diagnostics that write to `output`.
  ///
  /// @param output The log; null logs and counts nothing.
  explicit ProbeDiagnostics(platform::DiagnosticsProvider* output) noexcept
      : output_(output) {}

  /// @brief Logs the search's set-up and schedules the first silence summary.
  ///
  /// @param now_us When the search started, in monotonic microseconds.
  /// @param interfaces How many interfaces are searched.
  /// @param targets How many unicast addresses a sweep covers.
  /// @param ps4_port The UDP port PS4 searches go to.
  /// @param ps5_port The UDP port PS5 searches go to.
  void start(std::uint64_t now_us, std::size_t interfaces, std::size_t targets,
             std::uint16_t ps4_port, std::uint16_t ps5_port);
  /// @brief Counts a probe and logs its failure, if it is a new one.
  ///
  /// @param destination Where the probe went.
  /// @param error Why it failed, or nothing when it was sent.
  void sent(platform::SocketEndpoint destination,
            std::optional<Beacon::ProbeFailure> error);
  /// @brief Counts a received datagram.
  ///
  /// @param parsed Whether it was admitted as a console's reply.
  void received(bool parsed) noexcept;
  /// @brief Logs the counts when a summary is due and no console has
  /// answered, and schedules the next summary.
  ///
  /// @param now_us The monotonic time, in microseconds.
  /// @param found How many consoles the search currently sees; a summary is
  ///     skipped, but still rescheduled, when this is not zero.
  void silence(std::uint64_t now_us, std::size_t found);

  /// @brief Whether start() has run.
  ///
  /// @return True once started.
  auto started() const noexcept -> bool { return has_started_; }
  /// @brief Whether a silence summary is due.
  ///
  /// @param now_us The monotonic time, in microseconds.
  /// @return True when there is a log and the next summary's time has come.
  auto silence_due(std::uint64_t now_us) const noexcept -> bool {
    return output_ != nullptr &&
           next_silence_.expired(timing::Deadline::instant_of(now_us));
  }

 private:
  /// @brief A failed probe already logged.
  struct Report {
    /// @brief Where it went.
    platform::SocketEndpoint destination{};
    /// @brief Why it failed.
    Beacon::ProbeFailure error{};
  };

  /// @brief When the first silence summary is due, after the start.
  static constexpr std::chrono::seconds first_silence{10};
  /// @brief When the second silence summary is due, after the start.
  static constexpr std::chrono::seconds second_silence{60};
  /// @brief Time between later silence summaries.
  static constexpr std::chrono::seconds repeated_silence{300};

  /// @brief Adds one to a counter, stopping at its maximum.
  ///
  /// @param value The counter.
  static void increment(std::uint64_t& value) noexcept;
  /// @brief The words a failed probe is logged with.
  ///
  /// @param error The failure.
  /// @return `permission denied`, `short send` or `network failure`.
  static auto reason(Beacon::ProbeFailure error) noexcept -> std::string_view;

  /// @brief Writes a line to the log, when there is one.
  ///
  /// @param severity The severity.
  /// @param message The line.
  void write(platform::LogSeverity severity,
             std::string_view message) const noexcept;

  /// @brief The log; null when nothing is logged or counted.
  platform::DiagnosticsProvider* output_;
  /// @brief The distinct failed probes logged so far; the first #reported_
  /// are in use.
  std::array<Report, distinct_error_limit> reports_{};
  /// @brief How many entries of #reports_ are in use.
  std::size_t reported_{};
  /// @brief When the search started, on the monotonic clock.
  timing::Deadline::Instant started_{};
  /// @brief When the next silence summary is due.
  timing::Deadline next_silence_{timing::Deadline::Instant{}};
  /// @brief Probes attempted.
  std::uint64_t probes_{};
  /// @brief Probes that failed.
  std::uint64_t failures_{};
  /// @brief Datagrams received.
  std::uint64_t replies_{};
  /// @brief Datagrams received that were not a console's reply.
  std::uint64_t unparsed_{};
  /// @brief Whether start() has run.
  bool has_started_{};
  /// @brief Whether the note that further distinct failures go unlogged was
  /// written.
  bool suppressed_{};
};

}  // namespace libchiaking::consoles
