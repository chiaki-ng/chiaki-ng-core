/// @file
/// @brief Declares memory::secure_zero, the one primitive the library uses to
/// erase secrets from memory.
#pragma once

#include <cstddef>
#include <span>

namespace libchiaking::memory {

/// @brief Overwrites memory with zeros in a way the compiler is not allowed to
/// optimise away.
///
/// Uses the platform's own primitive where there is one (`SecureZeroMemory`,
/// `memset_s`, `explicit_bzero`) and a volatile write loop elsewhere.
///
/// @param bytes The memory to zero. An empty span is accepted and does nothing.
void secure_zero(std::span<std::byte> bytes) noexcept;

/// @brief Zeroes the storage of a span of objects, byte by byte.
/// @tparam T The element type; its object representation is overwritten.
/// @param values The objects to zero.
template <class T>
void secure_zero(std::span<T> values) noexcept {
  secure_zero(std::as_writable_bytes(values));
}

}  // namespace libchiaking::memory
