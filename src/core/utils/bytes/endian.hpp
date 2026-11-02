/// @file
/// @brief Declares the byte-order conversions between unsigned integers and
/// their big- or little-endian wire bytes, independent of the host's order.
#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <span>

namespace libchiaking::bytes {

/// @brief Reads a big-endian unsigned integer from the start of a span.
///
/// @tparam T The unsigned integer type to read.
/// @param bytes At least `sizeof(T)` bytes; only the first `sizeof(T)` are
///     read. A shorter span is undefined behaviour.
/// @return The value.
template <std::unsigned_integral T>
constexpr auto load_be(std::span<const std::byte> bytes) noexcept -> T {
  T value = 0;
  for (const auto byte : bytes.first(sizeof(T)))
    value = static_cast<T>((value << 8U) | std::to_integer<T>(byte));
  return value;
}

/// @brief Reads a little-endian unsigned integer from the start of a span.
///
/// @tparam T The unsigned integer type to read.
/// @param bytes At least `sizeof(T)` bytes; only the first `sizeof(T)` are
///     read. A shorter span is undefined behaviour.
/// @return The value.
template <std::unsigned_integral T>
constexpr auto load_le(std::span<const std::byte> bytes) noexcept -> T {
  T value = 0;
  for (std::size_t index = sizeof(T); index != 0; --index)
    value =
        static_cast<T>((value << 8U) | std::to_integer<T>(bytes[index - 1]));
  return value;
}

/// @brief Writes an unsigned integer as big-endian bytes at the start of a
/// span.
///
/// @tparam T The unsigned integer type to write.
/// @param bytes At least `sizeof(T)` bytes; only the first `sizeof(T)` are
///     written. A shorter span is undefined behaviour.
/// @param value The value.
template <std::unsigned_integral T>
constexpr void store_be(std::span<std::byte> bytes, T value) noexcept {
  for (std::size_t index = sizeof(T); index != 0; --index) {
    bytes[index - 1] = static_cast<std::byte>(value & 0xffU);
    value = static_cast<T>(value >> 8U);
  }
}

/// @brief Writes an unsigned integer as little-endian bytes at the start of a
/// span.
///
/// @tparam T The unsigned integer type to write.
/// @param bytes At least `sizeof(T)` bytes; only the first `sizeof(T)` are
///     written. A shorter span is undefined behaviour.
/// @param value The value.
template <std::unsigned_integral T>
constexpr void store_le(std::span<std::byte> bytes, T value) noexcept {
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    bytes[index] = static_cast<std::byte>(value & 0xffU);
    value = static_cast<T>(value >> 8U);
  }
}

/// @brief Appends an unsigned integer as big-endian bytes to a container.
///
/// @tparam T The unsigned integer type to write.
/// @tparam Output A container with `push_back(std::byte)`.
/// @param output The container to grow.
/// @param value The value.
template <std::unsigned_integral T, class Output>
void append_be(Output& output, T value) {
  std::array<std::byte, sizeof(T)> encoded{};
  store_be<T>(encoded, value);
  for (const auto byte : encoded) output.push_back(byte);
}

/// @brief Appends an unsigned integer as little-endian bytes to a container.
///
/// @tparam T The unsigned integer type to write.
/// @tparam Output A container with `push_back(std::byte)`.
/// @param output The container to grow.
/// @param value The value.
template <std::unsigned_integral T, class Output>
void append_le(Output& output, T value) {
  std::array<std::byte, sizeof(T)> encoded{};
  store_le<T>(encoded, value);
  for (const auto byte : encoded) output.push_back(byte);
}

}  // namespace libchiaking::bytes
