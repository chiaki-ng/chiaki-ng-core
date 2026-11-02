#pragma once

#include "libchiaking/platform/net/default.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "src/core/net/protocol/remote_play/ctrl_exchange.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace remote_play_test {

class CtrlLoopbackRig : public ::testing::Test {
 protected:
  using NetError = libchiaking::platform::NetError;
  using Socket = libchiaking::platform::TcpSocket;
  using Listener = libchiaking::platform::TcpListener;

  static auto steady() -> const libchiaking::platform::SteadyClock& {
    static const libchiaking::platform::SteadyClock clock;
    return clock;
  }

  static auto deadline_after(std::chrono::microseconds span) -> std::uint64_t {
    return steady().now_microseconds() +
           static_cast<std::uint64_t>(span.count());
  }

  static auto still_before(std::uint64_t deadline) -> bool {
    return steady().now_microseconds() < deadline;
  }

  static void sleep(std::chrono::milliseconds duration) {
    static libchiaking::platform::Sleeper sleeper{
        test_support::process_threading(), steady()};
    sleeper.sleep_for(duration);
  }

  static void nap() { sleep(std::chrono::milliseconds{1}); }

  static auto bytes(std::string_view text) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
  }

  static auto owned(std::string_view text) -> std::vector<std::byte> {
    const auto view = bytes(text);
    return {view.begin(), view.end()};
  }

  static void send_all(Socket& socket, std::span<const std::byte> data) {
    const auto deadline = deadline_after(std::chrono::seconds{2});
    while (!data.empty() && still_before(deadline)) {
      const auto sent = socket.send(data);
      if (sent) {
        ASSERT_NE(sent.value(), 0U);
        data = data.subspan(sent.value());
      } else {
        ASSERT_EQ(sent.error(), NetError::would_block);
        nap();
      }
    }
    ASSERT_TRUE(data.empty());
  }

  static auto receive_exact(Socket& socket, std::size_t size)
      -> std::vector<std::byte> {
    std::vector<std::byte> output(size);
    std::size_t filled = 0;
    const auto deadline = deadline_after(std::chrono::seconds{2});
    while (filled < size && still_before(deadline)) {
      const auto part = socket.receive(std::span(output).subspan(filled));
      if (part)
        filled += part.value();
      else
        EXPECT_EQ(part.error(), NetError::would_block);
      nap();
    }
    EXPECT_EQ(filled, size);
    output.resize(filled);
    return output;
  }

  auto listen(std::unique_ptr<Listener>& listener,
              libchiaking::platform::SocketEndpoint& endpoint,
              bool start_listening = true) -> testing::AssertionResult {
    auto created = network.sockets().create_tcp_listener(
        libchiaking::platform::IpAddress::loopback_ipv4());
    if (!created) return testing::AssertionFailure() << "no listener";
    listener = std::move(created.value());
    if (!listener->bind({libchiaking::platform::IpAddress::loopback_ipv4(), 0}))
      return testing::AssertionFailure() << "bind failed";
    if (start_listening && !listener->listen(1))
      return testing::AssertionFailure() << "listen failed";
    const auto local = listener->local_endpoint();
    if (!local) return testing::AssertionFailure() << "no local endpoint";
    endpoint = local.value();
    return testing::AssertionSuccess();
  }

  libchiaking::platform::DefaultNetworkProvider network{
      test_support::process_threading(), steady()};
};

}  // namespace remote_play_test
