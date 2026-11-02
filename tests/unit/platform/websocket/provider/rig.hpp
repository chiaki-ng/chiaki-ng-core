#pragma once

#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/websocket/provider.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace platform_websocket_test {

class ScriptedConnection final
    : public libchiaking::platform::WebSocketConnection {
 public:
  ScriptedConnection(
      libchiaking::platform::WebSocketMessage message,
      std::optional<libchiaking::platform::WebSocketError> receive_error,
      std::optional<libchiaking::platform::WebSocketError> close_error)
      : message_(std::move(message)),
        receive_error_(receive_error),
        close_error_(close_error) {}

  auto receive(const libchiaking::platform::StopToken& stop_token)
      -> libchiaking::Expected<libchiaking::platform::WebSocketMessage,
                               libchiaking::platform::WebSocketError> override {
    if (stop_token.stop_requested())
      return libchiaking::unexpected(
          libchiaking::platform::WebSocketError::cancelled);
    if (receive_error_.has_value())
      return libchiaking::unexpected(*receive_error_);
    return message_.clone();
  }

  auto close()
      -> libchiaking::Expected<void,
                               libchiaking::platform::WebSocketError> override {
    ++close_calls_;
    if (close_error_.has_value()) return libchiaking::unexpected(*close_error_);
    return {};
  }

  [[nodiscard]] auto close_count() const noexcept -> int {
    return close_calls_;
  }

 private:
  libchiaking::platform::WebSocketMessage message_;
  std::optional<libchiaking::platform::WebSocketError> receive_error_;
  std::optional<libchiaking::platform::WebSocketError> close_error_;
  int close_calls_ = 0;
};

class ScriptedProvider final : public libchiaking::platform::WebSocketProvider {
 public:
  ScriptedProvider(
      libchiaking::platform::WebSocketMessage message,
      std::optional<libchiaking::platform::WebSocketError> connect_error)
      : message_(std::move(message)), connect_error_(connect_error) {}

  auto connect(const libchiaking::platform::WebSocketRequest& request,
               const libchiaking::platform::StopToken& stop_token)
      -> libchiaking::Expected<
          std::unique_ptr<libchiaking::platform::WebSocketConnection>,
          libchiaking::platform::WebSocketError> override {
    captured_request_ = request.clone();
    captured_stop_requested_ = stop_token.stop_requested();
    if (captured_stop_requested_)
      return libchiaking::unexpected(
          libchiaking::platform::WebSocketError::cancelled);
    if (connect_error_.has_value())
      return libchiaking::unexpected(*connect_error_);
    std::unique_ptr<libchiaking::platform::WebSocketConnection> connection =
        std::make_unique<ScriptedConnection>(message_.clone(), std::nullopt,
                                             std::nullopt);
    return connection;
  }

  [[nodiscard]] auto captured_request() const noexcept
      -> const libchiaking::platform::WebSocketRequest& {
    return captured_request_;
  }

  [[nodiscard]] auto captured_stop_requested() const noexcept -> bool {
    return captured_stop_requested_;
  }

 private:
  libchiaking::platform::WebSocketMessage message_;
  std::optional<libchiaking::platform::WebSocketError> connect_error_;
  libchiaking::platform::WebSocketRequest captured_request_;
  bool captured_stop_requested_ = false;
};

class WebSocketValuesTest : public ::testing::Test {
 protected:
  static constexpr std::array every_error = {
      libchiaking::platform::WebSocketError::cancelled,
      libchiaking::platform::WebSocketError::timed_out,
      libchiaking::platform::WebSocketError::invalid_url,
      libchiaking::platform::WebSocketError::tls_trust_failed,
      libchiaking::platform::WebSocketError::proxy_failed,
      libchiaking::platform::WebSocketError::disconnected,
      libchiaking::platform::WebSocketError::protocol_error,
      libchiaking::platform::WebSocketError::message_too_large,
      libchiaking::platform::WebSocketError::unsupported,
      libchiaking::platform::WebSocketError::backend_failure,
  };

  [[nodiscard]] static auto bytes(std::initializer_list<unsigned> values)
      -> std::vector<std::byte> {
    std::vector<std::byte> output;
    for (const auto value : values)
      output.push_back(static_cast<std::byte>(value));
    return output;
  }

  [[nodiscard]] static auto same_headers(
      const std::vector<libchiaking::platform::HttpHeader>& value,
      const std::vector<libchiaking::platform::HttpHeader>& expected)
      -> ::testing::AssertionResult {
    if (value.size() != expected.size())
      return ::testing::AssertionFailure()
             << value.size() << " headers rather than " << expected.size();
    for (std::size_t index = 0; index < value.size(); ++index) {
      if (value[index].name != expected[index].name ||
          value[index].value.view() != expected[index].value.view())
        return ::testing::AssertionFailure()
               << "header " << index << " is " << value[index].name
               << " rather than " << expected[index].name;
    }
    return ::testing::AssertionSuccess();
  }
};

}  // namespace platform_websocket_test
