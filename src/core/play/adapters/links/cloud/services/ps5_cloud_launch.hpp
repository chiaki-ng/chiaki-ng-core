/// @file
/// @brief Declares Ps5CloudLaunch, the service half of a PS5 cloud launch:
/// what it launches and whether it retries.
#pragma once

#include "libchiaking/detail/catalog/launch_verdict.hpp"
#include "src/core/psn/types.hpp"

#include <string>

namespace libchiaking::play {

/// @brief The PS5 cloud part of a launch: the entitlement the catalog
/// cleared, streamed as PS5. An immutable value.
class Ps5CloudLaunch {
 public:
  /// @brief Takes the entitlement from the cleared launch.
  ///
  /// @param cleared What the catalog cleared.
  explicit Ps5CloudLaunch(const catalog::ClearedLaunch& cleared);

  /// @brief Reads the entitlement launched.
  ///
  /// @return The cleared identifier.
  auto entitlement() const noexcept -> const std::string& {
    return entitlement_;
  }
  /// @brief Names the platform the title streams as.
  ///
  /// @return Always PS5.
  auto platform() const noexcept -> psn::StreamingPlatform {
    return psn::StreamingPlatform::ps5;
  }
  /// @brief Reports whether a reservation PS refused for having no game is
  /// tried once more.
  ///
  /// @param round The round that was refused, from 0.
  /// @return `true` after the first round only.
  auto retries_reservation_after_refusal(unsigned round) const noexcept
      -> bool {
    return round == 0;
  }

 private:
  /// @brief The entitlement launched.
  std::string entitlement_;
};

}  // namespace libchiaking::play
