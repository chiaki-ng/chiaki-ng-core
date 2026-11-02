#pragma once

#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/ntp/packet.hpp"
#include "src/core/net/ntp/timestamp.hpp"
#include "src/core/net/ntp/trusted_time.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace test_support::net {

namespace platform = libchiaking::platform;
namespace ntp = libchiaking::net::ntp;

using Time = ntp::Timestamp::Time;
using Reply = ntp::Packet::Datagram;

class Monotonic final : public platform::ClockProvider {
 public:
  auto now_microseconds() const noexcept -> std::uint64_t override {
    return now_us.load();
  }

  void advance(std::chrono::microseconds by) noexcept {
    now_us += static_cast<std::uint64_t>(by.count());
  }

  std::atomic<std::uint64_t> now_us{1'000'000};
};

struct Server {
  std::chrono::microseconds skew{};
  std::chrono::microseconds round_trip{std::chrono::milliseconds{20}};
  std::chrono::microseconds held{std::chrono::microseconds{50}};
  bool silent{};
  bool forges_origin{};
  std::function<void(Reply&)> tamper{};
};

class ScriptedTimeServers final : public platform::NetworkProvider {
 public:
  static constexpr Time true_start{std::chrono::seconds{1'900'000'000}};

  static auto v4(std::uint8_t last) -> platform::IpAddress {
    return platform::IpAddress::ipv4({192, 0, 2, last});
  }

  static auto v6(std::uint8_t last) -> platform::IpAddress {
    return platform::IpAddress::ipv6(
        {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, last});
  }

  Monotonic clock;

  void resolves(std::string host, std::vector<platform::IpAddress> addresses) {
    const platform::Lock lock{mutex_};
    hosts_[std::move(host)] = std::move(addresses);
  }

  void serves(const platform::IpAddress& address, Server server) {
    const platform::Lock lock{mutex_};
    servers_.insert_or_assign(key(address), std::move(server));
  }

  auto true_now() const -> Time {
    return true_start + std::chrono::microseconds{static_cast<std::int64_t>(
                            clock.now_microseconds())};
  }

  auto asked(const platform::IpAddress& address) const -> int {
    const platform::Lock lock{mutex_};
    const auto found = asked_.find(key(address));
    return found == asked_.end() ? 0 : found->second;
  }

  auto queries() const -> int {
    const platform::Lock lock{mutex_};
    int total = 0;
    for (const auto& [address, count] : asked_) total += count;
    return total;
  }

  auto lookups(std::string_view host) const -> int {
    const platform::Lock lock{mutex_};
    const auto found = lookups_.find(std::string{host});
    return found == lookups_.end() ? 0 : found->second;
  }

  auto sockets() noexcept -> platform::SocketProvider& override {
    return sockets_;
  }
  auto pollers() noexcept -> platform::PollerProvider& override {
    return pollers_;
  }
  auto dns() noexcept -> platform::DnsProvider& override { return dns_; }
  auto interfaces() noexcept -> platform::InterfacesProvider& override {
    return interfaces_;
  }
  auto capabilities() const noexcept -> platform::NetworkCapabilities override {
    return {};
  }

 private:
  using NetResult = libchiaking::Expected<void, platform::NetError>;

  static auto key(const platform::IpAddress& address) -> std::string {
    return platform::to_string(address);
  }

  class Socket final : public platform::UdpSocket {
   public:
    explicit Socket(ScriptedTimeServers& network) noexcept
        : network_(network) {}

    auto owner() const noexcept -> const platform::NetworkProvider& override {
      return network_;
    }
    [[nodiscard]] auto bind(const platform::SocketEndpoint& endpoint)
        -> NetResult override {
      local_ = endpoint;
      local_.port = 40'123;
      return {};
    }
    [[nodiscard]] auto send_to(std::span<const std::byte> datagram,
                               const platform::SocketEndpoint& destination)
        -> libchiaking::Expected<std::size_t, platform::NetError> override {
      auto reply = network_.answer(datagram, destination);
      if (reply) queue_.push_back({destination, *reply});
      return datagram.size();
    }
    [[nodiscard]] auto receive_from(std::span<std::byte> buffer)
        -> libchiaking::Expected<platform::UdpReceiveResult,
                                 platform::NetError> override {
      if (queue_.empty())
        return libchiaking::unexpected(platform::NetError::would_block);
      const auto [source, bytes] = queue_.front();
      queue_.pop_front();
      std::ranges::copy(bytes, buffer.begin());
      return platform::UdpReceiveResult{bytes.size(), source, std::nullopt};
    }
    [[nodiscard]] auto set_boolean_option(platform::SocketOption, bool)
        -> NetResult override {
      return {};
    }
    [[nodiscard]] auto set_receive_buffer_size(std::size_t)
        -> NetResult override {
      return {};
    }
    [[nodiscard]] auto local_endpoint()
        -> libchiaking::Expected<platform::SocketEndpoint,
                                 platform::NetError> override {
      return local_;
    }
    [[nodiscard]] auto close() -> NetResult override { return {}; }

    auto has_reply() const noexcept -> bool { return !queue_.empty(); }

   private:
    ScriptedTimeServers& network_;
    platform::SocketEndpoint local_{};
    std::deque<std::pair<platform::SocketEndpoint, Reply>> queue_;
  };

  class Sockets final : public platform::SocketProvider {
   public:
    explicit Sockets(ScriptedTimeServers& network) noexcept
        : network_(network) {}
    [[nodiscard]] auto create_udp_socket(const platform::IpAddress&)
        -> libchiaking::Expected<std::unique_ptr<platform::UdpSocket>,
                                 platform::NetError> override {
      return std::unique_ptr<platform::UdpSocket>{
          std::make_unique<Socket>(network_)};
    }
    [[nodiscard]] auto create_tcp_socket(const platform::IpAddress&)
        -> libchiaking::Expected<std::unique_ptr<platform::TcpSocket>,
                                 platform::NetError> override {
      return libchiaking::unexpected(platform::NetError::not_supported);
    }
    [[nodiscard]] auto create_tcp_listener(const platform::IpAddress&)
        -> libchiaking::Expected<std::unique_ptr<platform::TcpListener>,
                                 platform::NetError> override {
      return libchiaking::unexpected(platform::NetError::not_supported);
    }

   private:
    ScriptedTimeServers& network_;
  };

  class Poller final : public platform::Poller {
   public:
    explicit Poller(ScriptedTimeServers& network) noexcept
        : network_(network) {}
    auto owner() const noexcept -> const platform::NetworkProvider& override {
      return network_;
    }
    [[nodiscard]] auto wake() -> NetResult override { return {}; }
    [[nodiscard]] auto wait(std::span<const platform::PollRequest> requests,
                            std::chrono::nanoseconds timeout,
                            const platform::StopToken&)
        -> libchiaking::Expected<platform::PollWaitResult,
                                 platform::NetError> override {
      platform::PollWaitResult result;
      for (const auto& request : requests) {
        const auto* socket = dynamic_cast<const Socket*>(request.pollable);
        if (socket != nullptr && socket->has_reply())
          result.ready.push_back({request.tag, platform::PollEvent::readable});
      }
      if (result.ready.empty())
        network_.clock.advance(
            std::chrono::duration_cast<std::chrono::microseconds>(timeout));
      result.woke = !result.ready.empty();
      return result;
    }

   private:
    ScriptedTimeServers& network_;
  };

  class Pollers final : public platform::PollerProvider {
   public:
    explicit Pollers(ScriptedTimeServers& network) noexcept
        : network_(network) {}
    [[nodiscard]] auto create_poller()
        -> libchiaking::Expected<std::unique_ptr<platform::Poller>,
                                 platform::NetError> override {
      return std::unique_ptr<platform::Poller>{
          std::make_unique<Poller>(network_)};
    }

   private:
    ScriptedTimeServers& network_;
  };

  class Dns final : public platform::DnsProvider {
   public:
    explicit Dns(ScriptedTimeServers& network) noexcept : network_(network) {}
    using platform::DnsProvider::resolve;
    [[nodiscard]] auto resolve(std::string_view host, std::string_view service)
        -> libchiaking::Expected<std::vector<platform::IpAddress>,
                                 platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      ++network_.lookups_[std::string{host}];
      if (service != "123")
        return libchiaking::unexpected(platform::NetError::invalid_argument);
      const auto found = network_.hosts_.find(std::string{host});
      if (found == network_.hosts_.end())
        return libchiaking::unexpected(platform::NetError::host_unreachable);
      return found->second;
    }

   private:
    ScriptedTimeServers& network_;
  };

  class Interfaces final : public platform::InterfacesProvider {
   public:
    [[nodiscard]] auto enumerate()
        -> libchiaking::Expected<std::vector<platform::InterfaceInfo>,
                                 platform::NetError> override {
      return std::vector<platform::InterfaceInfo>{};
    }
  };

  auto answer(std::span<const std::byte> request,
              const platform::SocketEndpoint& to) -> std::optional<Reply> {
    std::optional<Server> server;
    {
      const platform::Lock lock{mutex_};
      ++asked_[key(to.address)];
      const auto found = servers_.find(key(to.address));
      if (found != servers_.end()) server = found->second;
    }
    if (!server || server->silent || to.port != 123 ||
        request.size() != ntp::Packet::size)
      return std::nullopt;
    const auto received = true_now() + server->round_trip / 2 + server->skew;
    const auto sent = received + server->held;
    clock.advance(server->round_trip + server->held);
    Reply reply{};
    reply[0] = std::byte{0x24};
    reply[1] = std::byte{2};
    reply[10] = std::byte{0x01};
    const auto origin = server->forges_origin
                            ? std::span<const std::byte>{reply}.subspan(0, 8)
                            : request.subspan(40, 8);
    std::ranges::copy(origin, reply.begin() + 24);
    write(reply, 32, ntp::Timestamp::at(received));
    write(reply, 40, ntp::Timestamp::at(sent));
    if (server->tamper) server->tamper(reply);
    return reply;
  }

  static void write(Reply& reply, std::size_t at, ntp::Timestamp stamp) {
    for (std::size_t i = 0; i < 8; ++i)
      reply[at + i] = static_cast<std::byte>(stamp.wire() >> (56U - 8U * i));
  }

  platform::DefaultThreadingProvider threading_;
  mutable platform::Mutex mutex_{threading_};
  std::map<std::string, std::vector<platform::IpAddress>> hosts_;
  std::map<std::string, Server> servers_;
  std::map<std::string, int> asked_;
  std::map<std::string, int> lookups_;
  Sockets sockets_{*this};
  Pollers pollers_{*this};
  Dns dns_{*this};
  Interfaces interfaces_;
};

}  // namespace test_support::net
