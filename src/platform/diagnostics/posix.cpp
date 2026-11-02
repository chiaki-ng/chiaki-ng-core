#include <libchiaking/platform/diagnostics/default.hpp>
#include "system_log.hpp"

#include <syslog.h>

#include <string>

namespace libchiaking::platform {

class DefaultDiagnosticsProvider::Implementation::SystemLog final
    : public Implementation {
 public:
  SystemLog(std::string_view subsystem, std::string_view category)
      : prefix_(std::string{subsystem} + "/" + std::string{category} + ": ") {}

  void write(LogRecord record) noexcept override {
    try {
      const std::string message = prefix_ + std::string{record.message};
      ::syslog(priority(record.severity), "%s", message.c_str());
    } catch (...) {
    }
  }

 private:
  static int priority(LogSeverity severity) noexcept {
    switch (severity) {
      case LogSeverity::error:
        return LOG_ERR;
      case LogSeverity::warning:
        return LOG_WARNING;
      case LogSeverity::info:
        return LOG_INFO;
      case LogSeverity::verbose:
      case LogSeverity::debug:
        return LOG_DEBUG;
    }
    return LOG_INFO;
  }

  std::string prefix_;
};

auto DefaultDiagnosticsProvider::Implementation::open(
    std::string_view subsystem, std::string_view category)
    -> std::unique_ptr<Implementation> {
  return std::make_unique<SystemLog>(subsystem, category);
}

}  // namespace libchiaking::platform
