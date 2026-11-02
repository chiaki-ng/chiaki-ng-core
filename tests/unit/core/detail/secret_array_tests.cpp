#include "libchiaking/consoles_types.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace detail_test {
namespace {

using libchiaking::SecretArray;

static_assert(!std::is_copy_constructible_v<SecretArray<4>>);
static_assert(!std::is_copy_assignable_v<SecretArray<4>>);
static_assert(std::is_nothrow_move_constructible_v<SecretArray<4>>);
static_assert(std::is_nothrow_move_assignable_v<SecretArray<4>>);

class SecretArrayTest : public ::testing::Test {
 protected:
  static auto all_zero(std::span<const std::byte> bytes) -> bool {
    return std::ranges::all_of(
        bytes, [](std::byte value) { return value == std::byte{}; });
  }

  void SetUp() override { held.fill(std::byte{0xAB}); }

  SecretArray<8> held;
};

TEST_F(SecretArrayTest, IsFilledEdgeToEdge) {
  EXPECT_EQ(held[0], std::byte{0xAB});
  EXPECT_EQ(held[7], std::byte{0xAB});
}

TEST_F(SecretArrayTest, WipesTheSourceWhenMoveConstructed) {
  SecretArray<8> moved(std::move(held));
  EXPECT_EQ(moved[0], std::byte{0xAB});
  EXPECT_TRUE(all_zero(held));
}

TEST_F(SecretArrayTest,
       WipesTheSourceAndReplacesTheOldContentsWhenMoveAssigned) {
  SecretArray<8> target;
  target.fill(std::byte{0x11});
  target = std::move(held);
  EXPECT_EQ(target[0], std::byte{0xAB});
  EXPECT_TRUE(all_zero(held));
}

TEST_F(SecretArrayTest, KeepsItsContentsWhenMovedOntoItself) {
  SecretArray<8>& alias = held;
  held = std::move(alias);
  EXPECT_EQ(held[0], std::byte{0xAB});
}

TEST_F(SecretArrayTest, LendsItsBytesAsASpan) {
  std::span<const std::byte> borrowed = held;
  EXPECT_EQ(borrowed.size(), 8U);
  EXPECT_EQ(borrowed[3], std::byte{0xAB});
}

TEST_F(SecretArrayTest, HoldsCharactersAndWipesThemWhenMoved) {
  SecretArray<4, char> source;
  source.fill('k');
  EXPECT_EQ(source.size(), 4U);
  EXPECT_EQ(source[3], 'k');
  SecretArray<4, char> moved(std::move(source));
  EXPECT_EQ(moved[0], 'k');
  EXPECT_EQ(source[0], '\0');
}

TEST_F(SecretArrayTest, ClearsEveryByteUpToTheLast) {
  held.clear();
  EXPECT_TRUE(all_zero(held));
  EXPECT_EQ(held[held.size() - 1], std::byte{0});
}

TEST_F(SecretArrayTest, WipesItsStorageWhenDestroyed) {
  auto* buffer = static_cast<unsigned char*>(::operator new(
      sizeof(SecretArray<8>), std::align_val_t{alignof(SecretArray<8>)}));
  auto* placed = new (buffer) SecretArray<8>;
  placed->fill(std::byte{0xCD});
  const volatile unsigned char* before = buffer;
  EXPECT_EQ(before[0], 0xCD) << "secret storage was not populated";
  placed->~SecretArray<8>();
  const volatile unsigned char* observed = buffer;
  for (std::size_t i = 0; i < sizeof(SecretArray<8>); ++i)
    EXPECT_EQ(observed[i], 0) << "secret byte " << i << " survived destruction";
  ::operator delete(buffer, std::align_val_t{alignof(SecretArray<8>)});
}

}  // namespace
}  // namespace detail_test
