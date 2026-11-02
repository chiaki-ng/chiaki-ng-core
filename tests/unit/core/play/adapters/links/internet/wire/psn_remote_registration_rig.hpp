#pragma once

#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "src/core/play/adapters/links/internet/wire/psn_remote_registration.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/stop_states.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test::psn_remote_registration {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace rudp = libchiaking::net::transport::rudp;
namespace datagram = libchiaking::net::transport::datagram;
namespace platform = libchiaking::platform;

using CryptoResult = libchiaking::Expected<void, platform::CryptoError>;

class PairingCrypto final : public platform::CryptoProvider {
 public:
  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> CryptoResult override {
    return refused();
  }
  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<std::byte> output) -> CryptoResult override {
    ++hmac_calls_;
    if (fail_ || fail_on_hmac_ == hmac_calls_) return refused();
    std::ranges::fill(output, std::byte{0});
    return {};
  }
  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>,
                            std::span<std::byte> output)
      -> CryptoResult override {
    std::ranges::fill(output, std::byte{0});
    return {};
  }
  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> CryptoResult override {
    return refused();
  }

  auto hmac_calls() const noexcept -> int { return hmac_calls_; }
  void fail_on_hmac(int call) noexcept { fail_on_hmac_ = call; }
  void fail(bool failing) noexcept { fail_ = failing; }

 private:
  static auto refused() -> CryptoResult {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  int hmac_calls_{};
  int fail_on_hmac_{};
  bool fail_{};
};

class PairingChannel final : public rudp::HandshakeChannel {
 public:
  static constexpr std::uint64_t timeout_delay = ~std::uint64_t{0};

  explicit PairingChannel(test_support::FakeClock& clock) noexcept
      : clock_(clock) {}

  auto send(std::span<const std::byte> bytes)
      -> libchiaking::Expected<std::size_t, datagram::TransportError> override {
    sent_.emplace_back(bytes.begin(), bytes.end());
    return bytes.size();
  }
  auto receive_for(std::chrono::microseconds timeout,
                   const platform::StopToken&)
      -> libchiaking::Expected<std::optional<datagram::OwnedDatagram>,
                               datagram::TransportError> override {
    ++receive_calls_;
    if (!delays_.empty()) {
      const auto delay = delays_.front();
      delays_.pop_front();
      if (delay == timeout_delay) {
        clock_.now_us += static_cast<std::uint64_t>(timeout.count());
        return std::optional<datagram::OwnedDatagram>{};
      }
      clock_.now_us += delay;
    }
    if (!inbox_.empty()) {
      auto packet = std::move(inbox_.front());
      inbox_.pop_front();
      if (stop_state_ && receive_calls_ == 4) stop_state_->stopped = true;
      return std::optional<datagram::OwnedDatagram>{std::move(packet)};
    }
    clock_.now_us += static_cast<std::uint64_t>(timeout.count());
    return std::optional<datagram::OwnedDatagram>{};
  }

  void delay(std::uint64_t microseconds) { delays_.push_back(microseconds); }
  void stop_on_fourth_receive(std::shared_ptr<test_support::StopState> state) {
    stop_state_ = std::move(state);
  }
  auto inbox() noexcept -> std::deque<datagram::OwnedDatagram>& {
    return inbox_;
  }
  auto sent() const noexcept -> const std::vector<datagram::OwnedDatagram>& {
    return sent_;
  }

 private:
  test_support::FakeClock& clock_;
  std::deque<std::uint64_t> delays_;
  std::shared_ptr<test_support::StopState> stop_state_;
  unsigned receive_calls_{};
  std::deque<datagram::OwnedDatagram> inbox_;
  std::vector<datagram::OwnedDatagram> sent_;
};

class Replies {
 public:
  static constexpr std::string_view ps5_body =
      "PS5-RegistKey: 010102030405060708090a0b0c0d0e0f\r\n"
      "RP-Key: 000102030405060708090a0b0c0d0e0f\r\n"
      "PS5-Mac: 001122334455\r\n";

  static auto accepted_http(std::string_view body) -> std::string {
    return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\n\r\n" + std::string(body);
  }

  static auto frame(std::uint16_t type, std::vector<std::byte> payload)
      -> datagram::OwnedDatagram {
    const rudp::Frame one{type, std::move(payload)};
    auto encoded = rudp::FrameCodec::encode({&one, 1});
    EXPECT_TRUE(encoded);
    if (!encoded) return {};
    return std::move(encoded.value());
  }

  static auto frame_type(const datagram::OwnedDatagram& datagram)
      -> std::uint16_t {
    auto frames = rudp::FrameCodec::decode(datagram);
    EXPECT_TRUE(frames);
    if (!frames) return 0;
    EXPECT_FALSE(frames.value().empty());
    if (frames.value().empty()) return 0;
    return frames.value().front().type;
  }

  static auto init_response() -> datagram::OwnedDatagram {
    return frame(0xD000,
                 {std::byte{0x56}, std::byte{0x78}, std::byte{0}, std::byte{0},
                  std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
                  std::byte{0xC0}, std::byte{0x0E}});
  }

  static auto cookie_response() -> datagram::OwnedDatagram {
    return frame(0xA030, {std::byte{0x56}, std::byte{0x79}});
  }

  static auto http_response(std::string_view http) -> datagram::OwnedDatagram {
    std::vector<std::byte> payload{std::byte{0x56}, std::byte{0x7A}};
    for (const char c : http) payload.push_back(static_cast<std::byte>(c));
    return frame(0x0230, std::move(payload));
  }

  static auto registration_response() -> datagram::OwnedDatagram {
    return http_response(accepted_http(ps5_body));
  }

  static auto outcome(std::string_view text)
      -> remote_play::RudpCtrlHttpExchangeOutcome {
    remote_play::RudpCtrlHttpExchangeOutcome result{};
    for (const char c : text)
      result.response.received.push_back(static_cast<std::byte>(c));
    return result;
  }

  static void append(remote_play::RudpCtrlHttpExchangeOutcome& outcome,
                     std::string_view text) {
    for (const char c : text)
      outcome.response.received.push_back(static_cast<std::byte>(c));
  }
};

}  // namespace play_test::psn_remote_registration
