#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/net/protocol/remote_play/rudp_ctrl.hpp"
#include "src/core/net/protocol/remote_play/session_codec.hpp"
#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "tests/support/clock.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test::rudp_ctrl {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace rudp = libchiaking::net::transport::rudp;
namespace datagram = libchiaking::net::transport::datagram;
namespace platform = libchiaking::platform;

using CryptoResult = libchiaking::Expected<void, platform::CryptoError>;
using Datagram = datagram::OwnedDatagram;
using Received =
    libchiaking::Expected<std::optional<Datagram>, datagram::TransportError>;
using Sent = libchiaking::Expected<std::size_t, datagram::TransportError>;

class DecryptCrypto final : public platform::CryptoProvider {
 public:
  DecryptCrypto() = default;
  explicit DecryptCrypto(bool zero_counter) noexcept
      : zero_counter_(zero_counter) {}

  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> CryptoResult override {
    return refused();
  }
  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte> input,
                   std::span<std::byte> output) -> CryptoResult override {
    EXPECT_EQ(input.size(), 24U);
    EXPECT_EQ(output.size(), 32U);
    if (zero_counter_) {
      for (const auto byte : input | std::views::drop(16))
        EXPECT_EQ(byte, std::byte{0});
    }
    ++decrypt_calls_;
    if (fail_) return refused();
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

  auto decrypt_calls() const noexcept -> unsigned { return decrypt_calls_; }
  void fail(bool failing) noexcept { fail_ = failing; }

 private:
  static auto refused() -> CryptoResult {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  bool zero_counter_{};
  unsigned decrypt_calls_{};
  bool fail_{};
};

class SpyCrypto final : public platform::CryptoProvider {
 public:
  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> CryptoResult override {
    return refused();
  }
  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte> input,
                   std::span<std::byte> output) -> CryptoResult override {
    EXPECT_EQ(input.size(), 24U);
    if (input.size() != 24) return refused();
    ++calls_;
    std::ranges::copy(input.subspan(16, 8), counter_.begin());
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

  auto counter() const noexcept -> const std::array<std::byte, 8>& {
    return counter_;
  }
  auto calls() const noexcept -> unsigned { return calls_; }

 private:
  static auto refused() -> CryptoResult {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  std::array<std::byte, 8> counter_{};
  unsigned calls_{};
};

class CounterCrypto final : public platform::CryptoProvider {
 public:
  CounterCrypto() = default;
  explicit CounterCrypto(bool passthrough) noexcept
      : passthrough_(passthrough) {}

  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> CryptoResult override {
    return refused();
  }
  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte> input,
                   std::span<std::byte> output) -> CryptoResult override {
    if (!passthrough_) return refused();
    std::uint64_t counter = 0;
    for (const auto byte : input.last(8))
      counter = (counter << 8U) | std::to_integer<unsigned>(byte);
    cipher_counters_.push_back(counter);
    std::ranges::fill(output, std::byte{});
    return {};
  }
  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>,
                            std::span<std::byte> output)
      -> CryptoResult override {
    if (!passthrough_) return refused();
    std::ranges::fill(output, std::byte{});
    return {};
  }
  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> CryptoResult override {
    return refused();
  }

  auto cipher_counters() const noexcept -> const std::vector<std::uint64_t>& {
    return cipher_counters_;
  }

 private:
  static auto refused() -> CryptoResult {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  bool passthrough_{};
  std::vector<std::uint64_t> cipher_counters_;
};

class StubChannel final : public rudp::HandshakeChannel {
 public:
  auto send(std::span<const std::byte> bytes) -> Sent override {
    ++sends_;
    last_.assign(bytes.begin(), bytes.end());
    return short_send_ ? std::size_t{1} : bytes.size();
  }
  auto receive_for(std::chrono::microseconds, const platform::StopToken&)
      -> Received override {
    return std::optional<Datagram>{};
  }

  void short_send(bool shortened) noexcept { short_send_ = shortened; }
  auto sends() const noexcept -> int { return sends_; }
  auto last() const noexcept -> const std::vector<std::byte>& { return last_; }

 private:
  bool short_send_{};
  int sends_{};
  std::vector<std::byte> last_;
};

class ExchangeChannel final : public rudp::HandshakeChannel {
 public:
  explicit ExchangeChannel(test_support::FakeClock& clock) noexcept
      : clock_(clock) {}

  auto send(std::span<const std::byte> bytes) -> Sent override {
    sent_.emplace_back(bytes.begin(), bytes.end());
    if (send_error_) return libchiaking::unexpected(*send_error_);
    if (short_send_ ||
        (short_send_on_call_ != 0 && sent_.size() == short_send_on_call_))
      return std::size_t{0};
    return bytes.size();
  }
  auto receive_for(std::chrono::microseconds timeout,
                   const platform::StopToken&) -> Received override {
    if (on_receive_) on_receive_();
    if (receive_error_) return libchiaking::unexpected(*receive_error_);
    if (!inbox_.empty()) {
      Datagram next = std::move(inbox_.front());
      inbox_.pop_front();
      return std::optional<Datagram>{std::move(next)};
    }
    clock_.now_us += static_cast<std::uint64_t>(timeout.count());
    return std::optional<Datagram>{};
  }

  auto inbox() noexcept -> std::deque<Datagram>& { return inbox_; }
  auto sent() const noexcept -> const std::vector<std::vector<std::byte>>& {
    return sent_;
  }
  void send_error(std::optional<datagram::TransportError> error) noexcept {
    send_error_ = error;
  }
  void receive_error(std::optional<datagram::TransportError> error) noexcept {
    receive_error_ = error;
  }
  void short_send(bool shortened) noexcept { short_send_ = shortened; }
  void short_send_on_call(std::size_t call) noexcept {
    short_send_on_call_ = call;
  }
  void on_receive(std::function<void()> hook) { on_receive_ = std::move(hook); }

  void queue_session_init_round(std::uint16_t remote, std::string_view http) {
    auto init = counter_bytes(remote);
    init.insert(init.end(), 6, std::byte{0});
    init.push_back(std::byte{0xC0});
    init.push_back(std::byte{0x0E});
    inbox_.push_back(packet(0xD000, std::move(init)));
    inbox_.push_back(
        packet(0xA030, counter_bytes(static_cast<std::uint16_t>(remote + 1))));
    auto response = counter_bytes(static_cast<std::uint16_t>(remote + 2));
    for (const char character : http)
      response.push_back(static_cast<std::byte>(character));
    inbox_.push_back(packet(0x0230, std::move(response)));
    inbox_.push_back(packet(0xC000, {}));
  }

  auto count_frames(std::uint16_t type) const -> std::size_t {
    std::size_t count = 0;
    for (const auto& sent : sent_) {
      const auto frames = rudp::FrameCodec::decode(sent);
      EXPECT_TRUE(frames);
      if (frames && frames.value().front().type == type) ++count;
    }
    return count;
  }

  auto sent_contains(std::string_view text) const -> bool {
    return std::ranges::any_of(sent_, [text](const auto& sent) {
      const std::string_view bytes{reinterpret_cast<const char*>(sent.data()),
                                   sent.size()};
      return bytes.find(text) != std::string_view::npos;
    });
  }

  static auto counter_bytes(std::uint16_t value) -> std::vector<std::byte> {
    return {static_cast<std::byte>(value >> 8),
            static_cast<std::byte>(value & 0xFF)};
  }

  static auto packet(std::uint16_t type, std::vector<std::byte> payload)
      -> Datagram {
    const rudp::Frame frame{type, std::move(payload)};
    auto encoded = rudp::FrameCodec::encode({&frame, 1});
    EXPECT_TRUE(encoded);
    if (!encoded) return {};
    return std::move(encoded.value());
  }

 private:
  test_support::FakeClock& clock_;
  std::deque<Datagram> inbox_;
  std::optional<datagram::TransportError> send_error_;
  std::optional<datagram::TransportError> receive_error_;
  bool short_send_{};
  std::size_t short_send_on_call_{};
  std::vector<std::vector<std::byte>> sent_;
  std::function<void()> on_receive_;
};

class SwitchChannel final : public rudp::HandshakeChannel {
 public:
  auto send(std::span<const std::byte> bytes) -> Sent override {
    sent_.assign(bytes.begin(), bytes.end());
    sent_packets_.push_back(sent_);
    if (on_send_) on_send_();
    if (fail_send_)
      return libchiaking::unexpected(datagram::TransportError::io_failure);
    return short_send_ ? std::size_t{0} : bytes.size();
  }
  auto receive_for(std::chrono::microseconds, const platform::StopToken&)
      -> Received override {
    if (fail_receive_)
      return libchiaking::unexpected(datagram::TransportError::io_failure);
    return std::exchange(inbox_, std::nullopt);
  }

  void deliver(Datagram next) { inbox_ = std::move(next); }
  auto sent() const noexcept -> const std::vector<std::byte>& { return sent_; }
  auto sent_packets() const noexcept
      -> const std::vector<std::vector<std::byte>>& {
    return sent_packets_;
  }
  void on_send(std::function<void()> hook) { on_send_ = std::move(hook); }
  void fail_send(bool failing) noexcept { fail_send_ = failing; }
  void short_send(bool shortened) noexcept { short_send_ = shortened; }
  void fail_receive(bool failing) noexcept { fail_receive_ = failing; }

  static auto switch_packet() -> Datagram {
    auto ctrl = remote_play::CtrlCodec::encode_frame_header(0x34, 0);
    EXPECT_TRUE(ctrl);
    if (!ctrl) return {};
    std::vector<std::byte> payload{std::byte{0x56}, std::byte{0x77}};
    payload.insert(payload.end(), ctrl.value().begin(), ctrl.value().end());
    return ExchangeChannel::packet(0x0230, std::move(payload));
  }

 private:
  std::optional<Datagram> inbox_;
  std::vector<std::byte> sent_;
  std::vector<std::vector<std::byte>> sent_packets_;
  std::function<void()> on_send_;
  bool fail_send_{};
  bool short_send_{};
  bool fail_receive_{};
};

class CtrlFrames {
 public:
  static auto stream_switch_header() -> std::vector<std::byte> {
    auto header = remote_play::CtrlCodec::encode_frame_header(0x0034, 0);
    EXPECT_TRUE(header);
    if (!header) return {};
    return {header.value().begin(), header.value().end()};
  }

  static auto ongoing(std::uint16_t type, std::span<const std::byte> payload)
      -> remote_play::RudpOngoingCtrl {
    remote_play::RudpOngoingCtrl in{};
    auto header =
        remote_play::CtrlCodec::encode_frame_header(type, payload.size());
    EXPECT_TRUE(header);
    if (!header) return in;
    in.ctrl_frame.assign(header.value().begin(), header.value().end());
    in.ctrl_frame.insert(in.ctrl_frame.end(), payload.begin(), payload.end());
    in.remote_counter = 41;
    in.ack_counter = std::uint16_t{12};
    in.subtype = rudp::Subtype::ctrl_message;
    return in;
  }

  static auto exchange_outcome(std::string_view input)
      -> remote_play::RudpCtrlHttpExchangeOutcome {
    remote_play::RudpCtrlHttpExchangeOutcome out{};
    const auto text = std::as_bytes(std::span{input});
    out.response.received.assign(text.begin(), text.end());
    out.response.header_size = input.size();
    out.next_local_counter = 41;
    out.remote_counter = 7;
    return out;
  }

  static constexpr std::array<std::byte, 16> registration_key{
      std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
      std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07},
      std::byte{0x08}, std::byte{0x09}, std::byte{0x0a}, std::byte{0x0b},
      std::byte{0x0c}, std::byte{0x0d}, std::byte{0x0e}, std::byte{0x0f}};

  static constexpr std::string_view accepted_http =
      "HTTP/1.1 200 OK\r\n"
      "RP-Nonce: AAECAwQFBgcICQoLDA0ODw==\r\n\r\n";
};

}  // namespace play_test::rudp_ctrl
