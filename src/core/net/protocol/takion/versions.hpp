/// @file
/// @brief Declares which Takion versions the library speaks by default for each
/// kind of session, and the build switches that change them.
///
/// Takion is the UDP transport of PS's Remote Play: PS4 consoles speak
/// version 9, PS5 consoles up to 20, LegacyCloudStream 12, and CloudStream (PS5
/// cloud streaming) up to 25. Each build pins one version per target through
/// the `LIBCHIAKING_TAKION_VERSION_*` macros;
/// `LIBCHIAKING_TAKION_VERSION_OVERRIDE` replaces every target's version when
/// it is non-zero and available for that target's sessions (profile.hpp checks
/// all of them at compile time).
#pragma once

#include <array>
#include <cstdint>

#include "takion_build_config.hpp"

namespace libchiaking::net::protocol::takion {

/// @brief The build-wide version override; 0 means none.
inline constexpr std::uint32_t takion_build_version_override =
    LIBCHIAKING_TAKION_VERSION_OVERRIDE;

/// @brief The version every session's handshake starts with, before any
/// negotiation commits another: the version 7 framing, with no extended
/// headers and the oldest AV layout.
inline constexpr std::uint32_t takion_bootstrap_version = 7;
/// @brief The version used with PS4 consoles; 9 unless the build says
/// otherwise.
inline constexpr std::uint32_t takion_version_ps4 =
    LIBCHIAKING_TAKION_VERSION_PS4;
/// @brief The version used with PS5 consoles; 20 unless the build says
/// otherwise.
inline constexpr std::uint32_t takion_version_ps5 =
    LIBCHIAKING_TAKION_VERSION_PS5;
/// @brief The version used with LegacyCloudStream; 12 unless the build says
/// otherwise.
inline constexpr std::uint32_t takion_version_legacy_cloud_stream =
    LIBCHIAKING_TAKION_VERSION_LEGACY_CLOUD_STREAM;
/// @brief The version used with CloudStream; 23 unless the build says
/// otherwise.
inline constexpr std::uint32_t takion_version_cloud_stream =
    LIBCHIAKING_TAKION_VERSION_CLOUD_STREAM;
/// @brief The version a Senkusha connection test announces in its BIG, and
/// the version a CloudStream datacenter survey pins: 9.
inline constexpr std::uint32_t senkusha_client_takion_version = 9;
/// @brief The versions a console connection test offers when it negotiates,
/// in ascending order; all are available to console sessions.
inline constexpr std::array<std::uint32_t, 4>
    default_negotiated_takion_versions{12, 15, 18, 20};

}  // namespace libchiaking::net::protocol::takion
