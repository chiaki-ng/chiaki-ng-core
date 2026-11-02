#pragma once

#include "libchiaking/platform/net/default.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/protocol/remote_play/tcp_exchange.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace remote_play_test {

class TcpExchangeRig {
 public:
  static constexpr std::array<std::byte, 16> key{};

  static auto threading() -> libchiaking::platform::DefaultThreadingProvider& {
    return test_support::process_threading();
  }

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

  static void nap() {
    static libchiaking::platform::Sleeper sleeper{threading(), steady()};
    sleeper.sleep_for(std::chrono::milliseconds{1});
  }

  static auto bytes(std::string_view text) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
  }

  static auto byte_vector(std::string_view text) -> std::vector<std::byte> {
    const auto view = bytes(text);
    return {view.begin(), view.end()};
  }

  static auto request(
      libchiaking::net::protocol::remote_play::RegistrationTarget target,
      std::uint16_t port)
      -> std::optional<
          libchiaking::net::protocol::remote_play::SessionRequest> {
    auto encoded =
        libchiaking::net::protocol::remote_play::SessionCodec::encode_request(
            target, "127.0.0.1", port, key);
    if (!encoded) return std::nullopt;
    return std::move(encoded.value());
  }

  static auto send_all(libchiaking::platform::TcpSocket& peer,
                       std::string_view text) -> testing::AssertionResult {
    auto remaining = bytes(text);
    const auto deadline = deadline_after(std::chrono::seconds{2});
    while (!remaining.empty() && still_before(deadline)) {
      const auto sent = peer.send(remaining);
      if (sent) {
        if (sent.value() == 0)
          return testing::AssertionFailure() << "the peer sent nothing";
        remaining = remaining.subspan(sent.value());
      } else {
        if (sent.error() != libchiaking::platform::NetError::would_block)
          return testing::AssertionFailure() << "the peer failed to send";
        nap();
      }
    }
    if (!remaining.empty())
      return testing::AssertionFailure()
             << remaining.size() << " bytes were never sent";
    return testing::AssertionSuccess();
  }
};

class TcpExchangeLoopbackPeer {
 public:
  auto open() -> testing::AssertionResult {
    auto created = network.sockets().create_tcp_listener(
        libchiaking::platform::IpAddress::loopback_ipv4());
    if (!created)
      return testing::AssertionFailure() << "no listener was created";
    listener = std::move(created.value());
    if (!listener->bind({libchiaking::platform::IpAddress::loopback_ipv4(), 0}))
      return testing::AssertionFailure() << "the listener did not bind";
    if (!listener->listen(1))
      return testing::AssertionFailure() << "the listener did not listen";
    const auto local = listener->local_endpoint();
    if (!local)
      return testing::AssertionFailure() << "the listener has no endpoint";
    endpoint = local.value();
    return testing::AssertionSuccess();
  }

  auto accept_until(
      libchiaking::net::protocol::remote_play::SessionTcpExchange& exchange)
      -> std::unique_ptr<libchiaking::platform::TcpSocket> {
    const auto deadline =
        TcpExchangeRig::deadline_after(std::chrono::seconds{2});
    while (TcpExchangeRig::still_before(deadline)) {
      const auto progress = exchange.pump_once();
      EXPECT_TRUE(progress);
      if (!progress) return {};
      EXPECT_FALSE(progress.value());
      auto accepted = listener->accept();
      if (accepted) return std::move(accepted.value());
      EXPECT_EQ(accepted.error(), libchiaking::platform::NetError::would_block);
      TcpExchangeRig::nap();
    }
    ADD_FAILURE() << "the exchange never connected";
    return {};
  }

  libchiaking::platform::DefaultNetworkProvider network{
      TcpExchangeRig::threading(), TcpExchangeRig::steady()};
  std::unique_ptr<libchiaking::platform::TcpListener> listener;
  libchiaking::platform::SocketEndpoint endpoint;
};

class TcpExchangeSocketScript {
 public:
  std::deque<libchiaking::Expected<void, libchiaking::platform::NetError>>
      connects;
  std::deque<libchiaking::Expected<void, libchiaking::platform::NetError>>
      completions;
  std::deque<
      libchiaking::Expected<std::size_t, libchiaking::platform::NetError>>
      sends;
  std::deque<std::string> replies;
  std::vector<std::byte> written;
  unsigned created{};
  unsigned closes{};
};

