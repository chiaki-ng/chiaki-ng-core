#pragma once

#include <libchiaking/platform/diagnostics/provider.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace test_support {

class FakeLog final : public libchiaking::platform::DiagnosticsProvider {
 public:
  void write(libchiaking::platform::LogRecord record) noexcept override {
    try {
      messages.emplace_back(record.message);
    } catch (...) {
    }
  }

  auto contains(std::string_view needle) const -> bool {
    return std::ranges::any_of(messages, [needle](const std::string& message) {
      return message.find(needle) != std::string::npos;
    });
  }

  std::vector<std::string> messages;
};

}  // namespace test_support
