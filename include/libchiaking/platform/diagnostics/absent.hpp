/// @file
/// @brief Declares AbsentDiagnosticsProvider, the stand-in a factory returns
/// when the host supplies no log sink.
#pragma once

#include <libchiaking/platform/diagnostics/provider.hpp>

namespace libchiaking::platform {

/// @brief DiagnosticsProvider that discards every line.
///
/// A factory returns it from PlatformFactory::diagnostics when the host
/// supplies no sink, and leaves PlatformCapability::diagnostics unset. It has
/// no state and may be called from any thread at once.
class AbsentDiagnosticsProvider final : public DiagnosticsProvider {
 public:
  /// @brief Creates the stand-in.
  AbsentDiagnosticsProvider() = default;

  /// @brief Discards the line.
  ///
  /// @param record Ignored.
  void write(LogRecord record) noexcept override { (void)record; }
};

}  // namespace libchiaking::platform
