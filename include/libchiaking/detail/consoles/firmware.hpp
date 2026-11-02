/// @file
/// @brief Declares Generation and Firmware, which say what kind of console is
/// being spoken to.
#pragma once

#include "libchiaking/detail/consoles/pairing_target.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace libchiaking::consoles {

/// @brief Which PlayStation generation a console belongs to.
enum class Generation : std::uint8_t {
  /// @brief A PlayStation 4.
  ps4,
  /// @brief A PlayStation 5.
  ps5,
};

/// @brief The firmware family of a console, which selects the Remote Play
/// protocol variant spoken to it.
///
/// A firmware is always one of the known families; an unknown PS4 version is
/// treated as the newest family.
class Firmware {
 public:
  /// @brief The protocol families the library can speak.
  enum class Family : std::uint8_t {
    /// @brief PS4 system software below 7.0. Pairing identifies the user by
    /// online id, so it needs a user with a PSN account.
    ps4_8,
    /// @brief PS4 system software from 7.0 up to, but not including, 8.0.
    ps4_9,
    /// @brief PS4 system software 8.0 and newer.
    ps4_10,
    /// @brief PS5 system software.
    ps5_1,
  };

  /// @brief Lowest PS4 system version number that belongs to Family::ps4_9.
  static constexpr int ps4_9_first_system_version = 7'000'000;
  /// @brief Lowest PS4 system version number that belongs to Family::ps4_10.
  static constexpr int ps4_10_first_system_version = 8'000'000;

  /// @brief Makes a firmware of the given family.
  ///
  /// @param family The protocol family.
  constexpr explicit Firmware(Family family) noexcept : family_(family) {}

  /// @brief Returns the newest family of a generation, the assumption for a
  /// console whose firmware has not been learned.
  ///
  /// @param generation The console's generation.
  /// @return Family::ps5_1 for a PS5 and Family::ps4_10 for a PS4.
  static constexpr auto newest(Generation generation) noexcept -> Firmware {
    return Firmware{generation == Generation::ps5 ? Family::ps5_1
                                                  : Family::ps4_10};
  }

  /// @brief Translates a pairing protocol target into a firmware.
  ///
  /// @param target The target the pairing exchange used or the console
  ///     reported.
  /// @return The firmware, or nothing for RegistrationTarget::ps4_unknown.
  [[nodiscard]] static constexpr auto from(
      RegistrationTarget target) noexcept
      -> std::optional<Firmware> {
    switch (target) {
      case RegistrationTarget::ps4_8:
        return Firmware{Family::ps4_8};
      case RegistrationTarget::ps4_9:
        return Firmware{Family::ps4_9};
      case RegistrationTarget::ps4_10:
        return Firmware{Family::ps4_10};
      case RegistrationTarget::ps5_1:
        return Firmware{Family::ps5_1};
      case RegistrationTarget::ps4_unknown:
        break;
    }
    return std::nullopt;
  }

  /// @brief Works out the firmware from the system version a console states
  /// in its search reply.
  ///
  /// @param generation The console's generation.
  /// @param system_version The version as a decimal number, for example
  ///     "07020001".
  /// @return Family::ps5_1 for a PS5 whatever the version. For a PS4, the
  ///     family the number falls in; Family::ps4_10 when the text is not a
  ///     positive number.
  static auto from_system_version(Generation generation,
                                  std::string_view system_version) noexcept
      -> Firmware;

  /// @brief The protocol family.
  ///
  /// @return The Family.
  constexpr auto family() const noexcept -> Family { return family_; }

  /// @brief The generation the family belongs to.
  ///
  /// @return Generation::ps5 for Family::ps5_1, Generation::ps4 for every other
  /// family.
  constexpr auto generation() const noexcept -> Generation {
    return family_ == Family::ps5_1 ? Generation::ps5 : Generation::ps4;
  }

  /// @brief The pairing protocol target that corresponds to the family.
  ///
  /// @return The RegistrationTarget matching the family.
  constexpr auto target() const noexcept
      -> RegistrationTarget {
    switch (family_) {
      case Family::ps4_8:
        return RegistrationTarget::ps4_8;
      case Family::ps4_9:
        return RegistrationTarget::ps4_9;
      case Family::ps4_10:
        return RegistrationTarget::ps4_10;
      case Family::ps5_1:
        break;
    }
    return RegistrationTarget::ps5_1;
  }

  /// @brief Compares the family.
  friend constexpr auto operator==(const Firmware&, const Firmware&) noexcept
      -> bool = default;

 private:
  Family family_;
};

}  // namespace libchiaking::consoles
