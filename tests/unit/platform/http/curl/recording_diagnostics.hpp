#pragma once

#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include "tests/support/process_threading.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace platform_curl_test {

using namespace libchiaking::platform;

class RecordingDiagnostics final : public DiagnosticsProvider {
 public:
  void write(LogRecord record) noexcept override {
    try {
      Lock lock{guard_};
      entries_.push_back(Entry{record.severity, std::string{record.message}});
      if (!hold_on_.empty() &&
          record.message.find(hold_on_) != std::string_view::npos) {
        hold_on_.clear();
        static_cast<void>(released_.wait_for(lock, hold_for_));
      }
    } catch (...) {
    }
  }

  void hold_once(std::string text, std::chrono::milliseconds duration) {
    const Lock lock{guard_};
    hold_on_ = std::move(text);
    hold_for_ = duration;
  }

  std::size_t count(LogSeverity severity, std::string_view text) const {
    const Lock lock{guard_};
    return static_cast<std::size_t>(
        std::ranges::count_if(entries_, [&](const Entry& entry) {
          return entry.severity == severity &&
                 entry.message.find(text) != std::string::npos;
        }));
  }

  bool mentions(std::string_view text) const {
    const Lock lock{guard_};
    return std::ranges::any_of(entries_, [&](const Entry& entry) {
      return entry.message.find(text) != std::string::npos;
    });
  }

 private:
  struct Entry {
    LogSeverity severity;
    std::string message;
  };

  SteadyClock clock_;
  mutable Mutex guard_{test_support::process_threading()};
  ConditionVariable released_{test_support::process_threading(), clock_};
  std::vector<Entry> entries_;
  std::string hold_on_;
  std::chrono::milliseconds hold_for_{0};
};

}  // namespace platform_curl_test
