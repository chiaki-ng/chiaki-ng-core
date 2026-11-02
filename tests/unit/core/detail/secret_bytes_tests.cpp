#include "libchiaking/users_types.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
#define LIBCHIAKING_TEST_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define LIBCHIAKING_TEST_SANITIZED 1
#endif

namespace detail_test {
namespace {

using libchiaking::SecretBytes;

static_assert(!std::is_copy_constructible_v<SecretBytes>);
static_assert(!std::is_copy_assignable_v<SecretBytes>);
static_assert(std::is_nothrow_move_constructible_v<SecretBytes>);
static_assert(std::is_nothrow_move_assignable_v<SecretBytes>);

class AllocationProbe {
 public:
  static void fail_next() noexcept {
    fail_ = true;
    consumed_ = false;
  }
  static auto consume_failure() noexcept -> bool {
    if (!std::exchange(fail_, false)) return false;
    consumed_ = true;
    return true;
  }
  static auto consumed() noexcept -> bool { return consumed_; }
  static void watch(const std::byte* pointer, std::size_t size) noexcept {
    watched_ = pointer;
    size_ = size;
    wiped_ = false;
  }
  static void inspect(void* pointer) noexcept {
    if (pointer == nullptr || pointer != watched_) return;
    const auto bytes =
        std::span{static_cast<const unsigned char*>(pointer), size_};
    wiped_ = std::ranges::all_of(bytes,
                                 [](unsigned char byte) { return byte == 0; });
    watched_ = nullptr;
  }
  static auto wiped() noexcept -> bool { return wiped_; }

 private:
  static inline bool fail_{};
  static inline bool consumed_{};
  static inline const void* watched_{};
  static inline std::size_t size_{};
  static inline bool wiped_{};
};

class SecretBytesTest : public ::testing::Test {
 protected:
  static constexpr std::array seed{std::byte{1}, std::byte{2}, std::byte{3},
                                   std::byte{4}};

  static auto make_bytes() -> SecretBytes {
    auto made = SecretBytes::from_bytes(seed);
    EXPECT_TRUE(made);
    return std::move(made).value();
  }

  static auto all_zero(std::span<const std::byte> bytes) -> bool {
    return std::ranges::all_of(
        bytes, [](std::byte value) { return value == std::byte{}; });
  }

