/// @file
/// @brief Declares usecases::CheckProgress, which tells a frontend each launch
/// check as it starts.
#pragma once

#include "libchiaking/detail/catalog/launch_check.hpp"

#include <functional>

namespace libchiaking::catalog::usecases {

/// @brief Reports launch checks to an optional observer.
///
/// Holds a pointer to the observer, which must outlive it; calls it on the
/// thread running the checks.
class CheckProgress {
 public:
  /// @brief Reports to nobody.
  CheckProgress() noexcept = default;
  /// @brief Reports to an observer.
  ///
  /// @param observer Called as each check starts; an empty function reports
  ///     nothing. Must outlive this object.
  explicit CheckProgress(const LaunchCheckObserver& observer) noexcept
      : observer_(&observer) {}

  /// @brief Reports that a check is starting.
  ///
  /// @param check The check.
  void started(LaunchCheck check) const {
    if (observer_ != nullptr && *observer_) (*observer_)(check);
  }

 private:
  /// @brief The observer; null when nobody listens.
  const LaunchCheckObserver* observer_{};
};

}  // namespace libchiaking::catalog::usecases
