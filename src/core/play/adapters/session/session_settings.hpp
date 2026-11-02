/// @file
/// @brief Declares SessionSettings, the timing, tolerances and limits a
/// session loop runs with.
#pragma once

#include "src/core/media/video_intake.hpp"
#include "src/core/play/adapters/media/media_delivery.hpp"
#include "src/core/play/adapters/session/console_commands.hpp"
#include "src/core/play/adapters/session/controllers_core.hpp"

#include <chrono>
#include <cstddef>

namespace libchiaking::play {

/// @brief How a session's loop is paced and what it tolerates. Plain data;
/// the defaults are the library's.
struct SessionSettings {
  /// @brief The longest the loop sleeps between passes when nothing is due.
  std::chrono::milliseconds idle_wait{8};
  /// @brief How many transient pump errors in a row the loop tolerates before
  /// it ends the session.
  unsigned pump_error_tolerance{64};
  /// @brief The longest the loop goes between copies of the link's
  /// diagnostics for statistics while the link's state stays the same.
  std::chrono::milliseconds statistics_interval{250};
  /// @brief The most controller states queued.
  std::size_t controller_depth{Controllers::Core::queue_depth};
  /// @brief The most seat requests queued.
  std::size_t seat_depth{Seats::Core::request_depth};
  /// @brief Video validation and keyframe limits.
  media::VideoLimits video{};
  /// @brief Command queue sizes and the rest flush time.
  CommandSettings commands{};
  /// @brief Stage names media faults are reported with.
  MediaStages stages{};
  /// @brief Whether an audio output failure ends the session.
  bool audio_failure_ends_run{};
};

}  // namespace libchiaking::play
