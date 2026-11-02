/// @file
/// @brief The standard Remote Play picture sizes, bitrates and frame rates.
///
/// These are the presets the console streams natively; the local start path
/// uses them for its defaults, for the PS4 1080p downgrade and for the
/// placeholder picture of a session started with video disabled.
#pragma once

#include <cstdint>

namespace libchiaking {

/// @brief A picture size and the bitrate requested for it.
///
/// Plain data; copy freely.
struct VideoPreset {
  /// @brief Picture width in pixels.
  std::uint32_t width;
  /// @brief Picture height in pixels.
  std::uint32_t height;
  /// @brief Requested encoder bitrate in kilobits per second.
  std::uint32_t bitrate_kbps;
};

/// @brief 640x360 at 2000 kbit/s; also the placeholder used when video is
/// disabled.
inline constexpr VideoPreset video_preset_360p{640, 360, 2000};
/// @brief 960x540 at 6000 kbit/s.
inline constexpr VideoPreset video_preset_540p{960, 540, 6000};
/// @brief 1280x720 at 10000 kbit/s; the default, and what a base PS4 is
/// downgraded to from 1080p.
inline constexpr VideoPreset video_preset_720p{1280, 720, 10000};
/// @brief 1920x1080 at 15000 kbit/s; PS4 Pro and PS5 only.
inline constexpr VideoPreset video_preset_1080p{1920, 1080, 15000};

/// @brief 30 frames per second.
inline constexpr std::uint32_t video_fps_30 = 30;
/// @brief 60 frames per second.
inline constexpr std::uint32_t video_fps_60 = 60;

/// @brief The preset a local session uses when the caller names none.
inline constexpr VideoPreset default_video_preset = video_preset_720p;
/// @brief The frame rate a local session uses when the caller names none.
inline constexpr std::uint32_t default_video_fps = video_fps_60;

}  // namespace libchiaking
