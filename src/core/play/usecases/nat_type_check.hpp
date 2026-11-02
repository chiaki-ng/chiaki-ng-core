/// @file
/// @brief Declares NatTypeCheck, which finds the host's NAT type.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/nat_type.hpp"
#include "libchiaking/platform/runtime/stop_token.hpp"

namespace libchiaking::platform {
class DiagnosticsProvider;
}  // namespace libchiaking::platform

namespace libchiaking::play {
class StunProbe;
}  // namespace libchiaking::play

namespace libchiaking::play::usecases {

/// @brief Finds the host's NAT type, as type 1, 2 or 3.
///
/// Has the probe read how two STUN servers see one socket, then lets
/// NatMappings decide the type, and logs the result. Holds references only;
/// the probe must outlive it. Blocks while it runs; not for concurrent use.
class NatTypeCheck {
 public:
  /// @brief Prepares the check.
  ///
  /// @param probe Reads the mappings.
  /// @param log Receives log lines; may be null.
  NatTypeCheck(StunProbe& probe, platform::DiagnosticsProvider* log) noexcept;

  /// @brief Runs the check.
  ///
  /// @param stop Cancels the check.
  /// @return The NAT type.
  /// @retval NatTypeFailure Why it could not be determined.
  [[nodiscard]] auto check(const platform::StopToken& stop)
      -> Expected<NatType, NatTypeFailure>;

 private:
  /// @brief Reads the mappings.
  StunProbe& probe_;
  /// @brief Receives log lines; may be null.
  platform::DiagnosticsProvider* log_;
};

}  // namespace libchiaking::play::usecases
