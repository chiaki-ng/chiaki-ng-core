#pragma once

#include "src/core/net/ports.hpp"
#include "src/core/net/protocol/remote_play/pairing/cipher.hpp"
#include "src/core/net/protocol/remote_play/pairing/material.hpp"
#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/crypto/provider.hpp>
#include <libchiaking/platform/net/poller.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/random_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/testing/in_memory_network.hpp>
#include "src/core/net/protocol/remote_play/registered_console.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace consoles_test {

class XorCrypto final : public libchiaking::platform::CryptoProvider {
 public:
  bool fail_hmac{};

  [[nodiscard]] auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }
  [[nodiscard]] auto hmac_sha256(std::span<const std::byte>,
                                 std::span<const std::byte>,
                                 std::span<std::byte> digest)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    if (fail_hmac)
      return libchiaking::unexpected(
          libchiaking::platform::CryptoError::backend_failure);
    for (std::size_t i = 0; i < digest.size(); ++i)
      digest[i] = static_cast<std::byte>(i);
    return {};
  }
  [[nodiscard]] auto aes128_encrypt_block(std::span<const std::byte> key,
                                          std::span<const std::byte> block,
                                          std::span<std::byte> output)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    for (std::size_t i = 0; i < output.size(); ++i)
      output[i] = block[i] ^ key[i];
    return {};
  }
  [[nodiscard]] auto aes128_gmac(std::span<const std::byte>,
                                 std::span<const std::byte>,
                                 std::span<const std::byte>,
                                 std::span<std::byte>)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }
};

class HomeConsole {
 public:
  static constexpr std::string_view ps4_facts =
      "PS4-Mac:001122334455\r\n"
      "PS4-RegistKey:0102\r\n"
      "RP-Key:00112233445566778899aabbccddeeff\r\n";
  static constexpr std::string_view ps5_facts =
      "PS5-Mac:7a1b2c3d4e5f\r\n"
      "PS5-RegistKey:0102\r\n"
      "RP-Key:00112233445566778899aabbccddeeff\r\n"
      "PS5-Nickname:Den\r\n"
      "RP-KeyType:2\r\n";
  static constexpr std::string_view refusal =
      "HTTP/1.1 403 Forbidden\r\nRP-Application-Reason: 80108b12\r\n"
      "Content-Length: 0\r\n\r\n";

  explicit HomeConsole(libchiaking::testing::InMemoryNetwork& network,
                       std::uint16_t port = libchiaking::rp_session_port)
      : network_(network) {
    const auto loopback = libchiaking::platform::IpAddress::loopback_ipv4();
    udp_ = network.sockets().create_udp_socket(loopback).value();
    EXPECT_TRUE(udp_->bind({loopback, port}));
    endpoint_ = udp_->local_endpoint().value();
    listener_ = network.sockets().create_tcp_listener(loopback).value();
    EXPECT_TRUE(listener_->bind(endpoint_));
    EXPECT_TRUE(listener_->listen(1));
  }

  HomeConsole(const HomeConsole&) = delete;
  auto operator=(const HomeConsole&) -> HomeConsole& = delete;

  ~HomeConsole() { stop_serving(); }

  static auto bytes(std::string_view text) -> std::vector<std::byte> {
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return {first, first + text.size()};
  }

  static auto text(std::span<const std::byte> bytes) -> std::string {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
  }

  static auto material(
      libchiaking::net::protocol::remote_play::RegistrationTarget target,
      std::uint32_t pin)
      -> libchiaking::net::protocol::remote_play::RegistrationMaterial {
    std::array<std::byte, 16> ambassador{};
    for (std::size_t i = 0; i < ambassador.size(); ++i)
      ambassador[i] = static_cast<std::byte>(i);
    return libchiaking::net::protocol::remote_play::RegistrationMaterial::
        derive_local(target, ambassador, pin)
            .value();
  }

  static auto welcome(
      libchiaking::platform::CryptoProvider& crypto,
      libchiaking::net::protocol::remote_play::RegistrationTarget target,
      std::uint32_t pin, std::string_view facts,
      std::string_view past_the_length = {}) -> std::vector<std::byte> {
    auto body = bytes(std::string{facts} + std::string{past_the_length});
    const auto keys = material(target, pin);
    const auto transformed =
        libchiaking::net::protocol::remote_play::RegistrationCipher::transform(
            crypto, target, {keys.bright.data(), keys.bright.size()},
            {keys.ambassador.data(), keys.ambassador.size()}, 0,
            libchiaking::net::protocol::remote_play::CipherDirection::encrypt,
            body, body);
    EXPECT_TRUE(transformed);
    auto frame = bytes("HTTP/1.1 200 OK\r\nContent-Length: " +
                       std::to_string(facts.size()) + "\r\n\r\n");
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
  }

  auto endpoint() const noexcept -> libchiaking::platform::SocketEndpoint {
    return endpoint_;
  }

