#pragma once

#include <libchiaking/platform/net/default.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include "tests/support/process_threading.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace platform_curl_test {

using namespace libchiaking::platform;

class SilentHttpServer {
 public:
  SilentHttpServer() {
    auto listener =
        network_.sockets().create_tcp_listener(IpAddress::loopback_ipv4());
    if (!listener) return;
    listener_ = std::move(listener).value();
    if (!listener_->bind({IpAddress::loopback_ipv4(), 0}) ||
        !listener_->listen(16))
      return;
    const auto endpoint = listener_->local_endpoint();
    if (endpoint) port_ = endpoint.value().port;
  }

  SilentHttpServer(const SilentHttpServer&) = delete;
  SilentHttpServer& operator=(const SilentHttpServer&) = delete;

  ~SilentHttpServer() {
    if (listener_) (void)listener_->close();
  }

  bool ready() const noexcept { return port_ != 0; }

  std::string url(std::string_view path) const {
    return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
  }

 private:
  SteadyClock clock_;
  DefaultNetworkProvider network_{test_support::process_threading(), clock_};
  std::unique_ptr<TcpListener> listener_;
  std::uint16_t port_{0};
};

}  // namespace platform_curl_test
