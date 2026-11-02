/// @file
/// @brief Declares SecretArray, a fixed-size buffer for key material that wipes
/// itself.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <libchiaking/detail/secure_zero.hpp>
#include <span>
#include <type_traits>

namespace libchiaking {

/// @brief A fixed-size, inline buffer for a secret such as a key, zeroed
/// whenever its contents stop being needed.
///
/// The bytes are zeroed with memory::secure_zero when the array is destroyed,
/// when it is moved from, when it is move-assigned over, and on clear(). It
/// cannot be copied; clone() makes the one deliberate kind of copy, so every
/// duplicate of a secret is visible in the code. The storage lives inside the
/// object and nothing is allocated.
///
/// @tparam Size The number of elements; must be greater than zero.
/// @tparam Byte The element type, `std::byte` or `char`.
/// @note Pointers, references and spans into the array are plain views.
///     Anything copied out through them is no longer protected.
template <std::size_t Size, class Byte = std::byte>
class SecretArray {
  static_assert(Size > 0, "SecretArray requires nonzero storage");
  static_assert(std::is_same_v<Byte, std::byte> || std::is_same_v<Byte, char>,
                "Byte must be std::byte or char");

  std::array<Byte, Size> storage_{};

 public:
  /// @brief Makes an array of zero bytes.
  SecretArray() noexcept = default;

  /// @brief Not copyable; use clone() so each copy of a secret is explicit.
  SecretArray(const SecretArray&) = delete;
  /// @brief Not copy-assignable; use clone() so each copy of a secret is
  /// explicit.
  SecretArray& operator=(const SecretArray&) = delete;

  /// @brief Takes the bytes of another array and zeroes that array.
  /// @param other The array to take from; all zero afterwards.
  SecretArray(SecretArray&& other) noexcept {
    std::ranges::copy(other, begin());
    other.clear();
  }

  /// @brief Zeroes the current bytes, takes the bytes of another array and
  /// zeroes that array.
  /// @param other The array to take from; all zero afterwards. Assigning an
  ///     array to itself changes nothing.
  /// @return This array.
  SecretArray& operator=(SecretArray&& other) noexcept {
    if (this != &other) {
      clear();
      std::ranges::copy(other, begin());
      other.clear();
    }
    return *this;
  }

  /// @brief Zeroes the bytes before the storage is released.
  ~SecretArray() { clear(); }

  /// @brief Makes a second, independent copy of the secret.
  /// @return A new array holding the same bytes, wiped on its own schedule.
  auto clone() const noexcept -> SecretArray {
    SecretArray copy;
    std::ranges::copy(storage_, copy.begin());
    return copy;
  }

  /// @brief Pointer to the first element, for writing the secret in place.
  /// @return A pointer to Size writable elements inside this object, valid
  ///     until the array is destroyed; never null.
  Byte* data() noexcept { return storage_.data(); }
  /// @brief Pointer to the first element, read-only.
  /// @return A pointer to Size elements inside this object, valid until the
  ///     array is destroyed; never null.
  const Byte* data() const noexcept { return storage_.data(); }
  /// @brief The number of elements, always Size.
  /// @return Size; a cleared array keeps its size and holds zeros.
  constexpr std::size_t size() const noexcept { return Size; }
  /// @brief Iterator to the first element.
  /// @return The same pointer as data().
  Byte* begin() noexcept { return data(); }
  /// @brief Iterator one past the last element.
  /// @return data() plus Size; not dereferenceable.
  Byte* end() noexcept { return data() + Size; }
  /// @brief Read-only iterator to the first element.
  /// @return The same pointer as data().
  const Byte* begin() const noexcept { return data(); }
  /// @brief Read-only iterator one past the last element.
  /// @return data() plus Size; not dereferenceable.
  const Byte* end() const noexcept { return data() + Size; }
  /// @brief The element at an index.
  /// @param i Index below Size; it is not checked.
  /// @return A reference to that element, valid until the array is
  ///     destroyed.
  Byte& operator[](std::size_t i) noexcept { return storage_[i]; }
  /// @brief The element at an index, read-only.
  /// @param i Index below Size; it is not checked.
  /// @return A read-only reference to that element, valid until the array
  ///     is destroyed.
  const Byte& operator[](std::size_t i) const noexcept { return storage_[i]; }

  /// @brief The whole array as a writable fixed-extent span.
  /// @return A view of all Size elements, valid until the array is
  ///     destroyed; it sees the zeros after a clear or a move.
  std::span<Byte, Size> span() noexcept {
    return std::span<Byte, Size>{storage_};
  }
  /// @brief The whole array as a read-only fixed-extent span.
  /// @return A view of all Size elements, valid until the array is
  ///     destroyed; it sees the zeros after a clear or a move.
  std::span<const Byte, Size> span() const noexcept {
    return std::span<const Byte, Size>{storage_};
  }

  /// @brief Drops elements from the front of the part in use, moving the
  /// rest down and zeroing the places they leave behind.
  /// @param count How many elements to drop; at most used.
  /// @param used How many elements at the front hold data; at most Size.
  void drop_front(std::size_t count, std::size_t used) noexcept {
    const auto in_use = span().first(used);
    const auto kept =
        std::ranges::copy(in_use.subspan(count), in_use.begin()).out;
    memory::secure_zero(std::span<Byte>{kept, in_use.end()});
  }

  /// @brief Sets every element to one value.
  /// @param v The value to write.
  void fill(Byte v) noexcept { storage_.fill(v); }
  /// @brief Zeroes every element now, with memory::secure_zero.
  void clear() noexcept {
    memory::secure_zero(std::as_writable_bytes(std::span{storage_}));
  }
};

}  // namespace libchiaking
