/// @file
/// @brief Declares DiagnosticsProvider, the sink for the library's log lines.
#pragma once

#include <cstdint>
#include <string_view>

namespace libchiaking::platform {

/// @brief Importance of a log line, most severe first.
enum class LogSeverity : std::uint8_t {
  /// @brief An operation failed.
  error,
  /// @brief Something unexpected happened that the library carried on from.
  warning,
  /// @brief A normal milestone worth recording.
  info,
  /// @brief Detail useful when following one operation step by step.
  verbose,
  /// @brief Detail intended for developers.
  debug,
};

/// @brief One log line handed to a DiagnosticsProvider.
struct LogRecord {
  /// @brief How important the line is.
  LogSeverity severity;
  /// @brief The finished UTF-8 text. It is valid only for the duration of the
  /// DiagnosticsProvider::write call.
  std::string_view message;
};

/// @brief Receives every log line the library produces.
///
/// Implemented by the host frontend, or by DefaultDiagnosticsProvider or
/// diagnostics::LogQueue. The library reaches it through
/// PlatformFactory::diagnostics and borrows it; it must outlive everything that
/// writes to it.
///
/// Lines arrive already formatted. Filtering by severity, timestamps, files and
/// anything else a frontend does with them are the implementation's business.
/// @ingroup api_platform
class DiagnosticsProvider {
 public:
  /// @brief Not copyable; writers hold the provider by pointer.
  DiagnosticsProvider(const DiagnosticsProvider&) = delete;
  /// @brief Not copy-assignable; writers hold the provider by pointer.
  DiagnosticsProvider& operator=(const DiagnosticsProvider&) = delete;

 protected:
  /// @brief Constructs the base of an implementation.
  DiagnosticsProvider() = default;

 public:
  /// @brief Destroys the provider. The library never deletes a provider it
  /// was given.
  virtual ~DiagnosticsProvider() = default;

  /// @brief Delivers one log line.
  ///
  /// Called synchronously on whichever library thread produced the line, and
  /// possibly from several threads at once. The call sits on the library's
  /// working paths, so it should return quickly.
  ///
  /// @param record Severity and text. The implementation must use or copy the
  ///     text before returning and must not throw.
  virtual void write(LogRecord record) noexcept = 0;
};

}  // namespace libchiaking::platform
