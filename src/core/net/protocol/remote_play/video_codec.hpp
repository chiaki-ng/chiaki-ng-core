/// @file
/// @brief Declares VideoCodec, the picture encoding a Remote Play session asks
/// the console to stream.
#pragma once

#include <cstdint>

namespace libchiaking::net::protocol::remote_play {

/// @brief The video encoding the client asks a console or the cloud to send.
///
/// The value travels in three forms: as the encrypted `RP-StreamingType`
/// header of the PS5 control request (1, 2 or 3, see CtrlCodec), as the
/// `videoCodec`/`dynamicRange` pair of a PS5 launch specification ("avc"/"SDR",
/// "hevc"/"SDR", "hevc"/"AUTO"), and as the HEVC flag of the Takion stream
/// profile. PS4 consoles (RP-Server-Type 0 and 1) only stream H.264, so the
/// start path falls back to h264 for them.
enum class VideoCodec : std::uint8_t { h264, h265, h265_hdr };
/// @var VideoCodec::h264
/// @brief H.264/AVC in standard dynamic range; the only codec a PS4 streams.
/// @var VideoCodec::h265
/// @brief H.265/HEVC in standard dynamic range; PS5 only.
/// @var VideoCodec::h265_hdr
/// @brief H.265/HEVC with the console free to send high dynamic range
/// ("AUTO" dynamic range); PS5 only.

}  // namespace libchiaking::net::protocol::remote_play
