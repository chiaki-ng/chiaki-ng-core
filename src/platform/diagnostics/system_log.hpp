/// @file
/// @brief Declares the private implementation behind
/// DefaultDiagnosticsProvider: one system log per platform, chosen at build
/// time.
///
/// Apple platforms write to the unified log (`os_log`), other POSIX systems
/// to `syslog`, and Windows to the debugger output
/// (`OutputDebugStringW`).
#pragma once

#include <libchiaking/platform/diagnostics/default.hpp>

#include <memory>
#include <string_view>

namespace libchiaking::platform {

/// @brief The platform's system log, behind DefaultDiagnosticsProvider.
///
/// Each platform's source file defines the one concrete SystemLog and open().
/// Owned by the provider for its lifetime. write() may be called from
/// several threads at once; the system logs are themselves thread-safe and
/// the implementations keep no mutable state.
class DefaultDiagnosticsProvider::Implementation {
 public:
  /// @brief Opens the platform's system log.
  ///
  /// On Apple platforms the subsystem and category name the `os_log`
  /// handle; elsewhere every line is prefixed with `subsystem/category: `.
  ///
  /// @param subsystem The reverse-DNS name of the logging application.
  /// @param category The part of the application that logs.
  /// @return The open log; never null. Allocation failure throws.
  [[nodiscard]] static auto open(std::string_view subsystem,
                                 std::string_view category)
      -> std::unique_ptr<Implementation>;

  /// @brief Constructs the base of a system log.
  Implementation() = default;
  Implementation(const Implementation&) = delete;
  auto operator=(const Implementation&) -> Implementation& = delete;
  /// @brief Closes the log.
  virtual ~Implementation() = default;

  /// @brief Writes one line at its severity.
  ///
  /// Maps error, warning, info and verbose or debug onto the nearest
  /// system level; Windows has no levels. Lines are marked public in the
  /// unified log, so callers must already have masked secrets. A failure
  /// to allocate or convert the line drops it silently.
  ///
  /// @param record The severity and text; the text is copied before
  ///     returning.
  virtual void write(LogRecord record) noexcept = 0;

 private:
  /// @brief The concrete log of the platform being built.
  class SystemLog;
};

}  // namespace libchiaking::platform
