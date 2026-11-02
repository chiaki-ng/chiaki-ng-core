/// @file
/// @brief Declares LaunchCheck, the named steps of Catalog::clear_for_launch,
/// and the observer that is told as each one starts.
#pragma once

#include <cstdint>
#include <functional>

namespace libchiaking::catalog {

/// @brief Names one check that Catalog::clear_for_launch is about to make.
///
/// The checks are reported in the order they run for the title being cleared,
/// and the first refusal ends the sequence. Not every check runs for every
/// title: a title on the older LegacyCloudStream service reports only
/// LaunchCheck::loading_library, LaunchCheck::membership and
/// LaunchCheck::release, and a PS5 cloud title never reports
/// LaunchCheck::membership. LaunchCheck::loading_library can be reported again
/// when the games are rebuilt part way through.
///
/// @see LaunchCheckObserver
enum class LaunchCheck : std::uint8_t {
  /// @brief The user's games are being loaded, from what is held or from
  /// PS; reported first, and again when the games are rebuilt after an
  /// ownership or access recheck.
  loading_library,
  /// @brief The user's PlayStation Plus tier is being asked of PS;
  /// LegacyCloudStream
  /// titles only, at most once per clearance.
  membership,
  /// @brief The user's PS5 cloud streaming access is being read fresh from
  /// PS's subscription service; PS5 cloud titles only.
  cloud_streaming,
  /// @brief PS is being asked again whether the user owns the title,
  /// because the held ownership of a PS5 cloud title is unknown.
  ownership,
  /// @brief The exact owned PS5 entitlement is being read again; it must
  /// still exist and still be a full game that streams.
  entitlement,
  /// @brief PS's selected-title resolver is being asked for the cloud
  /// version of the PS5 title; a failed request refuses the launch.
  cloud_game,
  /// @brief The PS5 title is being matched against the restriction list of
  /// excluded titles, which is refreshed first when it is stale.
  exclusions,
  /// @brief The release date is being judged against trusted time, which is
  /// synchronised first when the title has a release date and no trusted
  /// time is held.
  release,
};

/// @brief Told as each LaunchCheck starts during Catalog::clear_for_launch.
///
/// It is called synchronously on the thread that called
/// Catalog::clear_for_launch, just before the check it names runs, and never
/// after that call returns. An empty function is never called. Each call holds
/// up the clearance, so it should return quickly.
using LaunchCheckObserver = std::function<void(LaunchCheck)>;

}  // namespace libchiaking::catalog
