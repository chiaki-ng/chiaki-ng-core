#pragma once

#include "libchiaking/platform/net/default.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/protocol/remote_play/session_id.hpp"
#include "src/core/net/protocol/remote_play/stream/local_ctrl.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace remote_play_test {

class LocalCtrlCrypto final : public libchiaking::platform::CryptoProvider {
 public:
  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }

  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<std::byte> digest)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    ++calls;
    ++hmac_calls;
    if (fail || fail_hmac)
      return libchiaking::unexpected(
          libchiaking::platform::CryptoError::backend_failure);
    for (auto& byte : digest) byte = std::byte{0};
    return {};
  }

  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>,
                            std::span<std::byte> output)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    ++calls;
    if (fail)
      return libchiaking::unexpected(
          libchiaking::platform::CryptoError::backend_failure);
    for (auto& byte : output) byte = std::byte{0};
    return {};
  }

  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }

  unsigned calls{};
  unsigned hmac_calls{};
  bool fail{};
  bool fail_hmac{};
};

class LocalCtrlSocket final : public libchiaking::platform::TcpSocket {
 public:
  LocalCtrlSocket(libchiaking::platform::NetworkProvider& owner,
                  unsigned& closes)
      : owner_(owner), closes_(closes) {}

  auto owner() const noexcept
      -> const libchiaking::platform::NetworkProvider& override {
    return owner_;
  }
  auto bind(const libchiaking::platform::SocketEndpoint&)
      -> libchiaking::Expected<void, libchiaking::platform::NetError> override {
    return {};
  }
  auto connect(const libchiaking::platform::SocketEndpoint&)
      -> libchiaking::Expected<void, libchiaking::platform::NetError> override {
    return {};
  }
  auto complete_connect()
      -> libchiaking::Expected<void, libchiaking::platform::NetError> override {
    return {};
  }
  auto send(std::span<const std::byte>)
      -> libchiaking::Expected<std::size_t,
                               libchiaking::platform::NetError> override {
    return libchiaking::unexpected(
        libchiaking::platform::NetError::would_block);
  }
  auto receive(std::span<std::byte>)
      -> libchiaking::Expected<std::size_t,
                               libchiaking::platform::NetError> override {
    return libchiaking::unexpected(
        libchiaking::platform::NetError::would_block);
  }
  auto local_endpoint()
      -> libchiaking::Expected<libchiaking::platform::SocketEndpoint,
                               libchiaking::platform::NetError> override {
    return libchiaking::platform::SocketEndpoint{};
  }
  auto peer_endpoint()
      -> libchiaking::Expected<libchiaking::platform::SocketEndpoint,
                               libchiaking::platform::NetError> override {
    return libchiaking::platform::SocketEndpoint{};
  }
  auto close()
      -> libchiaking::Expected<void, libchiaking::platform::NetError> override {
    ++closes_;
    return {};
  }

 private:
  libchiaking::platform::NetworkProvider& owner_;
  unsigned& closes_;
};

class LocalCtrlRig : public ::testing::Test {
 protected:
  static constexpr std::array<std::byte, 32> did{};

  static auto threading() -> libchiaking::platform::DefaultThreadingProvider& {
    return test_support::process_threading();
  }

  static auto steady() -> const libchiaking::platform::SteadyClock& {
    static const libchiaking::platform::SteadyClock clock;
    return clock;
  }

  static auto console(
      libchiaking::net::protocol::remote_play::RegistrationTarget target)
      -> libchiaking::net::protocol::remote_play::RegisteredConsole {
    libchiaking::net::protocol::remote_play::RegisteredConsole value;
    value.target = target;
    for (std::size_t i = 0; i < 16; ++i) {
      value.key[i] = static_cast<std::byte>(0x20 + i);
      value.registration_key[i] = static_cast<std::byte>(0x40 + i);
    }
    return value;
  }

  static auto counting_nonce() -> std::array<std::byte, 16> {
    std::array<std::byte, 16> nonce{};
    for (std::size_t i = 0; i < nonce.size(); ++i)
      nonce[i] = static_cast<std::byte>(i + 1);
    return nonce;
  }
};

class LocalCtrlPeerRig : public LocalCtrlRig {
 protected:
  void SetUp() override {
    auto created = network.sockets().create_tcp_listener(
        libchiaking::platform::IpAddress::loopback_ipv4());
    ASSERT_TRUE(created);
    listener = std::move(created.value());
    ASSERT_TRUE(
        listener->bind({libchiaking::platform::IpAddress::loopback_ipv4(), 0}));
    ASSERT_TRUE(listener->listen(1));
    const auto local = listener->local_endpoint();
    ASSERT_TRUE(local);
    endpoint = local.value();
    ps5.target =
        libchiaking::net::protocol::remote_play::RegistrationTarget::ps5_1;
  }

