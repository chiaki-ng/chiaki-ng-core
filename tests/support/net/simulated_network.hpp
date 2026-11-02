#pragma once

#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/net/types.hpp"
#include "tests/support/net/addresses.hpp"
#include "tests/support/net/ice_network.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace test_support::net {

class SimulatedNetwork {
 public:
  SimulatedNetwork(libchiaking::platform::CryptoProvider& crypto,
                   std::uint64_t seed)
      : network_(crypto, seed) {}

  std::size_t queue_limit{4096};

  auto network() noexcept -> Network& { return network_; }
  auto now() const noexcept -> std::uint64_t { return now_us_; }
  auto dropped() const noexcept -> std::size_t { return dropped_; }

  auto next_delivery_us() const noexcept -> std::optional<std::uint64_t> {
    return network_.next_delivery_us();
  }

  void advance_to(std::uint64_t now_us) {
    now_us_ = std::max(now_us_, now_us);
    std::vector<Delivery> deliveries;
    network_.deliver_due(now_us_, deliveries);
    for (auto& delivery : deliveries) {
      auto& slot = slots_[delivery.socket];
      if (!slot.open || slot.queue.size() >= queue_limit) {
        ++dropped_;
        continue;
      }
      slot.queue.push_back(std::move(delivery));
    }
  }

  auto backlog() const noexcept -> std::size_t {
    std::size_t total = 0;
    for (const auto& slot : slots_) total += slot.queue.size();
    return total;
  }

  auto add_machine() noexcept -> std::size_t { return machines_++; }

  auto bound(const libchiaking::platform::SocketEndpoint& local) const -> bool {
    return std::any_of(slots_.begin(), slots_.end(), [&](const Slot& slot) {
      return slot.open && slot.local == local;
    });
  }

  auto open(const libchiaking::platform::SocketEndpoint& local,
            std::optional<std::size_t> nat, std::size_t owner) -> std::size_t {
    const auto handle = network_.add_socket(local, nat, owner);
    if (slots_.size() <= handle) slots_.resize(handle + 1);
    slots_[handle] = Slot{true, local, owner, {}};
    return handle;
  }

  void close(std::size_t handle) noexcept {
    slots_[handle].open = false;
    slots_[handle].queue.clear();
  }

  void send(std::size_t handle,
            const libchiaking::platform::SocketEndpoint& remote,
            std::span<const std::byte> bytes) {
    network_.send(handle, remote,
                  std::vector<std::byte>{bytes.begin(), bytes.end()}, now_us_);
  }

  [[nodiscard]] auto take(std::size_t handle) -> std::optional<Delivery> {
    auto& queue = slots_[handle].queue;
    if (queue.empty()) return std::nullopt;
    auto delivery = std::move(queue.front());
    queue.pop_front();
    return delivery;
  }

  auto readable(std::size_t handle) const noexcept -> bool {
    return slots_[handle].open && !slots_[handle].queue.empty();
  }

  auto readable_for(std::size_t owner) const noexcept -> bool {
    return std::any_of(slots_.begin(), slots_.end(), [&](const Slot& slot) {
      return slot.open && slot.owner == owner && !slot.queue.empty();
    });
  }

  auto open_for(std::size_t owner) const noexcept -> std::size_t {
    return static_cast<std::size_t>(std::count_if(
        slots_.begin(), slots_.end(),
        [&](const Slot& slot) { return slot.open && slot.owner == owner; }));
  }

 private:
  struct Slot {
    bool open{};
    libchiaking::platform::SocketEndpoint local;
    std::size_t owner{};
    std::deque<Delivery> queue;
  };

  Network network_;
  std::vector<Slot> slots_;
  std::uint64_t now_us_{};
  std::size_t dropped_{};
  std::size_t machines_{};
};

class SimulatedMachine;

class SimulatedUdpSocket final : public libchiaking::platform::UdpSocket {
 public:
  SimulatedUdpSocket(SimulatedMachine& machine,
                     libchiaking::platform::IpFamily family) noexcept
      : machine_(machine), family_(family) {}

  ~SimulatedUdpSocket() override { static_cast<void>(close()); }

