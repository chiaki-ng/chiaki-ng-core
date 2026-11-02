#include <libchiaking/consoles_types.hpp>
#include <libchiaking/users_types.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace memory_test {
namespace {

TEST(SecureZero, ClearsEveryByte) {
  std::array<std::byte, 9> buffer{};
  buffer.fill(std::byte{0x5a});
  libchiaking::memory::secure_zero(std::span<std::byte>{buffer});
  EXPECT_TRUE(std::ranges::all_of(
      buffer, [](std::byte value) { return value == std::byte{}; }));
  std::array<char, 4> text{'a', 'b', 'c', 'd'};
  libchiaking::memory::secure_zero(std::span<char>{text});
  EXPECT_TRUE(
      std::ranges::all_of(text, [](char value) { return value == '\0'; }));
  libchiaking::memory::secure_zero(std::span<std::byte>{});
}

}  // namespace
}  // namespace memory_test
