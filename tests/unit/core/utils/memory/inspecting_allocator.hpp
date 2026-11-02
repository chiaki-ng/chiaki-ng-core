#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>

namespace memory_test {

class ReleasedBlocks {
 public:
  static void reset() noexcept {
    blocks_ = 0;
    dirty_ = 0;
  }

  static void record(std::span<const std::byte> released) noexcept {
    ++blocks_;
    if (std::ranges::any_of(
            released, [](std::byte value) { return value != std::byte{}; }))
      ++dirty_;
  }

  static auto blocks() noexcept -> std::size_t { return blocks_; }
  static auto dirty() noexcept -> std::size_t { return dirty_; }

 private:
  static inline std::size_t blocks_{};
  static inline std::size_t dirty_{};
};

template <class T>
class InspectingAllocator {
 public:
  using value_type = T;

  InspectingAllocator() noexcept = default;

  template <class U>
  InspectingAllocator(const InspectingAllocator<U>&) noexcept {}

  [[nodiscard]] auto allocate(std::size_t count) -> T* {
    return std::allocator<T>{}.allocate(count);
  }

  void deallocate(T* pointer, std::size_t count) noexcept {
    ReleasedBlocks::record(std::as_bytes(std::span<const T>{pointer, count}));
    std::allocator<T>{}.deallocate(pointer, count);
  }

  template <class U>
  friend auto operator==(const InspectingAllocator&,
                         const InspectingAllocator<U>&) noexcept -> bool {
    return true;
  }
};

}  // namespace memory_test
