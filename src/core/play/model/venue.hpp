/// @file
/// @brief Declares Venue, where a stream is played and, for the cloud, which
/// streaming service carries it.
#pragma once

#include "libchiaking/detail/catalog/platform.hpp"
#include "libchiaking/detail/play/session.hpp"

#include <optional>
#include <string_view>

namespace libchiaking::play {

/// @brief Where a stream runs: a console at home, a console over the
/// internet, or a cloud service of a given generation.
///
/// A value type, valid from construction: only a cloud venue has a service,
/// and it always has one. Immutable and safe to share between threads.
class Venue {
 public:
  /// @brief A console on the local network.
  ///
  /// @return The home venue.
  static constexpr auto home() noexcept -> Venue {
    return Venue{Where::home, std::nullopt};
  }
  /// @brief The player's console reached over the internet.
  ///
  /// @return The internet venue.
  static constexpr auto internet() noexcept -> Venue {
    return Venue{Where::internet, std::nullopt};
  }
  /// @brief A cloud stream on one service.
  ///
  /// @param service The LegacyCloudStream or PS5 cloud service that carries the
  /// stream.
  /// @return The cloud venue for @p service.
  static constexpr auto cloud(catalog::ServiceGeneration service) noexcept
      -> Venue {
    return Venue{Where::cloud, service};
  }
  /// @brief Builds the venue for a Where, choosing a default service for the
  /// cloud.
  ///
  /// @param where Where the stream runs.
  /// @param service The cloud service; ignored for consoles.
  /// @return The venue; a cloud venue with no service given is PS5 cloud.
  static constexpr auto of(
      Where where, std::optional<catalog::ServiceGeneration> service) noexcept
      -> Venue {
    return where == Where::cloud
               ? cloud(service.value_or(catalog::ServiceGeneration::ps5_cloud))
               : Venue{where, std::nullopt};
  }

  /// @brief Reads where the stream runs.
  ///
  /// @return Home, internet or cloud.
  constexpr auto where() const noexcept -> Where { return where_; }
  /// @brief Reads the cloud service.
  ///
  /// @return The service for a cloud venue; empty for a console.
  constexpr auto service() const noexcept
      -> std::optional<catalog::ServiceGeneration> {
    return service_;
  }
  /// @brief Reports whether a console is on the other end.
  ///
  /// @return `true` at home and over the internet.
  constexpr auto on_console() const noexcept -> bool {
    return where_ != Where::cloud;
  }
  /// @brief Reports whether the stream runs on the older LegacyCloudStream
  /// service.
  ///
  /// @return `true` only for a cloud venue on
  /// catalog::ServiceGeneration::old_service.
  constexpr auto on_old_service() const noexcept -> bool {
    return service_ == catalog::ServiceGeneration::old_service;
  }
  /// @brief Names the venue for logs and diagnostics.
  ///
  /// @return "local", "remote" or "cloud".
  constexpr auto name() const noexcept -> std::string_view {
    switch (where_) {
      case Where::home:
        return "local";
      case Where::internet:
        return "remote";
      case Where::cloud:
        break;
    }
    return "cloud";
  }
  /// @brief Names the venue for the player.
  ///
  /// @return "Local console", "Remote console", "LegacyCloudStream" or
  /// "CloudStream".
  constexpr auto label() const noexcept -> std::string_view {
    switch (where_) {
      case Where::home:
        return "Local console";
      case Where::internet:
        return "Remote console";
      case Where::cloud:
        break;
    }
    return on_old_service() ? "LegacyCloudStream" : "CloudStream";
  }
  /// @brief The progress text shown as a start begins.
  ///
  /// @return "Starting local connection", "Starting remote connection",
  ///     "Starting LegacyCloudStream cloud connection" or "Starting CloudStream
  ///     connection".
  constexpr auto start_message() const noexcept -> std::string_view {
    switch (where_) {
      case Where::home:
        return "Starting local connection";
      case Where::internet:
        return "Starting remote connection";
      case Where::cloud:
        break;
    }
    return on_old_service() ? "Starting LegacyCloudStream cloud connection"
                            : "Starting CloudStream connection";
  }

  /// @brief Compares where and service.
  ///
  /// @return `true` when both venues name the same place and service.
  friend constexpr auto operator==(const Venue&, const Venue&) noexcept
      -> bool = default;

 private:
  /// @brief Builds a venue from parts already known to agree.
  ///
  /// @param where Where the stream runs.
  /// @param service The service, present exactly when @p where is cloud.
  constexpr Venue(Where where,
                  std::optional<catalog::ServiceGeneration> service) noexcept
      : where_(where), service_(service) {}

  /// @brief Where the stream runs.
  Where where_;
  /// @brief The cloud service; empty for a console.
  std::optional<catalog::ServiceGeneration> service_;
};

}  // namespace libchiaking::play
