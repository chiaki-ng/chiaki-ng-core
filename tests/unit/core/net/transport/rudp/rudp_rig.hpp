#pragma once

#include "src/core/net/transport/datagram/transport.hpp"
#include "src/core/net/transport/rudp/rudp.hpp"
#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/runtime/random_provider.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace rudp_test {

class ScriptedChannel final
    : public libchiaking::net::transport::rudp::HandshakeChannel {
 public:
  using Datagram = libchiaking::net::transport::datagram::OwnedDatagram;
  using TransportError = libchiaking::net::transport::datagram::TransportError;

  explicit ScriptedChannel(test_support::FakeClock& clock) noexcept
      : clock_(&clock) {}

  [[nodiscard]] auto send(std::span<const std::byte> datagram)
      -> libchiaking::Expected<std::size_t, TransportError> override {
    sent.emplace_back(datagram.begin(), datagram.end());
    if (send_error) return libchiaking::unexpected(*send_error);
    if (short_send) return std::size_t{0};
    return datagram.size();
  }

  [[nodiscard]] auto receive_for(std::chrono::microseconds timeout,
                                 const libchiaking::platform::StopToken&)
      -> libchiaking::Expected<std::optional<Datagram>,
                               TransportError> override {
    if (on_receive) on_receive();
    if (receive_error) return libchiaking::unexpected(*receive_error);
    if (!inbox.empty()) {
      Datagram next = std::move(inbox.front());
      inbox.pop_front();
      return std::optional<Datagram>{std::move(next)};
    }
    clock_->now_us += static_cast<std::uint64_t>(timeout.count());
    return std::optional<Datagram>{};
  }

  std::deque<Datagram> inbox;
  std::optional<TransportError> send_error;
  std::optional<TransportError> receive_error;
  bool short_send{};
  std::vector<std::vector<std::byte>> sent;
  std::function<void()> on_receive;

 private:
  test_support::FakeClock* clock_;
};

class ScriptedRandom final
    : public libchiaking::platform::SecureRandomProvider {
 public:
  [[nodiscard]] auto fill_bytes(std::span<std::byte> output)
      -> libchiaking::Expected<void,
                               libchiaking::platform::PlatformError> override {
    ++calls;
    if (fail)
      return libchiaking::unexpected(
          libchiaking::platform::PlatformError::io_failure);
    EXPECT_EQ(output.size(), expect_size);
    EXPECT_GE(next.size(), output.size());
    if (next.size() < output.size())
      return libchiaking::unexpected(
          libchiaking::platform::PlatformError::io_failure);
    std::copy_n(next.begin(), output.size(), output.begin());
    return {};
  }

  void words(std::uint32_t first, std::uint32_t second) {
    next = {
        static_cast<std::byte>(first >> 24U),
        static_cast<std::byte>(first >> 16U),
        static_cast<std::byte>(first >> 8U),
        static_cast<std::byte>(first),
        static_cast<std::byte>(second >> 24U),
        static_cast<std::byte>(second >> 16U),
        static_cast<std::byte>(second >> 8U),
        static_cast<std::byte>(second),
    };
  }

  void counting_nonce() {
    expect_size = 16;
    next.clear();
    for (std::uint8_t value = 1; value <= 16; ++value)
      next.push_back(static_cast<std::byte>(value));
  }

  std::vector<std::byte> next;
  bool fail{};
  int calls{};
  std::size_t expect_size{8};
};

class RudpPackets {
 public:
  using Bytes = std::vector<std::byte>;
  using Frame = libchiaking::net::transport::rudp::Frame;

  static void push_be16(Bytes& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value >> 8U));
    out.push_back(static_cast<std::byte>(value & 0xFFU));
  }

  static auto encode_one(const Frame& frame) -> Bytes {
    const std::array<const Frame, 1> frames{frame};
    const auto encoded =
        libchiaking::net::transport::rudp::FrameCodec::encode(frames);
    EXPECT_TRUE(encoded);
    return encoded ? encoded.value() : Bytes{};
  }

  static auto init_response(std::uint16_t remote_counter,
                            std::span<const std::byte> cookie) -> Bytes {
    Bytes payload;
    push_be16(payload, remote_counter);
    payload.insert(payload.end(), 6, std::byte{0});
    payload.insert(payload.end(), cookie.begin(), cookie.end());
    return encode_one(Frame{0xD000, std::move(payload)});
  }

  static auto init_response(std::uint16_t remote_counter,
                            std::initializer_list<std::byte> cookie) -> Bytes {
    return init_response(remote_counter, std::span<const std::byte>{
                                             cookie.begin(), cookie.size()});
  }

  static auto cookie_response(std::uint16_t remote_counter) -> Bytes {
    Bytes payload;
    push_be16(payload, remote_counter);
    return encode_one(Frame{0xA030, std::move(payload)});
  }

  static auto handshake_request(std::uint16_t type, std::uint16_t counter,
                                std::span<const std::byte> cookie) -> Bytes {
    Bytes packet;
    push_be16(packet,
              static_cast<std::uint16_t>(0xC000U | (22U + cookie.size())));
    packet.insert(packet.end(), {std::byte{0x24}, std::byte{0x4F},
                                 std::byte{0x24}, std::byte{0x4F}});
    push_be16(packet, type);
    push_be16(packet, counter);
    packet.insert(
        packet.end(),
        {std::byte{0x0B}, std::byte{0x01}, std::byte{0x01}, std::byte{0x00},
         std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
         std::byte{0xAB}, std::byte{0xCD}, std::byte{0x05}, std::byte{0x82}});
    packet.insert(packet.end(), cookie.begin(), cookie.end());
    return packet;
  }
};

}  // namespace rudp_test
