#pragma once

#include <libchiaking/platform/net/default.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include "tests/support/process_threading.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace platform_curl_test {

using namespace libchiaking::platform;

class LoopbackHttpServer {
 public:
  explicit LoopbackHttpServer(std::string response)
      : response_(std::move(response)) {
    auto listener =
        network_.sockets().create_tcp_listener(IpAddress::loopback_ipv4());
    if (!listener) return;
    listener_ = std::move(listener).value();
    if (!listener_->bind({IpAddress::loopback_ipv4(), 0}) ||
        !listener_->listen(1))
      return;
    const auto endpoint = listener_->local_endpoint();
    if (!endpoint) return;
    port_ = endpoint.value().port;
    auto started = Thread::start(threading_, "loopback server",
                                 ThreadRole::unspecified, [this] { run(); });
    if (!started) return;
    thread_ = std::move(started).value();
    ready_ = true;
  }

  ~LoopbackHttpServer() {
    join();
    if (listener_) (void)listener_->close();
  }

  LoopbackHttpServer(const LoopbackHttpServer&) = delete;
  LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

  bool ready() const noexcept { return ready_; }
  bool succeeded() const noexcept { return succeeded_; }
  const std::string& request() const noexcept { return request_; }

  std::string url(std::string_view path) const {
    return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
  }

  void join() {
    if (thread_.joinable()) static_cast<void>(thread_.join());
  }

 private:
  [[nodiscard]] static bool wait_for(Poller& poller, const Pollable& pollable,
                                     PollEvent event) {
    const std::array<PollRequest, 1> requests{{PollRequest{
        .pollable = &pollable,
        .tag = 1,
        .interest = event,
    }}};
    const auto result = poller.wait(requests, std::chrono::seconds(5), {});
    if (!result) return false;
    for (const auto& ready : result.value().ready) {
      if ((ready.ready & event) != PollEvent::none) return true;
    }
    return false;
  }

  static std::size_t content_length(std::string_view request) {
    constexpr std::string_view field = "Content-Length:";
    const std::size_t position = request.find(field);
    if (position == std::string_view::npos) return 0;
    std::size_t cursor = position + field.size();
    while (cursor < request.size() && request[cursor] == ' ') ++cursor;
    std::size_t value = 0;
    while (cursor < request.size() && request[cursor] >= '0' &&
           request[cursor] <= '9') {
      value = value * 10 + static_cast<std::size_t>(request[cursor] - '0');
      ++cursor;
    }
    return value;
  }

  [[nodiscard]] bool receive_request(Poller& poller, TcpSocket& socket) {
    std::array<std::byte, 4096> buffer{};
    for (;;) {
      if (!wait_for(poller, socket, PollEvent::readable)) return false;
      const auto received = socket.receive(buffer);
      if (!received) {
        if (received.error() == NetError::would_block) continue;
        return false;
      }
      if (received.value() == 0) return false;
      request_.append(reinterpret_cast<const char*>(buffer.data()),
                      received.value());
      const std::size_t header_end = request_.find("\r\n\r\n");
      if (header_end != std::string::npos &&
          request_.size() >= header_end + 4 + content_length(request_))
        return true;
    }
  }

  [[nodiscard]] bool send_response(Poller& poller, TcpSocket& socket) {
    const auto* bytes = reinterpret_cast<const std::byte*>(response_.data());
    std::size_t offset = 0;
    while (offset < response_.size()) {
      if (!wait_for(poller, socket, PollEvent::writable)) return false;
      const auto sent = socket.send(std::span<const std::byte>(
          bytes + offset, response_.size() - offset));
      if (!sent) {
        if (sent.error() == NetError::would_block) continue;
        return false;
      }
      if (sent.value() == 0) return false;
      offset += sent.value();
    }
    return true;
  }

  void run() noexcept {
    auto poller = network_.pollers().create_poller();
    if (!poller || !wait_for(*poller.value(), *listener_, PollEvent::readable))
      return;

    auto accepted = listener_->accept();
    if (!accepted) return;
    auto socket = std::move(accepted).value();
    succeeded_ = receive_request(*poller.value(), *socket) &&
                 send_response(*poller.value(), *socket);
    (void)socket->close();
  }

  DefaultThreadingProvider& threading_ = test_support::process_threading();
  SteadyClock clock_;
  DefaultNetworkProvider network_{threading_, clock_};
  std::unique_ptr<TcpListener> listener_;
  std::string response_;
  std::string request_;
  std::uint16_t port_{0};
  Thread thread_;
  bool ready_{false};
  bool succeeded_{false};
};

}  // namespace platform_curl_test
