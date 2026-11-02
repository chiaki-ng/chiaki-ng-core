/// @file
/// @brief Declares RegistrationTarget, the variant of the registration
/// protocol a console speaks.
#pragma once

#include <cstdint>

namespace libchiaking::consoles {

/// @brief Which variant of the registration protocol a console speaks.
///
/// @see Firmware
enum class RegistrationTarget : std::uint8_t {
  /// @brief A PS4 on system software below 7.0.
  ps4_8,
  /// @brief A PS4 on system software from 7.0 up to, but not including, 8.0.
  ps4_9,
  /// @brief A PS4 on system software 8.0 and newer.
  ps4_10,
  /// @brief A PS5.
  ps5_1,
  /// @brief A PS4 whose variant is not known. It maps to no
  /// Firmware.
  ps4_unknown,
};

}  // namespace libchiaking::consoles
