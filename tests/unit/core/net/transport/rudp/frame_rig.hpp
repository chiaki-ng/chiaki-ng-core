#pragma once

#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rudp_test {

class FrameCodecRig : public ::testing::Test {
 protected:
  using Bytes = std::vector<std::byte>;
  using FrameCodec = libchiaking::net::transport::rudp::FrameCodec;
  using FrameError = libchiaking::net::transport::rudp::FrameError;
  using Frame = libchiaking::net::transport::rudp::Frame;

  static void append_frame(Bytes& out, std::uint16_t type,
                           const Bytes& payload) {
    const auto length = static_cast<std::uint16_t>(8 + payload.size());
    const auto field = static_cast<std::uint16_t>(0xC000U | length);
    out.push_back(static_cast<std::byte>(field >> 8U));
    out.push_back(static_cast<std::byte>(field & 0xFFU));
    out.push_back(std::byte{0x24});
    out.push_back(std::byte{0x4f});
    out.push_back(std::byte{0x24});
    out.push_back(std::byte{0x4f});
    out.push_back(static_cast<std::byte>(type >> 8U));
    out.push_back(static_cast<std::byte>(type & 0xFFU));
    out.insert(out.end(), payload.begin(), payload.end());
  }

  static auto packet_of(std::uint16_t type, const Bytes& payload) -> Bytes {
    Bytes packet;
    append_frame(packet, type, payload);
    return packet;
  }

  static inline const Bytes golden = {
      std::byte{0xc0}, std::byte{0x0a}, std::byte{0x24}, std::byte{0x4f},
      std::byte{0x24}, std::byte{0x4f}, std::byte{0x80}, std::byte{0x30},
      std::byte{0x12}, std::byte{0x34}};

  static inline const Bytes golden_init = {
      std::byte{0xc0}, std::byte{0x16}, std::byte{0x24}, std::byte{0x4f},
      std::byte{0x24}, std::byte{0x4f}, std::byte{0x80}, std::byte{0x30},
      std::byte{0x12}, std::byte{0x34}, std::byte{0x0b}, std::byte{0x01},
      std::byte{0x01}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00},
      std::byte{0x89}, std::byte{0xab}, std::byte{0xcd}, std::byte{0xef},
      std::byte{0x05}, std::byte{0x82}};

  static inline const Bytes golden_cookie = {
      std::byte{0xc0}, std::byte{0x18}, std::byte{0x24}, std::byte{0x4f},
      std::byte{0x24}, std::byte{0x4f}, std::byte{0x90}, std::byte{0x30},
      std::byte{0x12}, std::byte{0x34}, std::byte{0x0b}, std::byte{0x01},
      std::byte{0x01}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00},
      std::byte{0x89}, std::byte{0xab}, std::byte{0xcd}, std::byte{0xef},
      std::byte{0x05}, std::byte{0x82}, std::byte{0xaa}, std::byte{0xbb}};

  static inline const Bytes golden_switch = {
      std::byte{0xc0}, std::byte{0x22}, std::byte{0x24}, std::byte{0x4f},
      std::byte{0x24}, std::byte{0x4f}, std::byte{0x02}, std::byte{0x30},
      std::byte{0x12}, std::byte{0x34}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x10}, std::byte{0x00}, std::byte{0x0d},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
      std::byte{0x02}, std::byte{0x03}, std::byte{0x04}, std::byte{0x05},
      std::byte{0x06}, std::byte{0x07}, std::byte{0x08}, std::byte{0x09},
      std::byte{0x0a}, std::byte{0x0b}, std::byte{0x0c}, std::byte{0x0d},
      std::byte{0x0e}, std::byte{0x0f}};
};

}  // namespace rudp_test
