/// @file
/// @brief Declares UtcOffsetProvider, the source of the local time zone offset,
/// and its stock implementation.
#pragma once

#include <chrono>
#include <optional>

namespace libchiaking::platform {

/// @brief Reports how far local time is from UTC.
///
/// The library uses it to tell the cloud streaming service the player's time
/// zone. Implemented by SystemUtcOffset, or by a host or test that wants to fix
/// the zone.
class UtcOffsetProvider {
 public:
  /// @brief Destroys the provider.
  virtual ~UtcOffsetProvider() = default;
  /// @brief Reads the current offset of local time from UTC.
  ///
  /// @return Minutes to add to UTC to obtain local time, positive east of
  ///     Greenwich and including any daylight saving adjustment in force.
  ///     std::nullopt when the offset cannot be determined, in which case the
  ///     library has no time zone to report. An offset of 24 hours or more is
  ///     rejected by the library.
  [[nodiscard]] virtual auto utc_offset() const noexcept
      -> std::optional<std::chrono::minutes> = 0;
};

/// @brief Stock UtcOffsetProvider that asks the operating system for the local
/// time zone.
///
/// It uses localtime_r on POSIX systems and GetTimeZoneInformation on Windows,
/// and holds no state.
class SystemUtcOffset final : public UtcOffsetProvider {
 public:
  /// @brief Reads the operating system's current offset. See
  /// UtcOffsetProvider::utc_offset.
  ///
  /// @return The offset in whole minutes, or std::nullopt when the system
  ///     call fails.
  [[nodiscard]] auto utc_offset() const noexcept
      -> std::optional<std::chrono::minutes> override;
};

}  // namespace libchiaking::platform
