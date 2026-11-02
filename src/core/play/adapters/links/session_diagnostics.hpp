/// @file
/// @brief Declares SessionDiagnostics, the diagnostic trace of one link: the
/// stages it went through, how long each took, and its failures and health.
#pragma once

#include "src/core/net/protocol/remote_play/stream/active_handshake.hpp"
#include "src/core/net/protocol/remote_play/stream/limits.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/play/adapters/links/link_stages.hpp"
#include "src/core/utils/diagnostics/trace.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace libchiaking::play {

class LinkFailure;

/// @brief A link's trace: records each stage the first time it begins and
/// completes, the first video and audio, handshake states, failures, and a
/// health line at most every five seconds.
///
/// Times are monotonic microseconds from the trace's clock. Not synchronised;
/// used on the link's thread. Owned by the link's active stream.
class SessionDiagnostics : public diagnostics::Trace {
 public:
  /// @brief Builds the trace as diagnostics::Trace does.
  using Trace::Trace;
  /// @brief Records that a stage began; only its first beginning is recorded.
  ///
  /// @param stage The stage.
  void stage(net::protocol::remote_play::FailureStage stage) noexcept;
  /// @brief Runs one stage's work and records how it went and how long it
  /// took since the stage began.
  ///
  /// @tparam F A callable taking nothing and returning a value that converts
  ///     to `bool` for success.
  /// @param stage The stage.
  /// @param operation The work.
  /// @return What @p operation returned.
  template <class F>
  auto observe_stage(net::protocol::remote_play::FailureStage stage,
                     F&& operation) {
    this->stage(stage);
    return call_from(LinkStages::operation(stage),
                     began_[static_cast<unsigned>(stage)],
                     std::forward<F>(operation));
  }
  /// @brief Records that a stage completed, with its duration; only its first
  /// completion is recorded.
  ///
  /// @param stage The stage.
  void complete(net::protocol::remote_play::FailureStage stage) noexcept;
  /// @brief Logs a failure: an error, or info when it was a cancellation.
  ///
  /// @param failure The failure.
  /// @param cleanup Whether it happened while cleaning up.
  void failure(const LinkFailure& failure, bool cleanup = false) const noexcept;
  /// @brief Records state changes, the stream description and the first video
  /// and audio units.
  ///
  /// @param event A link event.
  void event(const net::protocol::remote_play::StreamEvent& event) noexcept;
  /// @brief Logs a change of the active handshake's state.
  ///
  /// @param state The new state.
  void active(net::protocol::remote_play::ActiveHandshakeState state) noexcept;
  /// @brief Logs the media health, at most once per health interval.
  ///
  /// @param value The link's diagnostics.
  void health(
      const net::protocol::remote_play::StreamDiagnostics& value) noexcept;
  /// @brief Tells whether health() would log now, so a caller can skip
  /// gathering diagnostics between intervals.
  ///
  /// @return True when no health was logged yet or the interval has passed.
  auto health_due() const noexcept -> bool;

  /// @brief The most characters of the console's quit text in an outcome trace.
  static constexpr std::size_t console_reason_limit = 160;
  /// @brief The most characters of an error message in an outcome trace.
  static constexpr std::size_t error_message_limit = 200;

  /// @brief Writes the one-line trace stored with a session's outcome.
  ///
  /// @param primary The first failure.
  /// @param cleanup The first cleanup failure.
  /// @param quit The console's quit reason.
  /// @return Route, stage, result, HTTP status, console reason and detail of
  ///     the failure, the cleanup failure, the quit and the error message, as
  ///     present; empty when there was nothing.
  static auto outcome_trace(const std::optional<LinkFailure>& primary,
                            const std::optional<LinkFailure>& cleanup,
                            const net::protocol::remote_play::QuitDetail& quit)
      -> std::string;

 private:
  /// @brief Writes one line to the trace's sink, which must be set.
  ///
  /// @param severity The line's severity.
  /// @param line The line.
  void write(platform::LogSeverity severity,
             const std::string& line) const noexcept;

  /// @var SessionDiagnostics::seen_
  /// @brief The stages that began, one bit per stage.
  ///
  /// @var SessionDiagnostics::completed_
  /// @brief The stages that completed, one bit per stage.
  std::uint64_t seen_{}, completed_{};
  /// @brief When each stage began, in monotonic microseconds.
  std::array<std::uint64_t, net::protocol::remote_play::failure_stage_slots>
      began_{};
  /// @var SessionDiagnostics::video_
  /// @brief Whether the first video unit was recorded.
  ///
  /// @var SessionDiagnostics::audio_
  /// @brief Whether the first audio unit was recorded.
  bool video_{}, audio_{};
  /// @brief The last active handshake state logged.
  std::optional<net::protocol::remote_play::ActiveHandshakeState> active_;
  /// @brief When health was last logged, in monotonic microseconds.
  std::optional<std::uint64_t> last_health_;
  /// @brief The lost ranges counted when loss was last warned about.
  std::uint64_t reported_lost_ranges_{};
  /// @brief The late socket reads counted when loss was last warned about.
  std::uint64_t reported_late_reads_{};
  /// @brief The video packets with unusable unit counts when they were last
  /// logged.
  std::uint64_t reported_video_count_packets_{};
  /// @brief The controller input counters when they were last warned about.
  net::protocol::remote_play::StreamInputDiagnostics reported_input_{};
};

}  // namespace libchiaking::play
