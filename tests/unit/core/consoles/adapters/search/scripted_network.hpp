#pragma once

#include <libchiaking/platform/net/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/testing/in_memory_network.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace consoles_test {

class ScriptedClock final : public libchiaking::platform::ClockProvider {
 public:
  std::uint64_t now_microseconds() const noexcept override {
    const auto reading = reads_.fetch_add(1) + 1;
    if (on_read) on_read(reading);
    return now.load();
  }

  mutable std::atomic<std::uint64_t> now{0};
  std::function<void(std::uint64_t)> on_read;

 private:
  mutable std::atomic<std::uint64_t> reads_{0};
};

class ScriptedNetwork final : public libchiaking::platform::NetworkProvider {
 public:
  using NetError = libchiaking::platform::NetError;
  using SocketEndpoint = libchiaking::platform::SocketEndpoint;

  ScriptedNetwork() = default;
  ScriptedNetwork(const ScriptedNetwork&) = delete;
  auto operator=(const ScriptedNetwork&) -> ScriptedNetwork& = delete;

  libchiaking::platform::DefaultThreadingProvider threading;
  libchiaking::platform::SteadyClock steady;
  libchiaking::testing::InMemoryNetwork memory{threading, steady};

  unsigned interface_calls{};
  bool interfaces_fail{};

  unsigned sockets_created{};
  std::vector<SocketEndpoint> sent;
  std::function<void()> on_send;
  std::optional<NetError> send_error;
  std::function<bool(const SocketEndpoint&)> unreachable;
  std::vector<SocketEndpoint> refused_sends;
  std::vector<std::uint16_t> bind_attempts;
  std::uint16_t refuse_binds_through{};
  std::vector<libchiaking::platform::SocketOption> options;
  bool refuse_broadcast{};
  bool unicast_only{};

  std::vector<std::string> dns_queries;
  std::optional<std::vector<libchiaking::platform::IpAddress>> dns_answer;
  unsigned dns_failures{};

  auto sockets() noexcept -> libchiaking::platform::SocketProvider& override {
    return sockets_;
  }
  auto pollers() noexcept -> libchiaking::platform::PollerProvider& override {
    return pollers_;
  }
  auto dns() noexcept -> libchiaking::platform::DnsProvider& override {
    return dns_;
  }
  auto interfaces() noexcept
      -> libchiaking::platform::InterfacesProvider& override {
    return interfaces_;
  }
  auto capabilities() const noexcept
      -> libchiaking::platform::NetworkCapabilities override {
    auto capabilities = memory.capabilities();
    capabilities.broadcast = !unicast_only;
    return capabilities;
  }

 private:
  class Socket final : public libchiaking::platform::UdpSocket {
   public:
    Socket(ScriptedNetwork& script,
           std::unique_ptr<libchiaking::platform::UdpSocket> inner) noexcept
        : script_(script), inner_(std::move(inner)) {}

    auto inner() const noexcept -> const libchiaking::platform::UdpSocket* {
      return inner_.get();
    }

    auto owner() const noexcept
        -> const libchiaking::platform::NetworkProvider& override {
      return inner_->owner();
    }

    [[nodiscard]] auto bind(const SocketEndpoint& endpoint)
        -> libchiaking::Expected<void, NetError> override {
      script_.bind_attempts.push_back(endpoint.port);
      if (endpoint.port != 0 && endpoint.port <= script_.refuse_binds_through)
        return libchiaking::unexpected(NetError::permission_denied);
      return inner_->bind(endpoint);
    }

    [[nodiscard]] auto send_to(std::span<const std::byte> datagram,
                               const SocketEndpoint& destination)
        -> libchiaking::Expected<std::size_t, NetError> override {
      script_.sent.push_back(destination);
      if (script_.on_send) {
        const auto callback = std::exchange(script_.on_send, {});
        callback();
      }
      if (script_.send_error)
        return libchiaking::unexpected(*script_.send_error);
      if (script_.unreachable && script_.unreachable(destination)) {
        script_.refused_sends.push_back(destination);
        return libchiaking::unexpected(NetError::network_unreachable);
      }
      return inner_->send_to(datagram, destination);
    }

    [[nodiscard]] auto receive_from(std::span<std::byte> buffer)
        -> libchiaking::Expected<libchiaking::platform::UdpReceiveResult,
                                 NetError> override {
      return inner_->receive_from(buffer);
    }

    [[nodiscard]] auto set_boolean_option(
        libchiaking::platform::SocketOption option, bool enabled)
        -> libchiaking::Expected<void, NetError> override {
      script_.options.push_back(option);
      if (script_.refuse_broadcast &&
          option == libchiaking::platform::SocketOption::broadcast)
        return libchiaking::unexpected(NetError::permission_denied);
      return inner_->set_boolean_option(option, enabled);
    }

    [[nodiscard]] auto set_receive_buffer_size(std::size_t bytes)
        -> libchiaking::Expected<void, NetError> override {
      return inner_->set_receive_buffer_size(bytes);
    }

    [[nodiscard]] auto local_endpoint()
        -> libchiaking::Expected<SocketEndpoint, NetError> override {
      return inner_->local_endpoint();
    }