  auto owner() const noexcept
      -> const libchiaking::platform::NetworkProvider& override;
  [[nodiscard]] auto bind(const libchiaking::platform::SocketEndpoint& endpoint)
      -> libchiaking::Expected<void, libchiaking::platform::NetError> override;
  [[nodiscard]] auto send_to(
      std::span<const std::byte> datagram,
      const libchiaking::platform::SocketEndpoint& destination)
      -> libchiaking::Expected<std::size_t,
                               libchiaking::platform::NetError> override;
  [[nodiscard]] auto receive_from(std::span<std::byte> buffer)
      -> libchiaking::Expected<libchiaking::platform::UdpReceiveResult,
                               libchiaking::platform::NetError> override;
  [[nodiscard]] auto set_boolean_option(libchiaking::platform::SocketOption,
                                        bool)
      -> libchiaking::Expected<void, libchiaking::platform::NetError> override {
    return {};
  }
  [[nodiscard]] auto set_receive_buffer_size(std::size_t)
      -> libchiaking::Expected<void, libchiaking::platform::NetError> override {
    return {};
  }
  [[nodiscard]] auto local_endpoint()
      -> libchiaking::Expected<libchiaking::platform::SocketEndpoint,
                               libchiaking::platform::NetError> override {
    if (!handle_)
      return libchiaking::unexpected(
          libchiaking::platform::NetError::not_connected);
    return local_;
  }
  [[nodiscard]] auto close()
      -> libchiaking::Expected<void, libchiaking::platform::NetError> override;

  auto readable() const noexcept -> bool;

 private:
  SimulatedMachine& machine_;
  libchiaking::platform::IpFamily family_;
  std::optional<std::size_t> handle_;
  libchiaking::platform::SocketEndpoint local_;
  bool closed_{};
};

class SimulatedPoller final : public libchiaking::platform::Poller {
 public:
  explicit SimulatedPoller(SimulatedMachine& machine) noexcept
      : machine_(machine) {}

  auto owner() const noexcept
      -> const libchiaking::platform::NetworkProvider& override;

  [[nodiscard]] auto wake()
      -> libchiaking::Expected<void, libchiaking::platform::NetError> override {
    woken_.store(true);
    return {};
  }

  [[nodiscard]] auto wait(
      std::span<const libchiaking::platform::PollRequest> requests,
      std::chrono::nanoseconds timeout, const libchiaking::platform::StopToken&)
      -> libchiaking::Expected<libchiaking::platform::PollWaitResult,
                               libchiaking::platform::NetError> override;

 private:
  SimulatedMachine& machine_;
  std::atomic<bool> woken_{false};
};

class SimulatedMachine final : public libchiaking::platform::NetworkProvider {
 public:
  SimulatedMachine(SimulatedNetwork& network,
                   std::vector<libchiaking::platform::InterfaceInfo> interfaces,
                   std::optional<std::size_t> nat = std::nullopt)
      : interface_list(std::move(interfaces)),
        network_(network),
        id_(network.add_machine()),
        nat_(nat),
        sockets_(*this),
        pollers_(*this),
        dns_(*this),
        interfaces_(*this) {}

  std::vector<libchiaking::platform::InterfaceInfo> interface_list;
  bool fail_enumerate{};
  bool fail_bind{};
  bool fail_poller{};
  std::size_t dns_queries{};
  std::size_t waits{};
  std::size_t last_requests{};
  std::chrono::nanoseconds last_timeout{};

  auto simulation() noexcept -> SimulatedNetwork& { return network_; }
  auto id() const noexcept -> std::size_t { return id_; }
  auto nat() const noexcept -> std::optional<std::size_t> { return nat_; }
  auto open_sockets() const noexcept -> std::size_t {
    return network_.open_for(id_);
  }
  auto readable() const noexcept -> bool { return network_.readable_for(id_); }

  void add_host(std::string name,
                std::vector<libchiaking::platform::IpAddress> addresses) {
    hosts_[std::move(name)] = std::move(addresses);
  }

  auto next_port() noexcept -> std::uint16_t { return next_port_++; }

