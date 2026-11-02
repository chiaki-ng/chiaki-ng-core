/// @file
/// @brief Declares the cursors the protocol code reads and writes wire
/// messages with: Reader over received bytes, Writer into a fixed buffer, and
/// Appender onto a growing one.
///
/// Every multi-byte integer is explicitly big- or little-endian, as the wire
/// format demands. Reader and Writer never step past their span; they report
/// a Shortfall instead.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace libchiaking::bytes {

/// @brief Says that a read or write did not fit in what was left of the
/// buffer, and where.
struct Shortfall {
  /// @brief The cursor's offset when the operation was refused, in bytes
  /// from the start of the buffer.
  std::size_t offset{};
  /// @brief How many bytes the operation needed.
  std::size_t needed{};
  /// @brief How many bytes were left from `offset` to the end.
  std::size_t available{};
};

/// @brief A forward cursor over received bytes.
///
/// Each read consumes from the current offset and advances it; a read that
/// does not fit fails with a Shortfall and leaves the offset unchanged. The
/// reader only views the bytes, which must outlive it and every span taken
/// from it. Not synchronised; one thread at a time.
class Reader {
 public:
  /// @brief Starts reading at the first byte.
  ///
  /// @param bytes The bytes to read; viewed, not copied.
  explicit constexpr Reader(std::span<const std::byte> bytes) noexcept
      : bytes_(bytes) {}

  /// @brief Returns how far reading has got.
  ///
  /// @return The number of bytes consumed so far.
  constexpr auto offset() const noexcept -> std::size_t { return offset_; }
  /// @brief Returns how much is left to read.
  ///
  /// @return The number of bytes after the offset.
  constexpr auto remaining() const noexcept -> std::size_t {
    return bytes_.size() - offset_;
  }

  /// @brief Reads one byte.
  ///
  /// @return The byte.
  /// @retval Shortfall Nothing is left.
  [[nodiscard]] auto u8() noexcept -> Expected<std::uint8_t, Shortfall>;
  /// @brief Reads a big-endian 16-bit unsigned integer.
  ///
  /// @return The value.
  /// @retval Shortfall Fewer than 2 bytes are left.
  [[nodiscard]] auto u16_be() noexcept -> Expected<std::uint16_t, Shortfall>;
  /// @brief Reads a big-endian 32-bit unsigned integer.
  ///
  /// @return The value.
  /// @retval Shortfall Fewer than 4 bytes are left.
  [[nodiscard]] auto u32_be() noexcept -> Expected<std::uint32_t, Shortfall>;
  /// @brief Reads a big-endian 64-bit unsigned integer.
  ///
  /// @return The value.
  /// @retval Shortfall Fewer than 8 bytes are left.
  [[nodiscard]] auto u64_be() noexcept -> Expected<std::uint64_t, Shortfall>;
  /// @brief Reads a big-endian 16-bit two's-complement integer.
  ///
  /// @return The value.
  /// @retval Shortfall Fewer than 2 bytes are left.
  [[nodiscard]] auto i16_be() noexcept -> Expected<std::int16_t, Shortfall>;
  /// @brief Takes the next bytes as a view.
  ///
  /// @param count How many bytes to take; zero takes an empty view.
  /// @return A view of the bytes, valid as long as the underlying buffer.
  /// @retval Shortfall Fewer than `count` bytes are left.
  [[nodiscard]] auto take(std::size_t count) noexcept
      -> Expected<std::span<const std::byte>, Shortfall>;
  /// @brief Steps over the next bytes without reading them.
  ///
  /// @param count How many bytes to step over.
  /// @return Nothing on success.
  /// @retval Shortfall Fewer than `count` bytes are left.
  [[nodiscard]] auto skip(std::size_t count) noexcept
      -> Expected<void, Shortfall>;
  /// @brief Consumes the given text if the next bytes are exactly it.
  ///
  /// @param expected The bytes to match, compared byte for byte.
  /// @return True when they matched and were consumed; false, with the
  ///     offset unchanged, when they differ or too few bytes are left.
  [[nodiscard]] auto consume(std::string_view expected) noexcept -> bool;

