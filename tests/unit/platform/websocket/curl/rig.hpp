#pragma once

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/platform/websocket/curl/provider.hpp>

#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/unit/platform/websocket/curl/loopback_websocket_server.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace platform_curl_test {

using namespace libchiaking::platform;

class CurlWebSocketRig : public testing::Test {
 protected:
  class DeadlineStop final : public StopState {
   public:
    explicit DeadlineStop(std::chrono::milliseconds timeout)
        : deadline_(now() + timeout) {}
    bool stop_requested() const noexcept override { return now() >= deadline_; }

   private:
    std::chrono::microseconds deadline_;
  };

  static DefaultThreadingProvider& threading() {
    return test_support::process_threading();
  }

  static SteadyClock& clock() {
    static SteadyClock steady;
    return steady;
  }

  static std::chrono::microseconds now() {
    return std::chrono::microseconds{
        static_cast<std::int64_t>(clock().now_microseconds())};
  }

  static void pause(std::chrono::nanoseconds duration) {
    Sleeper sleeper{threading(), clock()};
    static_cast<void>(sleeper.sleep_for(duration));
  }

  static StopToken deadline_after(std::chrono::milliseconds timeout) {
    return StopToken{std::make_shared<const DeadlineStop>(timeout)};
  }

  static WebSocketRequest request_for(const LoopbackWebSocketServer& server,
                                      std::size_t maximum) {
    WebSocketRequest request;
    request.url = libchiaking::SecretString{server.url()};
    request.headers.push_back(
        {"X-Ordered", libchiaking::SecretString{"first"}});
    request.headers.push_back(
        {"X-Ordered", libchiaking::SecretString{"second"}});
    request.subprotocol = "chiaki";
    request.connect_timeout = std::chrono::seconds(2);
    request.maximum_message_bytes = maximum;
    request.tls_configuration.proxy_policy = HttpProxyPolicy::disabled;
    return request;
  }

  CurlWebSocketProvider provider{threading(), clock()};
};

}  // namespace platform_curl_test
