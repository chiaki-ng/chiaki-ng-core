#pragma once

#include <libchiaking/platform/websocket/provider.hpp>

#include <atomic>
#include <memory>
#include <optional>

namespace test_support {

class RefusingWebSocket final
    : public libchiaking::platform::WebSocketProvider {
 public:
  RefusingWebSocket() = default;
  explicit RefusingWebSocket(
      libchiaking::platform::WebSocketError error) noexcept
      : error_(error) {}

  auto connect(const libchiaking::platform::WebSocketRequest&,
               const libchiaking::platform::StopToken& stop)
      -> libchiaking::Expected<
          std::unique_ptr<libchiaking::platform::WebSocketConnection>,
          libchiaking::platform::WebSocketError> override {
    ++connects;
    if (error_) return libchiaking::unexpected(*error_);
    return libchiaking::unexpected(
        stop.stop_requested()
            ? libchiaking::platform::WebSocketError::cancelled
            : libchiaking::platform::WebSocketError::disconnected);
  }

  std::atomic<int> connects{};

 private:
  std::optional<libchiaking::platform::WebSocketError> error_;
};

}  // namespace test_support