 private:
  /// @brief Reads a big-endian unsigned integer of the type's width.
  ///
  /// @tparam T The unsigned integer type to read.
  /// @return The value.
  /// @retval Shortfall Fewer than `sizeof(T)` bytes are left.
  template <std::unsigned_integral T>
  [[nodiscard]] auto load() noexcept -> Expected<T, Shortfall> {
    const auto view = take(sizeof(T));
    if (!view) return unexpected(view.error());
    return load_be<T>(view.value());
  }

  /// @brief The bytes being read.
  std::span<const std::byte> bytes_;
  /// @brief How many bytes have been consumed; never more than the size.
  std::size_t offset_{};
};

/// @brief A forward cursor writing into a fixed buffer.
///
/// Each write fills from the current offset and advances it; a write that
/// does not fit fails with a Shortfall, writes nothing and leaves the offset
/// unchanged. The buffer must outlive the writer. Not synchronised; one
/// thread at a time.
class Writer {
 public:
  /// @brief Starts writing at the first byte.
  ///
  /// @param bytes The buffer to fill.
  explicit constexpr Writer(std::span<std::byte> bytes) noexcept
      : bytes_(bytes) {}

  /// @brief Returns how much has been written.
  ///
  /// @return The number of bytes filled so far.
  constexpr auto offset() const noexcept -> std::size_t { return offset_; }
  /// @brief Returns how much room is left.
  ///
  /// @return The number of unfilled bytes after the offset.
  constexpr auto remaining() const noexcept -> std::size_t {
    return bytes_.size() - offset_;
  }

  /// @brief Writes one byte.
  ///
  /// @param value The byte.
  /// @return Nothing on success.
  /// @retval Shortfall No room is left.
  [[nodiscard]] auto u8(std::uint8_t value) noexcept
      -> Expected<void, Shortfall>;
  /// @brief Writes a big-endian 16-bit unsigned integer.
  ///
  /// @param value The value.
  /// @return Nothing on success.
  /// @retval Shortfall Fewer than 2 bytes of room are left.
  [[nodiscard]] auto u16_be(std::uint16_t value) noexcept
      -> Expected<void, Shortfall>;
  /// @brief Writes a big-endian 32-bit unsigned integer.
  ///
  /// @param value The value.
  /// @return Nothing on success.
  /// @retval Shortfall Fewer than 4 bytes of room are left.
  [[nodiscard]] auto u32_be(std::uint32_t value) noexcept
      -> Expected<void, Shortfall>;
  /// @brief Writes a big-endian 64-bit unsigned integer.
  ///
  /// @param value The value.
  /// @return Nothing on success.
  /// @retval Shortfall Fewer than 8 bytes of room are left.
  [[nodiscard]] auto u64_be(std::uint64_t value) noexcept
      -> Expected<void, Shortfall>;
  /// @brief Writes a big-endian 16-bit two's-complement integer.
  ///
  /// @param value The value.
  /// @return Nothing on success.
  /// @retval Shortfall Fewer than 2 bytes of room are left.
  [[nodiscard]] auto i16_be(std::int16_t value) noexcept
      -> Expected<void, Shortfall>;
  /// @brief Writes a little-endian 16-bit unsigned integer.
  ///
  /// @param value The value.
  /// @return Nothing on success.
  /// @retval Shortfall Fewer than 2 bytes of room are left.
  [[nodiscard]] auto u16_le(std::uint16_t value) noexcept
      -> Expected<void, Shortfall>;
  /// @brief Writes a little-endian 32-bit unsigned integer.
  ///
  /// @param value The value.
  /// @return Nothing on success.
  /// @retval Shortfall Fewer than 4 bytes of room are left.
  [[nodiscard]] auto u32_le(std::uint32_t value) noexcept
      -> Expected<void, Shortfall>;
  /// @brief Copies bytes in as they are.
  ///
  /// @param value The bytes to copy; must not overlap the unfilled room.
  /// @return Nothing on success.
  /// @retval Shortfall Less room is left than `value` holds.
  [[nodiscard]] auto bytes(std::span<const std::byte> value) noexcept
      -> Expected<void, Shortfall>;
  /// @brief Copies text in as its raw bytes, with no terminator.
  ///
  /// @param value The text.
  /// @return Nothing on success.
  /// @retval Shortfall Less room is left than `value` holds.
  [[nodiscard]] auto text(std::string_view value) noexcept
      -> Expected<void, Shortfall>;
  /// @brief Fills the next bytes with zero.
  ///
  /// @param count How many zero bytes to write.
  /// @return Nothing on success.
  /// @retval Shortfall Fewer than `count` bytes of room are left.
  [[nodiscard]] auto zeros(std::size_t count) noexcept
      -> Expected<void, Shortfall>;