  auto has_address(const libchiaking::platform::IpAddress& address) const
      -> bool {
    return std::any_of(
        interface_list.begin(), interface_list.end(),
        [&](const auto& info) { return info.address == address; });
  }

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
    return libchiaking::platform::NetworkCapabilities{true, true};
  }

 private:
  class Sockets final : public libchiaking::platform::SocketProvider {
   public:
    explicit Sockets(SimulatedMachine& machine) noexcept : machine_(machine) {}

    [[nodiscard]] auto create_udp_socket(
        const libchiaking::platform::IpAddress& family_of)
        -> libchiaking::Expected<
            std::unique_ptr<libchiaking::platform::UdpSocket>,
            libchiaking::platform::NetError> override {
      std::unique_ptr<libchiaking::platform::UdpSocket> socket =
          std::make_unique<SimulatedUdpSocket>(machine_, family_of.family());
      return socket;
    }
    [[nodiscard]] auto create_tcp_socket(
        const libchiaking::platform::IpAddress&)
        -> libchiaking::Expected<
            std::unique_ptr<libchiaking::platform::TcpSocket>,
            libchiaking::platform::NetError> override {
      return libchiaking::unexpected(
          libchiaking::platform::NetError::not_supported);
    }
    [[nodiscard]] auto create_tcp_listener(
        const libchiaking::platform::IpAddress&)
        -> libchiaking::Expected<
            std::unique_ptr<libchiaking::platform::TcpListener>,
            libchiaking::platform::NetError> override {
      return libchiaking::unexpected(
          libchiaking::platform::NetError::not_supported);
    }

   private:
    SimulatedMachine& machine_;
  };

  class Pollers final : public libchiaking::platform::PollerProvider {
   public:
    explicit Pollers(SimulatedMachine& machine) noexcept : machine_(machine) {}

    [[nodiscard]] auto create_poller()
        -> libchiaking::Expected<std::unique_ptr<libchiaking::platform::Poller>,
                                 libchiaking::platform::NetError> override {
      if (machine_.fail_poller)
        return libchiaking::unexpected(
            libchiaking::platform::NetError::system_failure);
      std::unique_ptr<libchiaking::platform::Poller> poller =
          std::make_unique<SimulatedPoller>(machine_);
      return poller;
    }

   private:
    SimulatedMachine& machine_;
  };

  class Dns final : public libchiaking::platform::DnsProvider {
   public:
    explicit Dns(SimulatedMachine& machine) noexcept : machine_(machine) {}

    using libchiaking::platform::DnsProvider::resolve;

    [[nodiscard]] auto resolve(std::string_view host, std::string_view)
        -> libchiaking::Expected<std::vector<libchiaking::platform::IpAddress>,
                                 libchiaking::platform::NetError> override {
      ++machine_.dns_queries;
      const auto found = machine_.hosts_.find(std::string{host});
      if (found == machine_.hosts_.end())
        return libchiaking::unexpected(
            libchiaking::platform::NetError::host_unreachable);
      return found->second;
    }

   private:
    SimulatedMachine& machine_;
  };

  class Interfaces final : public libchiaking::platform::InterfacesProvider {
   public:
    explicit Interfaces(SimulatedMachine& machine) noexcept
        : machine_(machine) {}

    [[nodiscard]] auto enumerate() -> libchiaking::Expected<
        std::vector<libchiaking::platform::InterfaceInfo>,
        libchiaking::platform::NetError> override {
      if (machine_.fail_enumerate)
        return libchiaking::unexpected(
            libchiaking::platform::NetError::system_failure);
      return machine_.interface_list;
    }

   private:
    SimulatedMachine& machine_;
  };

  SimulatedNetwork& network_;
  std::size_t id_;
  std::optional<std::size_t> nat_;
  Sockets sockets_;
  Pollers pollers_;
  Dns dns_;
  Interfaces interfaces_;
  std::map<std::string, std::vector<libchiaking::platform::IpAddress>> hosts_;
  std::uint16_t next_port_{50000};
};

inline auto SimulatedUdpSocket::owner() const noexcept
    -> const libchiaking::platform::NetworkProvider& {
  return machine_;
}

