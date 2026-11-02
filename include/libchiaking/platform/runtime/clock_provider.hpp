/// @file
/// @brief Declares ClockProvider, the time source the library reads,
/// SteadyClock, the stock monotonic one, and SystemClock, the stock wall
/// clock.
#pragma once

#include <cstdint>

namespace libchiaking::platform {

/// @brief A clock that reports the current time in microseconds.
///
/// Implemented by the host frontend, or by SteadyClock and SystemClock. The
/// library reaches the monotonic clock through PlatformFactory::clock and the
/// wall clock through PlatformFactory::wall_clock, and borrows each for as long
/// as anything built from that factory exists. It may be read from any library
/// thread, so an implementation must be safe to call concurrently.
/// @ingroup api_platform
class ClockProvider {
 public:
  /// @brief Destroys the clock. The library never deletes a clock it was
  /// given.
  virtual ~ClockProvider() = default;
  /// @brief Reads the clock.
  ///
  /// @return Microseconds since an origin the implementation chooses. For the
  ///     clock behind PlatformFactory::clock the value must never decrease
  ///     and must never be zero; only differences between readings are
  ///     meaningful. For the clock behind PlatformFactory::wall_clock the
  ///     origin is the Unix epoch, 1970-01-01T00:00:00Z, and the value may
  ///     jump when the device's date and time change.
  virtual std::uint64_t now_microseconds() const noexcept = 0;
};

/// @brief Stock ClockProvider backed by std::chrono::steady_clock.
///
/// It has no state and no dependencies beyond the standard library.
/// DefaultPlatformFactory uses it unless the host supplies another clock.
class SteadyClock final : public ClockProvider {
 public:
  /// @brief Reads std::chrono::steady_clock.
  ///
  /// @return Microseconds since that clock's epoch. Unaffected by changes to
  ///     the system date and time.
  std::uint64_t now_microseconds() const noexcept override;
};

/// @brief Stock wall ClockProvider backed by std::chrono::system_clock.
///
/// It has no state and no dependencies beyond the standard library.
/// DefaultPlatformFactory uses it for PlatformFactory::wall_clock unless the
/// host supplies another.
class SystemClock final : public ClockProvider {
 public:
  /// @brief Reads std::chrono::system_clock.
  ///
  /// @return Microseconds since the Unix epoch. Follows changes to the system
  ///     date and time, so it may jump in either direction.
  std::uint64_t now_microseconds() const noexcept override;
};

}  // namespace libchiaking::platform
