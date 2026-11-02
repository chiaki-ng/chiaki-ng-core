#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <libchiaking/platform/diagnostics/default.hpp>
#include <windows.h>

#include "system_log.hpp"

#include <cstddef>
#include <limits>
#include <string>

namespace libchiaking::platform {

class DefaultDiagnosticsProvider::Implementation::SystemLog final
    : public Implementation {
 public:
  SystemLog(std::string_view subsystem, std::string_view category)
      : prefix_(std::string{subsystem} + "/" + std::string{category} + ": ") {}

  void write(LogRecord record) noexcept override {
    try {
      std::string message = prefix_ + std::string{record.message};
      message.push_back('\n');
      if (message.size() >
          static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return;
      const int input_size = static_cast<int>(message.size());
      const int length = ::MultiByteToWideChar(CP_UTF8, 0, message.data(),
                                               input_size, nullptr, 0);
      if (length <= 0) return;
      std::wstring wide(static_cast<std::size_t>(length), L'\0');
      if (::MultiByteToWideChar(CP_UTF8, 0, message.data(), input_size,
                                wide.data(), length) != length)
        return;
      ::OutputDebugStringW(wide.c_str());
    } catch (...) {
    }
  }

 private:
  std::string prefix_;
};

auto DefaultDiagnosticsProvider::Implementation::open(
    std::string_view subsystem, std::string_view category)
    -> std::unique_ptr<Implementation> {
  return std::make_unique<SystemLog>(subsystem, category);
}

}  // namespace libchiaking::platform
