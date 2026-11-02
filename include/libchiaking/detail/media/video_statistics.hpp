/// @file
/// @brief Declares VideoStatistics, the counts of what happened to the video a
/// session received.
#pragma once

#include <cstdint>

namespace libchiaking::media {

/// @brief Counts what happened to the video a session received.
///
/// All counters start at zero when the session starts and only grow.
struct VideoStatistics {
  /// @brief Video frames accepted from the stream.
  std::uint64_t units{};
  /// @brief Total size of those frames, in bytes.
  std::uint64_t bytes{};
  /// @brief Frames the stream reported lost, plus frames the frontend
  /// dropped.
  std::uint64_t lost{};
  /// @brief Frames delivered after being pointed at an older reference
  /// picture because their own was lost.
  std::uint64_t recovered{};
  /// @brief Frames the frontend's decoder rejected (Session::video_rejected).
  std::uint64_t decoder_rejections{};
  /// @brief Keyframes requested from the console or service to repair the
  /// picture.
  std::uint64_t keyframe_requests{};
  /// @brief Error code the frontend gave with the most recent decoder
  /// rejection.
  int last_decoder_error{};
};

}  // namespace libchiaking::media
