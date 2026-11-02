/// @file
/// @brief The video, feedback and controller choices a caller makes for a
/// home stream.
#pragma once

#include "src/core/net/protocol/remote_play/video_presets.hpp"

#include <cstdint>

namespace libchiaking::net::protocol::remote_play {

/// @brief The picture a local stream asks the console for.
///
/// Plain data; defaults to 1280x720 at 60 fps and 10000 kbit/s.
struct LocalVideoSettings {
  /// @brief Picture width in pixels.
  std::uint32_t width{default_video_preset.width};
  /// @brief Picture height in pixels.
  std::uint32_t height{default_video_preset.height};
  /// @brief Highest frame rate in frames per second; 0 when video is disabled.
  std::uint32_t max_fps{default_video_fps};
  /// @brief Requested bitrate in kilobits per second.
  std::uint32_t bitrate_kbps{default_video_preset.bitrate_kbps};
  /// @brief Whether a 1080p request to a base PS4 (server type 0) is lowered to
  /// 720p.
  bool auto_downgrade{true};
};

/// @brief The feedback style a caller chose. FeedbackDeliveryPolicy::for_mode
/// currently gives both the same pacing.
enum class FeedbackMode : std::uint8_t {
  /// @brief The default style.
  standard,
  /// @brief The compatibility style; paced exactly like standard today.
  switch_compatibility,
};

/// @brief Which controller the client presents to the console.
enum class ControllerPreference : std::uint8_t {
  /// @brief DualSense to a PS5, DualShock 4 to a PS4.
  console_default,
  /// @brief Always a DualShock 4.
  dualshock4,
  /// @brief Always a DualSense.
  dualsense
};

}  // namespace libchiaking::net::protocol::remote_play