inline auto SimulatedUdpSocket::bind(
    const libchiaking::platform::SocketEndpoint& endpoint)
    -> libchiaking::Expected<void, libchiaking::platform::NetError> {
  using libchiaking::platform::NetError;
  if (closed_) return libchiaking::unexpected(NetError::closed);
  if (handle_ || endpoint.address.family() != family_)
    return libchiaking::unexpected(NetError::invalid_argument);
  if (machine_.fail_bind)
    return libchiaking::unexpected(NetError::address_in_use);
  if (!machine_.has_address(endpoint.address))
    return libchiaking::unexpected(NetError::address_not_available);
  auto local = endpoint;
  if (local.port == 0) local.port = machine_.next_port();
  if (machine_.simulation().bound(local))
    return libchiaking::unexpected(NetError::address_in_use);
  local_ = local;
  handle_ = machine_.simulation().open(local, machine_.nat(), machine_.id());
  return {};
}

inline auto SimulatedUdpSocket::send_to(
    std::span<const std::byte> datagram,
    const libchiaking::platform::SocketEndpoint& destination)
    -> libchiaking::Expected<std::size_t, libchiaking::platform::NetError> {
  using libchiaking::platform::NetError;
  if (closed_) return libchiaking::unexpected(NetError::closed);
  if (!handle_ || destination.address.family() != family_ ||
      destination.port == 0)
    return libchiaking::unexpected(NetError::invalid_argument);
  if (datagram.size() > 65507)
    return libchiaking::unexpected(NetError::message_too_large);
  machine_.simulation().send(*handle_, destination, datagram);
  return datagram.size();
}

inline auto SimulatedUdpSocket::receive_from(std::span<std::byte> buffer)
    -> libchiaking::Expected<libchiaking::platform::UdpReceiveResult,
                             libchiaking::platform::NetError> {
  using libchiaking::platform::NetError;
  if (closed_) return libchiaking::unexpected(NetError::closed);
  if (!handle_) return libchiaking::unexpected(NetError::would_block);
  auto delivery = machine_.simulation().take(*handle_);
  if (!delivery) return libchiaking::unexpected(NetError::would_block);
  const auto size = std::min(buffer.size(), delivery->bytes.size());
  std::copy_n(delivery->bytes.begin(), size, buffer.begin());
  return libchiaking::platform::UdpReceiveResult{size, delivery->source,
                                                 std::nullopt};
}

inline auto SimulatedUdpSocket::close()
    -> libchiaking::Expected<void, libchiaking::platform::NetError> {
  if (closed_) return {};
  closed_ = true;
  if (handle_) machine_.simulation().close(*handle_);
  return {};
}

inline auto SimulatedUdpSocket::readable() const noexcept -> bool {
  return !closed_ && handle_ && machine_.simulation().readable(*handle_);
}

inline auto SimulatedPoller::owner() const noexcept
    -> const libchiaking::platform::NetworkProvider& {
  return machine_;
}

inline auto SimulatedPoller::wait(
    std::span<const libchiaking::platform::PollRequest> requests,
    std::chrono::nanoseconds timeout, const libchiaking::platform::StopToken&)
    -> libchiaking::Expected<libchiaking::platform::PollWaitResult,
                             libchiaking::platform::NetError> {
  ++machine_.waits;
  machine_.last_requests = requests.size();
  machine_.last_timeout = timeout;
  libchiaking::platform::PollWaitResult result;
  for (const auto& request : requests) {
    const auto* socket =
        dynamic_cast<const SimulatedUdpSocket*>(request.pollable);
    if (socket == nullptr || &socket->owner() != &machine_)
      return libchiaking::unexpected(
          libchiaking::platform::NetError::wrong_provider);
    if ((request.interest & libchiaking::platform::PollEvent::readable) !=
            libchiaking::platform::PollEvent::none &&
        socket->readable())
      result.ready.push_back(libchiaking::platform::PollReadiness{
          request.tag, libchiaking::platform::PollEvent::readable});
  }
  result.woke = woken_.exchange(false);
  return result;
}

}  // namespace test_support::net
