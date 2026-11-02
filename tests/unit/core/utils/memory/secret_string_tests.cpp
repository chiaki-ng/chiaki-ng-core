#include "tests/unit/core/utils/memory/inspecting_allocator.hpp"

#include <libchiaking/consoles_types.hpp>
#include <libchiaking/users_types.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace memory_test {
namespace {

class SecretString : public ::testing::Test {
 protected:
  using Wiped = libchiaking::BasicSecret<
      char,
      libchiaking::memory::WipingAllocator<char, InspectingAllocator<char>>>;
  using Unwiped = libchiaking::BasicSecret<char, InspectingAllocator<char>>;

  void SetUp() override { ReleasedBlocks::reset(); }
};

TEST_F(SecretString, IsMoveOnlyAndNeverMadeImplicitlyFromText) {
  using libchiaking::SecretBytes;
  using Secret = libchiaking::SecretString;
  static_assert(!std::is_copy_constructible_v<Secret>);
  static_assert(!std::is_copy_assignable_v<Secret>);
  static_assert(std::is_nothrow_move_constructible_v<Secret>);
  static_assert(std::is_nothrow_move_assignable_v<Secret>);
  static_assert(!std::is_copy_constructible_v<SecretBytes>);
  static_assert(std::is_nothrow_move_constructible_v<SecretBytes>);
  static_assert(!std::is_convertible_v<std::string_view, Secret>);
}

TEST_F(SecretString, WipesItsStorageWhenDestroyed) {
  {
    Wiped secret{std::string_view{"destroy me"}};
    EXPECT_EQ(secret.view(), "destroy me");
  }
  EXPECT_EQ(ReleasedBlocks::blocks(), 1U);
  EXPECT_EQ(ReleasedBlocks::dirty(), 0U);

  ReleasedBlocks::reset();
  {
    Unwiped control{std::string_view{"left behind"}};
  }
  EXPECT_EQ(ReleasedBlocks::blocks(), 1U);
  EXPECT_EQ(ReleasedBlocks::dirty(), 1U);
}

TEST_F(SecretString, WipesEveryBufferItOutgrows) {
  {
    Wiped secret;
    for (int index = 0; index < 3000; ++index)
      secret.push_back(static_cast<char>('0' + index % 10));
    secret.append(secret.view());
    ASSERT_EQ(secret.size(), 6000U);
    EXPECT_EQ(secret.view()[5999], secret.view()[2999]);
  }
  EXPECT_GT(ReleasedBlocks::blocks(), 2U);
  EXPECT_EQ(ReleasedBlocks::dirty(), 0U);
}

TEST_F(SecretString, LeavesNothingInTheSourceWhenMoved) {
  Wiped source{std::string_view{"moving"}};
  const auto* storage = source.data();
  Wiped moved{std::move(source)};
  EXPECT_TRUE(source.empty());
  EXPECT_TRUE(source.view().empty());
  EXPECT_EQ(moved.data(), storage);
  EXPECT_EQ(moved.view(), "moving");
  EXPECT_EQ(ReleasedBlocks::blocks(), 0U);

  Wiped target{std::string_view{"overwritten"}};
  target = std::move(moved);
  EXPECT_EQ(target.view(), "moving");
  EXPECT_TRUE(moved.empty());
  EXPECT_EQ(ReleasedBlocks::blocks(), 1U);
  EXPECT_EQ(ReleasedBlocks::dirty(), 0U);
}

TEST_F(SecretString, ClonesIntoIndependentStorage) {
  const libchiaking::SecretString original{std::string_view{"token"}};
  auto copy = original.clone();
  EXPECT_EQ(copy.view(), "token");
  EXPECT_NE(copy.data(), original.data());
  copy.append("-more");
  EXPECT_EQ(original.view(), "token");
  EXPECT_EQ(copy.view(), "token-more");
}

TEST_F(SecretString, WipesItsLiveBytesWhenCleared) {
  {
    Unwiped secret{std::string_view{"cleared"}};
    secret.clear();
    EXPECT_TRUE(secret.empty());
    EXPECT_TRUE(secret.view().empty());
  }
  EXPECT_EQ(ReleasedBlocks::blocks(), 1U);
  EXPECT_EQ(ReleasedBlocks::dirty(), 0U);
}

TEST_F(SecretString, AssignsAndAppendsItsOwnView) {
  libchiaking::SecretString value;
  EXPECT_TRUE(value.empty());
  EXPECT_TRUE(value.view().empty());
  EXPECT_TRUE(value.bytes().empty());
  value.assign("xy");
  value.append(value.view());
  EXPECT_EQ(value.view(), "xyxy");
  value.assign(value.view());
  EXPECT_EQ(value.view(), "xyxy");
  value.assign(std::string_view{"a\0b", 3});
  ASSERT_EQ(value.size(), 3U);
  EXPECT_EQ(value.view()[1], '\0');
  std::size_t count{};
  for (const char character : value) {
    (void)character;
    ++count;
  }
  EXPECT_EQ(count, 3U);
}

TEST_F(SecretString, AgreesWithSecretBytesOnBytesAndViews) {
  const std::array raw{std::byte{'h'}, std::byte{'i'}, std::byte{0xff}};
  const libchiaking::SecretBytes body{std::span<const std::byte>{raw}};
  EXPECT_EQ(body.size(), 3U);
  EXPECT_EQ(body.view(), std::string_view("hi\xff", 3));
  EXPECT_TRUE(std::ranges::equal(body.bytes(), raw));
  const libchiaking::SecretBytes text{std::string_view{"{}"}};
  EXPECT_EQ(text.view(), "{}");
  EXPECT_EQ(text.data()[0], std::byte{'{'});
  const libchiaking::SecretString from_bytes{body.bytes()};
  EXPECT_EQ(from_bytes.view(), body.view());
}

}  // namespace
}  // namespace memory_test
