/// @file
/// @brief Declares VideoCodec, the video codecs a stream can carry.
#pragma once

#include <cstdint>

namespace libchiaking::media {

/// @brief Names the video codecs a stream can be asked for.
///
/// A request is only a wish: PS4 consoles and the older cloud service always
/// stream `h264`, and PS5 cloud streaming always streams `h265`.
///
/// @see PictureRequest::on_console
/// @see PictureRequest::on_cloud
enum class VideoCodec : std::uint8_t {
  /// @brief AVC.
  h264,
  /// @brief HEVC in standard dynamic range.
  h265,
  /// @brief HEVC with high dynamic range.
  h265_hdr,
};

}  // namespace libchiaking::media