  auto hear_knock() -> std::optional<libchiaking::platform::SocketEndpoint> {
    std::array<std::byte, 64> buffer{};
    const auto received = udp_->receive_from(buffer);
    if (!received) return std::nullopt;
    knock = text(std::span{buffer}.first(received.value().size));
    return received.value().source;
  }

  auto await_knock() -> libchiaking::platform::SocketEndpoint {
    wait_until_readable(*udp_);
    return hear_knock().value();
  }

  void answer(const libchiaking::platform::SocketEndpoint& to,
              std::string_view reply) {
    const auto sent = udp_->send_to(bytes(reply), to);
    ASSERT_TRUE(sent);
    EXPECT_EQ(sent.value(), reply.size());
  }

  auto accept() -> bool {
    if (accepted_) return true;
    auto next = listener_->accept();
    if (!next) {
      EXPECT_EQ(next.error(), libchiaking::platform::NetError::would_block);
      return false;
    }
    accepted_ = std::move(next.value());
    return true;
  }

  auto read()
      -> libchiaking::Expected<std::size_t, libchiaking::platform::NetError> {
    if (!accepted_) {
      ADD_FAILURE() << "read before a connection was accepted";
      return libchiaking::unexpected(
          libchiaking::platform::NetError::not_connected);
    }
    std::array<std::byte, 2048> buffer{};
    const auto part = accepted_->receive(buffer);
    if (part)
      request.insert(
          request.end(), buffer.begin(),
          buffer.begin() + static_cast<std::ptrdiff_t>(part.value()));
    return part;
  }

  auto hung_up() -> bool {
    auto part = read();
    while (part) part = read();
    return part.error() == libchiaking::platform::NetError::closed;
  }

  void send(std::span<const std::byte> reply) {
    ASSERT_NE(accepted_, nullptr);
    const auto sent = accepted_->send(reply);
    ASSERT_TRUE(sent);
    EXPECT_EQ(sent.value(), reply.size());
  }

  void send(std::string_view reply) {
    send(std::span<const std::byte>{bytes(reply)});
  }

  void hang_up() {
    if (accepted_) (void)accepted_->close();
  }

  auto request_text() const -> std::string { return text(request); }

  auto request_complete() const -> bool {
    const auto heard = request_text();
    const auto head_end = heard.find("\r\n\r\n");
    static constexpr std::string_view label = "Content-Length: ";
    const auto length_at = heard.find(label);
    if (head_end == std::string::npos || length_at == std::string::npos ||
        length_at > head_end)
      return false;
    std::size_t length = 0;
    const auto* first = heard.data() + length_at + label.size();
    std::from_chars(first, heard.data() + head_end, length);
    return heard.size() >= head_end + 4 + length;
  }

  void serve(std::string knock_answer, std::vector<std::byte> reply) {
    ASSERT_FALSE(server_.joinable());
    server_ = libchiaking::platform::Thread::start(
                  threading_, "test-home-console",
                  libchiaking::platform::ThreadRole::unspecified,
                  [this, knock_answer = std::move(knock_answer),
                   reply = std::move(reply)] {
                    bool replied = false;
                    while (!stopping_.load()) {
                      if (const auto source = hear_knock();
                          source && !knock_answer.empty()) {
                        answer(*source, knock_answer);
                        knock_answered.store(true);
                      }
                      if (accept()) {
                        (void)read();
                        if (!replied && !reply.empty() && request_complete()) {
                          send(std::span<const std::byte>{reply});
                          replied = true;
                        }
                      }
                      sleeper_.sleep_for(std::chrono::milliseconds{1});
                    }
                  })
                  .value();
  }

  void stop_serving() {
    stopping_.store(true);
    if (server_.joinable()) static_cast<void>(server_.join());
    stopping_.store(false);
  }

  std::string knock;
  std::vector<std::byte> request;
  std::atomic<bool> knock_answered{};

 private:
  void wait_until_readable(const libchiaking::platform::Pollable& pollable) {
    auto poller = network_.pollers().create_poller().value();
    const std::array<libchiaking::platform::PollRequest, 1> requests{
        {{&pollable, 1, libchiaking::platform::PollEvent::readable}}};
    const auto ready = poller->wait(requests, std::chrono::seconds{2},
                                    libchiaking::platform::StopToken{});
    ASSERT_TRUE(ready);
    EXPECT_FALSE(ready.value().ready.empty());
  }

  libchiaking::testing::InMemoryNetwork& network_;
  std::unique_ptr<libchiaking::platform::UdpSocket> udp_;
  std::unique_ptr<libchiaking::platform::TcpListener> listener_;
  std::unique_ptr<libchiaking::platform::TcpSocket> accepted_;
  libchiaking::platform::SocketEndpoint endpoint_{};
  libchiaking::platform::DefaultThreadingProvider threading_;
  libchiaking::platform::SteadyClock steady_;
  libchiaking::platform::Sleeper sleeper_{threading_, steady_};
  libchiaking::platform::Thread server_;
  std::atomic<bool> stopping_{};
};

}  // namespace consoles_test
