#pragma once

#include "src/core/net/nat/stun/binding_client.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

namespace nat_test {

class StunBindingRig : public ::testing::Test {
 protected:
  static constexpr std::array<std::byte, 12> transaction{
      std::byte{0xb7}, std::byte{0xe7}, std::byte{0x47}, std::byte{0x05},
      std::byte{0xbc}, std::byte{0x8a}, std::byte{0x31}, std::byte{0x59},
      std::byte{0x8c}, std::byte{0xf5}, std::byte{0x20}, std::byte{0x7a}};

  static auto valid_response() -> std::array<std::byte, 32> {
    return {std::byte{1},    std::byte{1},    std::byte{0},    std::byte{12},
            std::byte{0x21}, std::byte{0x12}, std::byte{0xa4}, std::byte{0x42},
            transaction[0],  transaction[1],  transaction[2],  transaction[3],
            transaction[4],  transaction[5],  transaction[6],  transaction[7],
            transaction[8],  transaction[9],  transaction[10], transaction[11],
            std::byte{0},    std::byte{1},    std::byte{0},    std::byte{8},
            std::byte{0},    std::byte{1},    std::byte{0x12}, std::byte{0x34},
            std::byte{192},  std::byte{0},    std::byte{2},    std::byte{1}};
  }

  static auto expected_mapping() -> libchiaking::platform::SocketEndpoint {
    return libchiaking::platform::SocketEndpoint{
        libchiaking::platform::IpAddress::ipv4({192, 0, 2, 1}), 0x1234};
  }
};

}  // namespace nat_test
