#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/media/counted_allocations.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <new>

auto operator new(std::size_t size) -> void* {
  play_test::CountedAllocations::count();
  if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept { std::free(memory); }

void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace play_test {
namespace {

TEST(CountedAllocations, CountsEveryNewOnThisThreadAndEverywhere) {
  const auto everywhere = CountedAllocations::everywhere();
  const auto here = CountedAllocations::on_this_thread();
  const auto held = std::make_unique<int>(7);
  EXPECT_EQ(CountedAllocations::everywhere(), everywhere + 1);
  EXPECT_EQ(CountedAllocations::on_this_thread(), here + 1);
  EXPECT_EQ(*held, 7);
}

}  // namespace
}  // namespace play_test
