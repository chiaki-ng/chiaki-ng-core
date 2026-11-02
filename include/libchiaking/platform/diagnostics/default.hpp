/// @file
/// @brief Declares DefaultDiagnosticsProvider, the stock sink that forwards log
/// lines to the operating system's log.
#pragma once

#include <libchiaking/platform/diagnostics/provider.hpp>

#include <memory>
#include <string_view>

namespace libchiaking::platform {

/// @brief Stock DiagnosticsProvider that writes each line to the system log.
///
/// It uses os_log on Apple systems, syslog on other POSIX systems and the
/// debugger output stream on Windows, and depends on nothing else. It does no
/// filtering. A line that cannot be delivered is dropped silently. write() may
/// be called from any thread at once and blocks only as long as the system log
/// call does.
class DefaultDiagnosticsProvider final : public DiagnosticsProvider {
 public:
  /// @brief Creates a sink labelled with a subsystem and a category.
  ///
  /// @param subsystem Identifier of the application, for example a
  ///     reverse-DNS name. Copied.
  /// @param category Label for this stream of lines within the subsystem.
  ///     Copied.
  /// @note On Apple systems the pair selects the os_log subsystem and
  ///     category. Elsewhere each line is prefixed with "subsystem/category:
  ///     ".
  DefaultDiagnosticsProvider(std::string_view subsystem,
                             std::string_view category);
  /// @brief Releases the sink.
  ~DefaultDiagnosticsProvider() override;

  /// @brief Not copyable; writers hold the sink by pointer.
  DefaultDiagnosticsProvider(const DefaultDiagnosticsProvider&) = delete;
  /// @brief Not copy-assignable; writers hold the sink by pointer.
  DefaultDiagnosticsProvider& operator=(const DefaultDiagnosticsProvider&) =
      delete;

  /// @brief Forwards one line to the system log. See
  /// DiagnosticsProvider::write.
  ///
  /// @param record Line to forward. LogSeverity::verbose and
  ///     LogSeverity::debug are both logged at the system's debug level.
  void write(LogRecord record) noexcept override;

 private:
  class Implementation;
  std::unique_ptr<Implementation> implementation_;
};

}  // namespace libchiaking::platform
