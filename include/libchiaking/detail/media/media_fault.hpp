/// @file
/// @brief Declares MediaFault, why a stream's media pipeline stopped, and the
/// MediaFailure stages it names.
#pragma once

#include <cstdint>
#include <string>

namespace libchiaking::media {

/// @brief Names the part of media handling that failed and ended a session.
enum class MediaFailure : std::uint8_t {
  /// @brief The stream's video description was missing, malformed or larger
  /// than OutputRequest::largest_picture_side allows.
  profile_validation = 1,
  /// @brief A video frame was empty, too large or belonged to no known
  /// profile.
  frame_validation = 2,
  /// @brief The frontend's decoder could not be given a frame.
  decoder_submission = 3,
  /// @brief Lost video could not be reported to the console or service for
  /// recovery.
  recovery_request = 4,
  /// @brief The frontend reported that its video output failed
  /// (Session::video_output_failed).
  decoder_output = 5,
  /// @brief The frontend's audio output failed and
  /// OutputRequest::audio_failure_ends_session was set.
  audio_output = 6,
};

/// @brief Describes the media failure that ended a session.
///
/// Only the first failure of a session is kept.
///
/// @see Outcome::media_fault
struct MediaFault {
  /// @brief Part of media handling that failed.
  MediaFailure kind{MediaFailure::profile_validation};
  /// @brief Name of the stage that failed, for diagnostics; not for display.
  std::string stage{};
  /// @brief Error code supplied by whoever reported the failure, such as the
  /// frontend's decoder.
  int cause{};

  /// @brief Compares kind, stage and cause.
  friend auto operator==(const MediaFault&, const MediaFault&)
      -> bool = default;
};

}  // namespace libchiaking::media