  static auto holds_seed(const SecretBytes& bytes) -> bool {
    return std::ranges::equal(bytes.span(), seed);
  }
};

TEST_F(SecretBytesTest, IsEmptyWhenMadeFromNoBytes) {
  const auto made = SecretBytes::from_bytes({});
  ASSERT_TRUE(made);
  EXPECT_TRUE(made.value().empty());
}

TEST_F(SecretBytesTest, ClonesIntoAnIndependentCopy) {
  auto owner = make_bytes();
  auto cloned = owner.try_clone();
  ASSERT_TRUE(cloned);
  owner[0] = std::byte{9};
  EXPECT_EQ(cloned.value()[0], seed[0]);
}

TEST_F(SecretBytesTest, AppendsAndAssignsFromItsOwnBytes) {
  auto owner = make_bytes();
  ASSERT_TRUE(owner.try_assign(seed));
  ASSERT_TRUE(owner.try_append(owner.span().subspan(1, 2)));
  constexpr std::array appended{std::byte{1}, std::byte{2}, std::byte{3},
                                std::byte{4}, std::byte{2}, std::byte{3}};
  EXPECT_TRUE(std::ranges::equal(owner.span(), appended));
  ASSERT_TRUE(owner.try_assign(owner.span().subspan(1, 3)));
  EXPECT_TRUE(std::ranges::equal(owner.span(), std::span{seed}.subspan(1)));
}

TEST_F(SecretBytesTest, LeavesTheSourceEmptyWhenMoved) {
  auto owner = make_bytes();
  auto moved = std::move(owner);
  EXPECT_TRUE(owner.empty());
  EXPECT_EQ(moved.size(), 4U);
}

TEST_F(SecretBytesTest, WipesWhatShrinkingAndClearingLeaveBehind) {
  auto owner = make_bytes();
  ASSERT_TRUE(owner.try_assign(std::span{seed}.subspan(1)));
  auto moved = std::move(owner);
  ASSERT_EQ(moved.size(), 3U);
  const auto* memory = reinterpret_cast<const unsigned char*>(moved.data());
  ASSERT_TRUE(moved.try_resize(1));
  EXPECT_EQ(memory[1], 0);
  EXPECT_EQ(memory[2], 0);
  ASSERT_TRUE(moved.try_resize(3));
  EXPECT_EQ(moved[1], std::byte{});
  EXPECT_EQ(moved[2], std::byte{});
  moved.clear();
  EXPECT_TRUE(moved.empty());
  EXPECT_EQ(memory[0], 0);
  EXPECT_EQ(memory[1], 0);
  EXPECT_EQ(memory[2], 0);
  moved.clear();
  EXPECT_TRUE(moved.empty());
}

TEST_F(SecretBytesTest, ClearsWhenAlreadyEmpty) {
  SecretBytes empty;
  empty.clear();
  EXPECT_TRUE(empty.empty());
}

TEST_F(SecretBytesTest, IsMadeZeroedToASize) {
  const auto blank = SecretBytes::zeroed(3);
  EXPECT_EQ(blank.size(), 3U);
  EXPECT_TRUE(all_zero(blank.span()));
}

#if !defined(LIBCHIAKING_TEST_SANITIZED)

class SecretBytesRelease : public SecretBytesTest {};

TEST_F(SecretBytesRelease, WipesItsBufferWhenDestroyed) {
  {
    auto owner = make_bytes();
    AllocationProbe::watch(owner.data(), owner.size());
  }
  EXPECT_TRUE(AllocationProbe::wiped());
}

TEST_F(SecretBytesRelease, WipesTheOldBufferWhenItGrows) {
  auto owner = make_bytes();
  AllocationProbe::watch(owner.data(), owner.size());
  ASSERT_TRUE(owner.try_resize(512));
  EXPECT_TRUE(AllocationProbe::wiped());
  EXPECT_TRUE(std::ranges::equal(owner.span().first(seed.size()), seed));
}

TEST_F(SecretBytesRelease, WipesTheReplacedBufferWhenMoveAssigned) {
  auto target = make_bytes();
  auto source = make_bytes();
  AllocationProbe::watch(target.data(), target.size());
  target = std::move(source);
  EXPECT_TRUE(AllocationProbe::wiped());
  EXPECT_TRUE(source.empty());
  EXPECT_TRUE(holds_seed(target));
}

TEST_F(SecretBytesRelease, TheWipingAllocatorWipesWhatItFrees) {
  libchiaking::memory::WipingAllocator<std::uint32_t> allocator;
  auto* words = allocator.allocate(4);
  std::fill_n(words, 4, 0xabcdefU);
  AllocationProbe::watch(reinterpret_cast<const std::byte*>(words),
                         4 * sizeof(*words));
  allocator.deallocate(words, 4);
  EXPECT_TRUE(AllocationProbe::wiped());
}

TEST_F(SecretBytesRelease, ReportsAFailedAllocationWhenMade) {
  AllocationProbe::fail_next();
  const auto failed = SecretBytes::from_bytes(seed);
  EXPECT_TRUE(AllocationProbe::consumed());
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), SecretBytes::Error::allocation_failed);
}

TEST_F(SecretBytesRelease, ReportsAFailedAllocationWhenCloned) {
  auto owner = make_bytes();
  AllocationProbe::fail_next();
  EXPECT_FALSE(owner.try_clone());
  EXPECT_TRUE(AllocationProbe::consumed());
}

TEST_F(SecretBytesRelease, KeepsItsBytesWhenAnAppendCannotAllocate) {
  auto owner = make_bytes();
  AllocationProbe::fail_next();
  EXPECT_FALSE(owner.try_append(owner.span()));
  EXPECT_TRUE(AllocationProbe::consumed());
  EXPECT_TRUE(holds_seed(owner));
}

TEST_F(SecretBytesRelease, KeepsItsBytesWhenAnAssignCannotAllocate) {
  auto owner = make_bytes();
  AllocationProbe::fail_next();
  EXPECT_FALSE(owner.try_assign(owner.span()));
  EXPECT_TRUE(AllocationProbe::consumed());
  EXPECT_TRUE(holds_seed(owner));
}

TEST_F(SecretBytesRelease, KeepsItsBytesWhenAResizeCannotAllocate) {
  auto owner = make_bytes();
  AllocationProbe::fail_next();
  EXPECT_FALSE(owner.try_resize(512));
  EXPECT_TRUE(AllocationProbe::consumed());
  EXPECT_TRUE(holds_seed(owner));
}

#endif

}  // namespace
}  // namespace detail_test

#if !defined(LIBCHIAKING_TEST_SANITIZED)
void* operator new(std::size_t size) {
  if (detail_test::AllocationProbe::consume_failure()) throw std::bad_alloc{};
  if (void* pointer = std::malloc(size == 0 ? 1 : size)) return pointer;
  throw std::bad_alloc{};
}

void operator delete(void* pointer) noexcept {
  detail_test::AllocationProbe::inspect(pointer);
  std::free(pointer);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* pointer, std::size_t) noexcept {
  operator delete(pointer);
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif
