#pragma once

#include "libchiaking/platform/default_factory.hpp"
#include "src/core/net/nat/holepunch/holepunch_probe.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <libchiaking/platform/factory.hpp>
#include <memory>
#include <span>
#include <utility>

namespace play_test::holepunch_probe {

namespace pf = libchiaking::platform;

class QuietUdp : public pf::UdpSocket {
 public:
  explicit QuietUdp(pf::NetworkProvider& network) noexcept
      : network_(network) {}

  const pf::NetworkProvider& owner() const noexcept override {
    return network_;
  }
  [[nodiscard]] auto bind(const pf::SocketEndpoint&)
      -> libchiaking::Expected<void, pf::NetError> override {
    return {};
  }
  [[nodiscard]] auto send_to(std::span<const std::byte>,
                             const pf::SocketEndpoint&)
      -> libchiaking::Expected<std::size_t, pf::NetError> override {
    return std::size_t{0};
  }
  [[nodiscard]] auto receive_from(std::span<std::byte>)
      -> libchiaking::Expected<pf::UdpReceiveResult, pf::NetError> override {
    return libchiaking::unexpected(pf::NetError::would_block);
  }
  [[nodiscard]] auto set_boolean_option(pf::SocketOption, bool)
      -> libchiaking::Expected<void, pf::NetError> override {
    return {};
  }
  [[nodiscard]] auto set_receive_buffer_size(std::size_t)
      -> libchiaking::Expected<void, pf::NetError> override {
    return {};
  }
  [[nodiscard]] auto local_endpoint()
      -> libchiaking::Expected<pf::SocketEndpoint, pf::NetError> override {
    return libchiaking::unexpected(pf::NetError::not_supported);
  }
  [[nodiscard]] auto close()
      -> libchiaking::Expected<void, pf::NetError> override {
    return {};
  }

 private:
  pf::NetworkProvider& network_;
};

class CountingUdp final : public QuietUdp {
 public:
  CountingUdp(pf::NetworkProvider& network, unsigned& destroyed) noexcept
      : QuietUdp(network), destroyed_(destroyed) {}
  ~CountingUdp() override { ++destroyed_; }

  CountingUdp(const CountingUdp&) = delete;
  auto operator=(const CountingUdp&) -> CountingUdp& = delete;

 private:
  unsigned& destroyed_;
};

class Loopback {
 public:
  Loopback() {
    auto poller = network_.pollers().create_poller();
    EXPECT_TRUE(poller);
    if (poller) poller_ = std::move(poller.value());
  }

  [[nodiscard]] auto factory() -> pf::PlatformFactory& { return factory_; }
  [[nodiscard]] auto network() -> pf::NetworkProvider& { return network_; }
  [[nodiscard]] auto poller() -> pf::Poller& { return *poller_; }
  [[nodiscard]] auto clock() -> const pf::ClockProvider& {
    return factory_.clock();
  }

  auto socket() -> std::unique_ptr<pf::UdpSocket> {
    auto socket =
        network_.sockets().create_udp_socket(pf::IpAddress::loopback_ipv4());
    EXPECT_TRUE(socket);
    if (!socket || !socket.value()) return nullptr;
    EXPECT_TRUE(socket.value()->bind({pf::IpAddress::loopback_ipv4(), 0}));
    return std::move(socket.value());
  }

  static auto endpoint(pf::UdpSocket& socket) -> pf::SocketEndpoint {
    auto endpoint = socket.local_endpoint();
    EXPECT_TRUE(endpoint);
    if (!endpoint) return {};
    return endpoint.value();
  }

  auto readable(pf::UdpSocket& socket,
                std::chrono::nanoseconds timeout = std::chrono::seconds{2},
                const pf::StopToken& stop = {}) -> bool {
    const pf::PollRequest requests[] = {
        pf::PollRequest{
            .pollable = &socket, .tag = 1, .interest = pf::PollEvent::readable},
    };
    const auto waited = poller_->wait(requests, timeout, stop);
    if (!waited) return false;
    for (const auto& ready : waited.value().ready) {
      if (ready.tag == 1 &&
          (ready.ready & pf::PollEvent::readable) != pf::PollEvent::none)
        return true;
    }
    return false;
  }

  auto bound_sockets() -> libchiaking::net::nat::holepunch::BoundSockets {
    auto sockets = libchiaking::net::nat::holepunch::BoundSockets::create(
        network_.sockets(), false);
    EXPECT_TRUE(sockets);
    if (!sockets) return {};
    return std::move(sockets.value());
  }

 private:
  pf::DefaultPlatformFactory factory_;
  pf::NetworkProvider& network_ = factory_.network();
  std::unique_ptr<pf::Poller> poller_;
};

}  // namespace play_test::holepunch_probe
