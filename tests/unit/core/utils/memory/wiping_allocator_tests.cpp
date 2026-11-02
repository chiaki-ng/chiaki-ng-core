#include "tests/unit/core/utils/memory/inspecting_allocator.hpp"

#include <libchiaking/consoles_types.hpp>
#include <libchiaking/users_types.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <type_traits>
#include <vector>

namespace memory_test {
namespace {

class WipingAllocator : public ::testing::Test {
 protected:
  template <class T, class Base = std::allocator<T>>
  using Wiping = libchiaking::memory::WipingAllocator<T, Base>;
  using Inspected = Wiping<char, InspectingAllocator<char>>;
  using Rebound = std::allocator_traits<Inspected>::rebind_alloc<int>;

  void SetUp() override { ReleasedBlocks::reset(); }
};

TEST_F(WipingAllocator, RebindsAndComparesEqual) {
  using Alloc = Wiping<char>;
  static_assert(std::is_same_v<std::allocator_traits<Alloc>::value_type, char>);
  static_assert(std::is_nothrow_default_constructible_v<Alloc>);
  static_assert(std::is_same_v<Rebound, Wiping<int, InspectingAllocator<int>>>);
  EXPECT_EQ(Alloc{}, Wiping<long>{});
}

TEST_F(WipingAllocator, WipesEveryBlockAReboundContainerReleases) {
  {
    std::vector<int, Rebound> words;
    for (int index = 1; index <= 100; ++index) words.push_back(index);
    ASSERT_EQ(words.size(), 100U);
    EXPECT_EQ(words[99], 100);
  }
  EXPECT_GT(ReleasedBlocks::blocks(), 1U);
  EXPECT_EQ(ReleasedBlocks::dirty(), 0U);
}

}  // namespace
}  // namespace memory_test
