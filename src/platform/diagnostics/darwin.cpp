#include <libchiaking/platform/diagnostics/default.hpp>
#include "system_log.hpp"

#include <os/log.h>

#include <string>

namespace libchiaking::platform {

class DefaultDiagnosticsProvider::Implementation::SystemLog final
    : public Implementation {
 public:
  SystemLog(const std::string& subsystem, const std::string& category)
      : log_(os_log_create(subsystem.c_str(), category.c_str())) {}

  void write(LogRecord record) noexcept override {
    try {
      const std::string message{record.message};
      os_log_with_type(log_, log_type(record.severity), "%{public}s",
                       message.c_str());
    } catch (...) {
    }
  }

 private:
  static os_log_type_t log_type(LogSeverity severity) noexcept {
    switch (severity) {
      case LogSeverity::error:
        return OS_LOG_TYPE_ERROR;
      case LogSeverity::warning:
        return OS_LOG_TYPE_DEFAULT;
      case LogSeverity::info:
        return OS_LOG_TYPE_INFO;
      case LogSeverity::verbose:
      case LogSeverity::debug:
        return OS_LOG_TYPE_DEBUG;
    }
    return OS_LOG_TYPE_DEFAULT;
  }

  os_log_t log_;
};

auto DefaultDiagnosticsProvider::Implementation::open(
    std::string_view subsystem, std::string_view category)
    -> std::unique_ptr<Implementation> {
  return std::make_unique<SystemLog>(std::string{subsystem},
                                     std::string{category});
}

}  // namespace libchiaking::platform
