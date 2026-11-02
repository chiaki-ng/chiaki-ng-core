#include <libchiaking/platform/diagnostics/default.hpp>
#include "system_log.hpp"

#include <switch.h>

#include <string>
#include <string_view>

namespace libchiaking::platform {

class DefaultDiagnosticsProvider::Implementation::SystemLog final
    : public Implementation {
 public:
  SystemLog(std::string_view subsystem, std::string_view category)
      : prefix_(std::string{subsystem} + "/" + std::string{category} + ": ") {}

  void write(LogRecord record) noexcept override {
    try {
      std::string line = prefix_;
      line.append(level(record.severity));
      line.append(record.message);
      line.push_back('\n');
      (void)::svcOutputDebugString(line.data(), line.size());
    } catch (...) {
    }
  }

 private:
  static auto level(LogSeverity severity) noexcept -> std::string_view {
    switch (severity) {
      case LogSeverity::error:
        return "error: ";
      case LogSeverity::warning:
        return "warning: ";
      case LogSeverity::info:
        return "info: ";
      case LogSeverity::verbose:
        return "verbose: ";
      case LogSeverity::debug:
        return "debug: ";
    }
    return "info: ";
  }

  std::string prefix_;
};

auto DefaultDiagnosticsProvider::Implementation::open(
    std::string_view subsystem, std::string_view category)
    -> std::unique_ptr<Implementation> {
  return std::make_unique<SystemLog>(subsystem, category);
}

}  // namespace libchiaking::platform