  static auto deadline_after(std::chrono::microseconds span) -> std::uint64_t {
    return steady().now_microseconds() +
           static_cast<std::uint64_t>(span.count());
  }

  static auto still_before(std::uint64_t deadline) -> bool {
    return steady().now_microseconds() < deadline;
  }

  void nap() { sleeper.sleep_for(std::chrono::milliseconds{1}); }

  auto config(std::uint64_t attempt_id) const
      -> libchiaking::net::protocol::remote_play::LocalCtrlAttempt::Config {
    return {ps5,
            "127.0.0.1",
            endpoint.port,
            did,
            libchiaking::net::protocol::remote_play::VideoCodec::h264,
            nonce,
            endpoint,
            attempt_id};
  }

  auto expected_request() -> std::optional<std::string> {
    auto value = libchiaking::net::protocol::remote_play::LocalCtrlPreparation::
        prepare_from_nonce(
            crypto, ps5, "127.0.0.1", endpoint.port, did,
            libchiaking::net::protocol::remote_play::VideoCodec::h264, nonce);
    if (!value) return std::nullopt;
    return std::string(value.value().request.view());
  }

  auto accept_until(
      libchiaking::net::protocol::remote_play::LocalCtrlAttempt& attempt)
      -> std::unique_ptr<libchiaking::platform::TcpSocket> {
    const auto deadline = deadline_after(std::chrono::seconds{2});
    while (still_before(deadline)) {
      const auto progress = attempt.pump_once();
      if (!progress) {
        ADD_FAILURE() << "the attempt failed while connecting";
        return {};
      }
      EXPECT_FALSE(progress.value());
      auto accepted = listener->accept();
      if (accepted) return std::move(accepted.value());
      EXPECT_EQ(accepted.error(), libchiaking::platform::NetError::would_block);
      nap();
    }
    ADD_FAILURE() << "the console never accepted the connection";
    return {};
  }

  void receive_request(
      libchiaking::net::protocol::remote_play::LocalCtrlAttempt& attempt,
      libchiaking::platform::TcpSocket& accepted) {
    const auto expected = expected_request();
    ASSERT_TRUE(expected);
    std::string received;
    const auto deadline = deadline_after(std::chrono::seconds{2});
    while (received.size() < expected->size() && still_before(deadline)) {
      const auto progress = attempt.pump_once();
      ASSERT_TRUE(progress);
      EXPECT_FALSE(progress.value());
      std::array<std::byte, 1024> buffer{};
      const auto part = accepted.receive(buffer);
      if (part)
        received.append(reinterpret_cast<const char*>(buffer.data()),
                        part.value());
      else
        EXPECT_EQ(part.error(), libchiaking::platform::NetError::would_block);
      nap();
    }
    EXPECT_EQ(received, *expected);
  }

  void send_response(libchiaking::platform::TcpSocket& socket,
                     std::string_view response = "HTTP/1.1 200 OK\r\n\r\n") {
    auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(response.data()), response.size());
    const auto deadline = deadline_after(std::chrono::seconds{2});
    while (!bytes.empty() && still_before(deadline)) {
      const auto sent = socket.send(bytes);
      if (sent)
        bytes = bytes.subspan(sent.value());
      else
        EXPECT_EQ(sent.error(), libchiaking::platform::NetError::would_block);
      nap();
    }
    EXPECT_TRUE(bytes.empty());
  }

  void wait_until_ready(
      libchiaking::net::protocol::remote_play::LocalCtrlAttempt& attempt) {
    const auto deadline = deadline_after(std::chrono::seconds{2});
    bool ready{};
    while (!ready && still_before(deadline)) {
      const auto progress = attempt.pump_once();
      ASSERT_TRUE(progress);
      ready = progress.value();
      nap();
    }
    ASSERT_TRUE(ready);
  }

  libchiaking::platform::DefaultNetworkProvider network{threading(), steady()};
  libchiaking::platform::Sleeper sleeper{threading(), steady()};
  std::unique_ptr<libchiaking::platform::TcpListener> listener;
  libchiaking::platform::SocketEndpoint endpoint;
  test_support::FakeClock clock{1'000'000};
  LocalCtrlCrypto crypto;
  libchiaking::net::protocol::remote_play::RegisteredConsole ps5;
  std::array<std::byte, 16> nonce = counting_nonce();
};

}  // namespace remote_play_test
