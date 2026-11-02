#pragma once

#include "src/core/utils/bytes/byte_io.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

namespace bytes_test {

class Rig {
 public:
  static constexpr std::array<std::byte, 21> sample{
      std::byte{'R'},  std::byte{'C'},  std::byte{0x7f}, std::byte{0x12},
      std::byte{0x34}, std::byte{0x89}, std::byte{0xab}, std::byte{0xcd},
      std::byte{0xef}, std::byte{0x01}, std::byte{0x23}, std::byte{0x45},
      std::byte{0x67}, std::byte{0x89}, std::byte{0xab}, std::byte{0xcd},
      std::byte{0xef}, std::byte{0x80}, std::byte{0x00}, std::byte{0xff},
      std::byte{0xfe}};

  static auto same(const libchiaking::bytes::Shortfall& shortfall,
                   std::size_t offset, std::size_t needed,
                   std::size_t available) -> testing::AssertionResult {
    if (shortfall.offset == offset && shortfall.needed == needed &&
        shortfall.available == available)
      return testing::AssertionSuccess();
    return testing::AssertionFailure()
           << "shortfall {offset " << shortfall.offset << ", needed "
           << shortfall.needed << ", available " << shortfall.available
           << "} is not {offset " << offset << ", needed " << needed
           << ", available " << available << "}";
  }

  template <class Result>
  static auto refused(const Result& result, std::size_t offset,
                      std::size_t needed, std::size_t available)
      -> testing::AssertionResult {
    if (result) return testing::AssertionFailure() << "was not refused";
    return same(result.error(), offset, needed, available);
  }
};

}  // namespace bytes_test