    [[nodiscard]] auto close()
        -> libchiaking::Expected<void, NetError> override {
      return inner_->close();
    }

   private:
    ScriptedNetwork& script_;
    std::unique_ptr<libchiaking::platform::UdpSocket> inner_;
  };

  class Sockets final : public libchiaking::platform::SocketProvider {
   public:
    explicit Sockets(ScriptedNetwork& script) noexcept : script_(script) {}

    [[nodiscard]] auto create_udp_socket(
        const libchiaking::platform::IpAddress& family_of)
        -> libchiaking::Expected<
            std::unique_ptr<libchiaking::platform::UdpSocket>,
            NetError> override {
      auto inner = script_.memory.sockets().create_udp_socket(family_of);
      if (!inner) return libchiaking::unexpected(inner.error());
      ++script_.sockets_created;
      return std::unique_ptr<libchiaking::platform::UdpSocket>(
          std::make_unique<Socket>(script_, std::move(inner.value())));
    }

    [[nodiscard]] auto create_tcp_socket(
        const libchiaking::platform::IpAddress& family_of)
        -> libchiaking::Expected<
            std::unique_ptr<libchiaking::platform::TcpSocket>,
            NetError> override {
      return script_.memory.sockets().create_tcp_socket(family_of);
    }

    [[nodiscard]] auto create_tcp_listener(
        const libchiaking::platform::IpAddress& family_of)
        -> libchiaking::Expected<
            std::unique_ptr<libchiaking::platform::TcpListener>,
            NetError> override {
      return script_.memory.sockets().create_tcp_listener(family_of);
    }

   private:
    ScriptedNetwork& script_;
  };

  class Poller final : public libchiaking::platform::Poller {
   public:
    Poller(ScriptedNetwork& script,
           std::unique_ptr<libchiaking::platform::Poller> inner) noexcept
        : script_(script), inner_(std::move(inner)) {}

    auto owner() const noexcept
        -> const libchiaking::platform::NetworkProvider& override {
      return script_;
    }

    [[nodiscard]] auto wake()
        -> libchiaking::Expected<void, NetError> override {
      return inner_->wake();
    }

    [[nodiscard]] auto wait(
        std::span<const libchiaking::platform::PollRequest> requests,
        std::chrono::nanoseconds timeout,
        const libchiaking::platform::StopToken& stop)
        -> libchiaking::Expected<libchiaking::platform::PollWaitResult,
                                 NetError> override {
      std::vector<libchiaking::platform::PollRequest> unwrapped{
          requests.begin(), requests.end()};
      for (auto& request : unwrapped) {
        if (const auto* scripted =
                dynamic_cast<const Socket*>(request.pollable))
          request.pollable = scripted->inner();
      }
      return inner_->wait(unwrapped, timeout, stop);
    }

   private:
    ScriptedNetwork& script_;
    std::unique_ptr<libchiaking::platform::Poller> inner_;
  };

  class Pollers final : public libchiaking::platform::PollerProvider {
   public:
    explicit Pollers(ScriptedNetwork& script) noexcept : script_(script) {}

    [[nodiscard]] auto create_poller()
        -> libchiaking::Expected<std::unique_ptr<libchiaking::platform::Poller>,
                                 NetError> override {
      auto inner = script_.memory.pollers().create_poller();
      if (!inner) return libchiaking::unexpected(inner.error());
      return std::unique_ptr<libchiaking::platform::Poller>(
          std::make_unique<Poller>(script_, std::move(inner.value())));
    }

   private:
    ScriptedNetwork& script_;
  };

  class Dns final : public libchiaking::platform::DnsProvider {
   public:
    explicit Dns(ScriptedNetwork& script) noexcept : script_(script) {}

    using libchiaking::platform::DnsProvider::resolve;

    [[nodiscard]] auto resolve(std::string_view host, std::string_view service)
        -> libchiaking::Expected<std::vector<libchiaking::platform::IpAddress>,
                                 NetError> override {
      script_.dns_queries.emplace_back(host);
      if (script_.dns_failures > 0) {
        --script_.dns_failures;
        return libchiaking::unexpected(NetError::host_unreachable);
      }
      if (script_.dns_answer) return *script_.dns_answer;
      return script_.memory.dns().resolve(host, service);
    }

   private:
    ScriptedNetwork& script_;
  };

  class Interfaces final : public libchiaking::platform::InterfacesProvider {
   public:
    explicit Interfaces(ScriptedNetwork& script) noexcept : script_(script) {}

    [[nodiscard]] auto enumerate() -> libchiaking::Expected<
        std::vector<libchiaking::platform::InterfaceInfo>, NetError> override {
      ++script_.interface_calls;
      if (script_.interfaces_fail)
        return libchiaking::unexpected(NetError::system_failure);
      return script_.memory.interface_list;
    }

   private:
    ScriptedNetwork& script_;
  };

  Sockets sockets_{*this};
  Pollers pollers_{*this};
  Dns dns_{*this};
  Interfaces interfaces_{*this};
};

}  // namespace consoles_test
