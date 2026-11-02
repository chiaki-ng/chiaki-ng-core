#pragma once

#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "libchiaking/testing/in_memory_network.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace network_test {

namespace platform = libchiaking::platform;
namespace fakes = libchiaking::testing;

using namespace std::chrono_literals;

class InMemoryNetworkTest : public ::testing::Test {
 protected:
  struct Pair {
    std::unique_ptr<platform::TcpListener> listener;
    std::unique_ptr<platform::TcpSocket> client;
    std::unique_ptr<platform::TcpSocket> server;
  };

  template <class Work>
  auto after_a_moment(Work work) -> platform::Thread {
    return platform::Thread::start(
               threading, "in memory network test",
               platform::ThreadRole::unspecified,
               [this, work = std::move(work)] {
                 platform::Sleeper{threading, monotonic}.sleep_for(30ms);
                 work();
               })
        .value();
  }

  static auto bytes(std::string_view text) -> std::span<const std::byte> {
    return std::as_bytes(std::span{text.data(), text.size()});
  }

  static auto text(std::span<const std::byte> data) -> std::string {
    return {reinterpret_cast<const char*>(data.data()), data.size()};
  }

  static auto loopback(std::uint16_t port = 0) -> platform::SocketEndpoint {
    return {platform::IpAddress::loopback_ipv4(), port};
  }

  static auto udp(fakes::InMemoryNetwork& on, platform::SocketEndpoint endpoint)
      -> std::unique_ptr<platform::UdpSocket> {
    auto socket = on.sockets().create_udp_socket(endpoint.address);
    EXPECT_TRUE(socket);
    EXPECT_TRUE(socket.value()->bind(endpoint));
    return std::move(socket.value());
  }

  static auto listener(fakes::InMemoryNetwork& on,
                       platform::SocketEndpoint endpoint = loopback())
      -> std::unique_ptr<platform::TcpListener> {
    auto created = on.sockets().create_tcp_listener(endpoint.address);
    EXPECT_TRUE(created);
    EXPECT_TRUE(created.value()->bind(endpoint));
    EXPECT_TRUE(created.value()->listen(4));
    return std::move(created.value());
  }

  static auto client(fakes::InMemoryNetwork& on)
      -> std::unique_ptr<platform::TcpSocket> {
    auto created =
        on.sockets().create_tcp_socket(platform::IpAddress::loopback_ipv4());
    EXPECT_TRUE(created);
    return std::move(created.value());
  }

  static auto wait_for(fakes::InMemoryNetwork& on,
                       const platform::Pollable& pollable,
                       platform::PollEvent interest,
                       std::chrono::milliseconds timeout = 500ms)
      -> platform::PollEvent {
    auto poller = on.pollers().create_poller();
    EXPECT_TRUE(poller);
    const std::array<platform::PollRequest, 1> requests{
        {{&pollable, 7, interest}}};
    const auto result =
        poller.value()->wait(requests, timeout, platform::StopToken{});
    EXPECT_TRUE(result);
    if (result.value().ready.empty()) return platform::PollEvent::none;
    EXPECT_EQ(result.value().ready.front().tag, 7U);
    return result.value().ready.front().ready;
  }

  static auto connected_pair(fakes::InMemoryNetwork& on) -> Pair {
    Pair pair;
    pair.listener = listener(on);
    pair.client = client(on);
    const auto target = pair.listener->local_endpoint();
    EXPECT_TRUE(target);
    const auto started = pair.client->connect(target.value());
    EXPECT_FALSE(started);
    EXPECT_EQ(started.error_or(platform::NetError{}),
              platform::NetError::would_block);
    EXPECT_EQ(wait_for(on, *pair.client, platform::PollEvent::writable),
              platform::PollEvent::writable);
    EXPECT_TRUE(pair.client->complete_connect());
    auto accepted = pair.listener->accept();
    EXPECT_TRUE(accepted);
    pair.server = std::move(accepted.value());
    return pair;
  }

  platform::DefaultThreadingProvider& threading{
      test_support::process_threading()};
  platform::SteadyClock monotonic;
  fakes::InMemoryNetwork network{threading, monotonic};
};

}  // namespace network_test
