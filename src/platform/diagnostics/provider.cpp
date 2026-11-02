#include <libchiaking/platform/diagnostics/default.hpp>
#include "system_log.hpp"

namespace libchiaking::platform {

DefaultDiagnosticsProvider::DefaultDiagnosticsProvider(
    std::string_view subsystem, std::string_view category)
    : implementation_(Implementation::open(subsystem, category)) {}

DefaultDiagnosticsProvider::~DefaultDiagnosticsProvider() = default;

void DefaultDiagnosticsProvider::write(LogRecord record) noexcept {
  implementation_->write(record);
}

}  // namespace libchiaking::platform
