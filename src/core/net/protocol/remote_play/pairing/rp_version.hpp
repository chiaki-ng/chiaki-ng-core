/// @file
/// @brief Declares RpVersion, the Remote Play protocol version a console
/// variant speaks and its `RP-Version` text.
#pragma once

#include "src/core/net/protocol/remote_play/registered_console.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace libchiaking::net::protocol::remote_play {

/// @brief A Remote Play protocol version: one of PS4 RP 8.0, 9.0 and 10.0, or
/// PS5 RP 1.0.
///
/// Each known RegistrationTarget has exactly one version and the mapping is
/// fixed at compile time. RegistrationTarget::ps4_unknown has none. A value
/// type, valid from construction.
class RpVersion {
 public:
  /// @brief The console generation a version belongs to. The same text means
  /// different things per family, so replies are read in the requested family.
  enum class Family : std::uint8_t { ps4, ps5 };
  /// @var libchiaking::net::protocol::remote_play::RpVersion::Family::ps4
  /// @brief A PS4 version: 8.0, 9.0 or 10.0.
  /// @var libchiaking::net::protocol::remote_play::RpVersion::Family::ps5
  /// @brief The PS5 version, 1.0.

  /// @brief `RP-Version` of PS4 system software below 7.0.
  static constexpr std::string_view ps4_8 = "8.0";
  /// @brief `RP-Version` of PS4 system software 7.x.
  static constexpr std::string_view ps4_9 = "9.0";
  /// @brief `RP-Version` of PS4 system software 8.0 and newer.
  static constexpr std::string_view ps4_10 = "10.0";
  /// @brief `RP-Version` of every PS5.
  static constexpr std::string_view ps5_1 = "1.0";

  /// @brief The version a console variant speaks.
  ///
  /// @param target The variant.
  /// @return The version, or nothing for RegistrationTarget::ps4_unknown and
  ///     values outside the enumeration.
  [[nodiscard]] static constexpr auto of(RegistrationTarget target) noexcept
      -> std::optional<RpVersion> {
    switch (target) {
      case RegistrationTarget::ps4_8:
      case RegistrationTarget::ps4_9:
      case RegistrationTarget::ps4_10:
      case RegistrationTarget::ps5_1:
        return RpVersion{target};
      case RegistrationTarget::ps4_unknown:
        break;
    }
    return std::nullopt;
  }

  /// @brief Reads an `RP-Version` value a console sent.
  ///
  /// @param text The value, compared exactly.
  /// @param family The generation of the console that sent it.
  /// @return The version, or nothing when the text is not one of that family's
  ///     versions.
  [[nodiscard]] static constexpr auto parse(std::string_view text,
                                            Family family) noexcept
      -> std::optional<RpVersion> {
    if (family == Family::ps5)
      return text == ps5_1 ? std::optional{RpVersion{RegistrationTarget::ps5_1}}
                           : std::nullopt;
    if (text == ps4_8) return RpVersion{RegistrationTarget::ps4_8};
    if (text == ps4_9) return RpVersion{RegistrationTarget::ps4_9};
    if (text == ps4_10) return RpVersion{RegistrationTarget::ps4_10};
    return std::nullopt;
  }

  /// @brief The console variant of this version.
  ///
  /// @return The variant.
  constexpr auto target() const noexcept -> RegistrationTarget {
    return target_;
  }

  /// @brief The generation of this version.
  ///
  /// @return Family::ps5 for PS5 RP 1.0, Family::ps4 otherwise.
  constexpr auto family() const noexcept -> Family {
    return target_ == RegistrationTarget::ps5_1 ? Family::ps5 : Family::ps4;
  }

  /// @brief Whether this is the PS5 version.
  ///
  /// @return True for PS5 RP 1.0.
  constexpr auto ps5() const noexcept -> bool {
    return family() == Family::ps5;
  }

  /// @brief The `RP-Version` text.
  ///
  /// @return The text, such as "10.0".
  constexpr auto text() const noexcept -> std::string_view {
    switch (target_) {
      case RegistrationTarget::ps4_8:
        return ps4_8;
      case RegistrationTarget::ps4_9:
        return ps4_9;
      case RegistrationTarget::ps4_10:
        return ps4_10;
      case RegistrationTarget::ps5_1:
        return ps5_1;
      case RegistrationTarget::ps4_unknown:
        break;
    }
    return {};
  }

 private:
  /// @brief Makes the version of a known variant.
  ///
  /// @param target A variant with a version.
  constexpr explicit RpVersion(RegistrationTarget target) noexcept
      : target_(target) {}

  /// @brief The variant this version belongs to.
  RegistrationTarget target_;
};

}  // namespace libchiaking::net::protocol::remote_play