class TcpExchangeScriptedSocket final
    : public libchiaking::platform::TcpSocket {
 public:
  TcpExchangeScriptedSocket(TcpExchangeSocketScript& script,
                            const libchiaking::platform::NetworkProvider& owner)
      : script_(script), owner_(owner) {}

  const libchiaking::platform::NetworkProvider& owner()
      const noexcept override {
    return owner_;
  }

  auto bind(const libchiaking::platform::SocketEndpoint&)
      -> libchiaking::Expected<void, libchiaking::platform::NetError> override {
    return {};
  }

  auto connect(const libchiaking::platform::SocketEndpoint&)
      -> libchiaking::Expected<void, libchiaking::platform::NetError> override {
    return next(script_.connects,
                libchiaking::Expected<void, libchiaking::platform::NetError>{});
  }

  auto complete_connect()
      -> libchiaking::Expected<void, libchiaking::platform::NetError> override {
    return next(script_.completions,
                libchiaking::Expected<void, libchiaking::platform::NetError>{
                    libchiaking::unexpected(
                        libchiaking::platform::NetError::would_block)});
  }

  auto send(std::span<const std::byte> data)
      -> libchiaking::Expected<std::size_t,
                               libchiaking::platform::NetError> override {
    auto result = next(
        script_.sends,
        libchiaking::Expected<std::size_t, libchiaking::platform::NetError>{
            data.size()});
    if (result)
      script_.written.insert(
          script_.written.end(), data.begin(),
          data.begin() + static_cast<std::ptrdiff_t>(result.value()));
    return result;
  }

  auto receive(std::span<std::byte> buffer)
      -> libchiaking::Expected<std::size_t,
                               libchiaking::platform::NetError> override {
    if (script_.replies.empty())
      return libchiaking::unexpected(
          libchiaking::platform::NetError::would_block);
    auto& reply = script_.replies.front();
    const auto size = std::min(buffer.size(), reply.size());
    std::memcpy(buffer.data(), reply.data(), size);
    reply.erase(0, size);
    if (reply.empty()) script_.replies.pop_front();
    return size;
  }

  auto local_endpoint()
      -> libchiaking::Expected<libchiaking::platform::SocketEndpoint,
                               libchiaking::platform::NetError> override {
    return libchiaking::platform::SocketEndpoint{
        libchiaking::platform::IpAddress::loopback_ipv4(), 40000};
  }

  auto peer_endpoint()
      -> libchiaking::Expected<libchiaking::platform::SocketEndpoint,
                               libchiaking::platform::NetError> override {
    return libchiaking::platform::SocketEndpoint{
        libchiaking::platform::IpAddress::loopback_ipv4(), 9295};
  }

  auto close()
      -> libchiaking::Expected<void, libchiaking::platform::NetError> override {
    ++script_.closes;
    return {};
  }

 private:
  template <class Result>
  static auto next(std::deque<Result>& queue, Result fallback) -> Result {
    if (queue.empty()) return fallback;
    auto result = std::move(queue.front());
    queue.pop_front();
    return result;
  }

  TcpExchangeSocketScript& script_;
  const libchiaking::platform::NetworkProvider& owner_;
};

class TcpExchangeScriptedSockets final
    : public libchiaking::platform::SocketProvider {
 public:
  explicit TcpExchangeScriptedSockets(TcpExchangeSocketScript& script)
      : script_(script) {}

  auto create_udp_socket(const libchiaking::platform::IpAddress&)
      -> libchiaking::Expected<
          std::unique_ptr<libchiaking::platform::UdpSocket>,
          libchiaking::platform::NetError> override {
    return libchiaking::unexpected(
        libchiaking::platform::NetError::not_supported);
  }

  auto create_tcp_socket(const libchiaking::platform::IpAddress&)
      -> libchiaking::Expected<
          std::unique_ptr<libchiaking::platform::TcpSocket>,
          libchiaking::platform::NetError> override {
    ++script_.created;
    return std::make_unique<TcpExchangeScriptedSocket>(script_, owner_);
  }

  auto create_tcp_listener(const libchiaking::platform::IpAddress&)
      -> libchiaking::Expected<
          std::unique_ptr<libchiaking::platform::TcpListener>,
          libchiaking::platform::NetError> override {
    return libchiaking::unexpected(
        libchiaking::platform::NetError::not_supported);
  }

 private:
  TcpExchangeSocketScript& script_;
  libchiaking::platform::DefaultNetworkProvider owner_{
      TcpExchangeRig::threading(), TcpExchangeRig::steady()};
};

}  // namespace remote_play_test