 private:
  /// @brief Writes a big-endian unsigned integer of the type's width.
  ///
  /// @tparam T The unsigned integer type to write.
  /// @param value The value.
  /// @return Nothing on success.
  /// @retval Shortfall Fewer than `sizeof(T)` bytes of room are left.
  template <std::unsigned_integral T>
  [[nodiscard]] auto put_be(T value) noexcept -> Expected<void, Shortfall> {
    const auto target = claim(sizeof(T));
    if (!target) return unexpected(target.error());
    store_be<T>(target.value(), value);
    return {};
  }
  /// @brief Writes a little-endian unsigned integer of the type's width.
  ///
  /// @tparam T The unsigned integer type to write.
  /// @param value The value.
  /// @return Nothing on success.
  /// @retval Shortfall Fewer than `sizeof(T)` bytes of room are left.
  template <std::unsigned_integral T>
  [[nodiscard]] auto put_le(T value) noexcept -> Expected<void, Shortfall> {
    const auto target = claim(sizeof(T));
    if (!target) return unexpected(target.error());
    store_le<T>(target.value(), value);
    return {};
  }
  /// @brief Reserves the next bytes for a write and advances past them.
  ///
  /// @param count How many bytes to reserve.
  /// @return The reserved bytes, still holding whatever was there.
  /// @retval Shortfall Fewer than `count` bytes of room are left.
  [[nodiscard]] auto claim(std::size_t count) noexcept
      -> Expected<std::span<std::byte>, Shortfall>;

  /// @brief The buffer being filled.
  std::span<std::byte> bytes_;
  /// @brief How many bytes have been filled; never more than the size.
  std::size_t offset_{};
};

/// @brief Appends wire-format values to the end of a growing byte vector.
///
/// Never fails short: the vector grows as needed, so the only failure is
/// `std::bad_alloc` from it. The vector must outlive the appender. Not
/// synchronised; one thread at a time.
class Appender {
 public:
  /// @brief Appends to the end of a vector, keeping what it already holds.
  ///
  /// @param output The vector to grow.
  explicit Appender(std::vector<std::byte>& output) noexcept
      : output_(output) {}

  /// @brief Appends one byte.
  ///
  /// @param value The byte.
  void u8(std::uint8_t value);
  /// @brief Appends a big-endian 16-bit unsigned integer.
  ///
  /// @param value The value.
  void u16_be(std::uint16_t value);
  /// @brief Appends a big-endian 32-bit unsigned integer.
  ///
  /// @param value The value.
  void u32_be(std::uint32_t value);
  /// @brief Appends a big-endian 64-bit unsigned integer.
  ///
  /// @param value The value.
  void u64_be(std::uint64_t value);
  /// @brief Appends a big-endian 16-bit two's-complement integer.
  ///
  /// @param value The value.
  void i16_be(std::int16_t value);
  /// @brief Appends a little-endian 16-bit unsigned integer.
  ///
  /// @param value The value.
  void u16_le(std::uint16_t value);
  /// @brief Appends a little-endian 32-bit unsigned integer.
  ///
  /// @param value The value.
  void u32_le(std::uint32_t value);
  /// @brief Appends bytes as they are.
  ///
  /// @param value The bytes; must not be part of the output vector.
  void bytes(std::span<const std::byte> value);
  /// @brief Appends text as its raw bytes, with no terminator.
  ///
  /// @param value The text.
  void text(std::string_view value);
  /// @brief Appends zero bytes.
  ///
  /// @param count How many.
  void zeros(std::size_t count);

 private:
  /// @brief The vector being grown.
  std::vector<std::byte>& output_;
};

}  // namespace libchiaking::bytes
