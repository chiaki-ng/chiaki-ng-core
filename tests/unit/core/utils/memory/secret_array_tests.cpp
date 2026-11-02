#include <libchiaking/consoles_types.hpp>
#include <libchiaking/users_types.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace memory_test {
namespace {

TEST(SecretArray, StillWipesTheSourceWhenMoved) {
  libchiaking::SecretArray<4> source;
  source.fill(std::byte{7});
  libchiaking::SecretArray<4> moved{std::move(source)};
  EXPECT_TRUE(std::ranges::all_of(
      source, [](std::byte value) { return value == std::byte{}; }));
  EXPECT_EQ(moved[3], std::byte{7});
}

}  // namespace
}  // namespace memory_test
