/// @file
/// @brief Declares memory::WipingAllocator, the allocator that zeroes memory
/// before releasing it.
#pragma once

#include <libchiaking/detail/secure_zero.hpp>

#include <cstddef>
#include <memory>
#include <span>

namespace libchiaking::memory {

/// @brief A standard allocator that zeroes each block with secure_zero just
/// before handing it back.
///
/// Used by BasicSecret so that secret bytes never remain in freed heap memory,
/// including the old block left behind when a container grows. It keeps no
/// state: every instance is interchangeable with every other, so containers can
/// move their buffers freely.
///
/// @tparam T The element type allocated.
/// @tparam Upstream The allocator that really obtains and releases the memory.
///     It is default-constructed for each call, so it must be stateless.
template <class T, class Upstream = std::allocator<T>>
class WipingAllocator {
 public:
  /// @brief The element type allocated.
  using value_type = T;

  /// @brief Names the same allocator for another element type, as containers
  /// require.
  /// @tparam U The other element type.
  template <class U>
  struct rebind {
    /// @brief The allocator for U, layered on the upstream allocator
    /// rebound to U.
    using other = WipingAllocator<
        U, typename std::allocator_traits<Upstream>::template rebind_alloc<U>>;
  };

  /// @brief Makes an allocator; there is no state to set up.
  WipingAllocator() noexcept = default;

  /// @brief Converts from a wiping allocator for another element type; there
  /// is no state to carry over.
  template <class U, class OtherUpstream>
  WipingAllocator(const WipingAllocator<U, OtherUpstream>&) noexcept {}

  /// @brief Obtains uninitialised storage for a number of elements from the
  /// upstream allocator.
  /// @param count The number of elements.
  /// @return Pointer to the storage. Throws whatever the upstream allocator
  ///     throws, normally `std::bad_alloc`.
  [[nodiscard]] auto allocate(std::size_t count) -> T* {
    Upstream upstream;
    return std::allocator_traits<Upstream>::allocate(upstream, count);
  }

  /// @brief Zeroes the whole block and then releases it to the upstream
  /// allocator.
  /// @param pointer The block, as returned by allocate().
  /// @param count The element count that was passed to allocate(); all of it
  ///     is zeroed, not only the part in use.
  void deallocate(T* pointer, std::size_t count) noexcept {
    secure_zero(std::span<T>{pointer, count});
    Upstream upstream;
    std::allocator_traits<Upstream>::deallocate(upstream, pointer, count);
  }

  /// @brief Always true: any wiping allocator may release memory obtained by
  /// any other.
  template <class U, class OtherUpstream>
  friend constexpr auto operator==(
      const WipingAllocator&, const WipingAllocator<U, OtherUpstream>&) noexcept
      -> bool {
    return true;
  }
};

}  // namespace libchiaking::memory
