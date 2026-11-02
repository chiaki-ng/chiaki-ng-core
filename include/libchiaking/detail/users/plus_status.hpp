/// @file
/// @brief Declares PlusStatus, what a PSN profile says about PlayStation Plus.
#pragma once

#include <cstdint>

namespace libchiaking::users {

/// @brief Whether a PSN profile reports a PlayStation Plus subscription. It
/// does not say which tier.
enum class PlusStatus : std::uint8_t {
  /// @brief The profile did not say.
  unknown,
  /// @brief The profile reports no subscription.
  absent,
  /// @brief The profile reports an active subscription.
  active,
};

}  // namespace libchiaking::users
