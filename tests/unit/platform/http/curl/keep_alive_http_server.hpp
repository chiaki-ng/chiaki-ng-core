#pragma once

#include <libchiaking/platform/net/default.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace platform_curl_test {

using namespace libchiaking::platform;

class KeepAliveHttpServer {
 public:
  explicit KeepAliveHttpServer(std::size_t request_count)
      : request_count_(request_count) {
    auto listener =
        network_.sockets().create_tcp_listener(IpAddress::loopback_ipv4());
    if (!listener) return;
    listener_ = std::move(listener).value();
    if (!listener_->bind({IpAddress::loopback_ipv4(), 0}) ||
        !listener_->listen(8))
      return;
    const auto endpoint = listener_->local_endpoint();
    if (!endpoint) return;
    port_ = endpoint.value().port;
    auto started = Thread::start(threading_, "keep-alive server",
                                 ThreadRole::unspecified, [this] { run(); });
    if (!started) return;
    thread_ = std::move(started).value();
    ready_ = true;
  }

  ~KeepAliveHttpServer() {
    join();
    if (listener_) (void)listener_->close();
  }

  KeepAliveHttpServer(const KeepAliveHttpServer&) = delete;
  KeepAliveHttpServer& operator=(const KeepAliveHttpServer&) = delete;

  bool ready() const noexcept { return ready_; }
  std::size_t accepted() const noexcept { return accepted_; }
  const std::vector<std::string>& requests() const noexcept {
    return requests_;
  }

  std::uint16_t port() const noexcept { return port_; }

  std::string url(std::string_view path) const {
    return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
  }

  void join() {
    if (thread_.joinable()) static_cast<void>(thread_.join());
  }

 private:
  struct Connection {
    std::unique_ptr<TcpSocket> socket;
    std::string pending;
  };

  [[nodiscard]] static bool send_all(Poller& poller, TcpSocket& socket,
                                     std::string_view payload) {
    const auto* bytes = reinterpret_cast<const std::byte*>(payload.data());
    std::size_t offset = 0;
    while (offset < payload.size()) {
      const std::array<PollRequest, 1> requests{{PollRequest{
          .pollable = &socket, .tag = 1, .interest = PollEvent::writable}}};
      if (!poller.wait(requests, std::chrono::seconds(5), {})) return false;
      const auto sent = socket.send(
          std::span<const std::byte>(bytes + offset, payload.size() - offset));
      if (!sent) {
        if (sent.error() == NetError::would_block) continue;
        return false;
      }
      offset += sent.value();
    }
    return true;
  }

  [[nodiscard]] bool serve_pending(Poller& poller, Connection& connection) {
    const std::size_t header_end = connection.pending.find("\r\n\r\n");
    if (header_end == std::string::npos) return true;
    requests_.push_back(connection.pending.substr(0, header_end + 4));
    connection.pending.erase(0, header_end + 4);
    const std::string body = std::to_string(requests_.size());
    const std::string response =
        "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\n\r\n" + body;
    return send_all(poller, *connection.socket, response);
  }

  void run() noexcept {
    auto poller = network_.pollers().create_poller();
    if (!poller) return;
    std::vector<Connection> connections;
    std::array<std::byte, 4096> buffer{};
    while (requests_.size() < request_count_) {
      std::vector<PollRequest> interest{
          PollRequest{.pollable = listener_.get(),
                      .tag = 0,
                      .interest = PollEvent::readable}};
      for (std::size_t index = 0; index < connections.size(); ++index) {
        interest.push_back(
            PollRequest{.pollable = connections[index].socket.get(),
                        .tag = index + 1,
                        .interest = PollEvent::readable});
      }
      const auto result =
          poller.value()->wait(interest, std::chrono::seconds(5), {});
      if (!result || result.value().ready.empty()) return;
      for (const auto& ready : result.value().ready) {
        if ((ready.ready & PollEvent::readable) == PollEvent::none) continue;
        if (ready.tag == 0) {
          auto accepted = listener_->accept();
          if (!accepted) {
            if (accepted.error() == NetError::would_block) continue;
            return;
          }
          ++accepted_;
          connections.push_back(Connection{std::move(accepted).value(), {}});
          continue;
        }
        auto& connection = connections[ready.tag - 1];
        if (!connection.socket) continue;
        const auto received = connection.socket->receive(buffer);
        if (!received) {
          if (received.error() == NetError::would_block) continue;
          (void)connection.socket->close();
          connection.socket.reset();
          continue;
        }
        if (received.value() == 0) {
          (void)connection.socket->close();
          connection.socket.reset();
          continue;
        }
        connection.pending.append(reinterpret_cast<const char*>(buffer.data()),
                                  received.value());
        if (!serve_pending(*poller.value(), connection)) return;
      }
      std::erase_if(connections, [](const Connection& connection) {
        return !connection.socket;
      });
    }
    for (auto& connection : connections) (void)connection.socket->close();
  }

  DefaultThreadingProvider threading_;
  SteadyClock clock_;
  DefaultNetworkProvider network_{threading_, clock_};
  std::unique_ptr<TcpListener> listener_;
  std::vector<std::string> requests_;
  std::size_t request_count_;
  std::size_t accepted_{0};
  std::uint16_t port_{0};
  Thread thread_;
  bool ready_{false};
};

}  // namespace platform_curl_test
