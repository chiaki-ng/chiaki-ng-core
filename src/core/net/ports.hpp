/// @file
/// @brief Names the fixed TCP and UDP ports a PlayStation console listens on
/// for Remote Play, and the largest port number.
#pragma once

#include <cstdint>

namespace libchiaking {

/// @brief The console's Remote Play session port: the TCP port that serves the
/// HTTP session requests (`/sie/ps4/rp/sess/init` and the ctrl connection).
inline constexpr std::uint16_t rp_session_port = 9295;
/// @brief The console's UDP port for the Takion stream, used on a home network
/// unless the route names another.
inline constexpr std::uint16_t rp_stream_port = 9296;
/// @brief The console's UDP port for Senkusha, the network test that measures
/// MTU and round-trip time before streaming.
inline constexpr std::uint16_t senkusha_port = 9297;
/// @brief The largest valid TCP or UDP port number, used to reject and wrap
/// out-of-range ports.
inline constexpr std::uint16_t max_port = 65535;

}  // namespace libchiaking
