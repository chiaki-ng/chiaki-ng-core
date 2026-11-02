/// @file
/// @brief Names the two PS cloud streaming services and the console
/// generations a cloud game can run on.
///
/// Every cloud streaming step (authorisation codes, GK client ids, the
/// launch specification) is routed by these two values, so they live apart from
/// any one client. The PlayStation Plus status a profile states, which the
/// public API defines, is named here for the PSN-facing code too.
#pragma once

#include "libchiaking/detail/users/plus_status.hpp"

#include <cstdint>

namespace libchiaking::psn {

/// @brief A profile's PlayStation Plus status under the name the PSN-facing
/// code uses.
using PlusStatus = users::PlusStatus;

/// @brief The PS cloud streaming service a cloud session is opened with.
///
/// The two services share the GK backend but differ in every credential:
/// the OAuth client and redirect URI, the user agent, the GK product name
/// and which consoles they stream.
enum class StreamingService : std::uint8_t {
  /// @brief The old PlayStation Now service (GK product `legacy_cloud_stream`),
  /// which
  /// streams PS3, PS4 and PS5 games.
  legacy_cloud_stream,
  /// @brief PS5 cloud streaming for PlayStation Plus Premium (GK product
  /// `qlite`); PS5 games only.
  cloud_stream
};
/// @brief The console generation a cloud game runs on, which picks the GK
/// virtualisation type (`konan`, `kratos`, `cronos`) and the authorisation
/// scope.
enum class StreamingPlatform : std::uint8_t {
  /// @brief A PS3 game; GK virtualisation type `konan`, LegacyCloudStream only.
  ps3,
  /// @brief A PS4 game; GK virtualisation type `kratos`, LegacyCloudStream
  /// only.
  ps4,
  /// @brief A PS5 game; GK virtualisation type `cronos`, on either
  /// service.
  ps5
};

}  // namespace libchiaking::psn
