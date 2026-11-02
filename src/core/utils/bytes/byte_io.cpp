#include "src/core/utils/bytes/byte_io.hpp"

#include <algorithm>

namespace libchiaking::bytes {

auto Reader::u8() noexcept -> Expected<std::uint8_t, Shortfall> {
  return load<std::uint8_t>();
}

auto Reader::u16_be() noexcept -> Expected<std::uint16_t, Shortfall> {
  return load<std::uint16_t>();
}

auto Reader::u32_be() noexcept -> Expected<std::uint32_t, Shortfall> {
  return load<std::uint32_t>();
}

auto Reader::u64_be() noexcept -> Expected<std::uint64_t, Shortfall> {
  return load<std::uint64_t>();
}

auto Reader::i16_be() noexcept -> Expected<std::int16_t, Shortfall> {
  const auto value = load<std::uint16_t>();
  if (!value) return unexpected(value.error());
  return static_cast<std::int16_t>(value.value());
}

auto Reader::take(std::size_t count) noexcept
    -> Expected<std::span<const std::byte>, Shortfall> {
  if (count > remaining())
    return unexpected(Shortfall{offset_, count, remaining()});
  const auto view = bytes_.subspan(offset_, count);
  offset_ += count;
  return view;
}

auto Reader::skip(std::size_t count) noexcept -> Expected<void, Shortfall> {
  const auto skipped = take(count);
  if (!skipped) return unexpected(skipped.error());
  return {};
}

auto Reader::consume(std::string_view expected) noexcept -> bool {
  if (expected.size() > remaining()) return false;
  const auto candidate = std::as_bytes(std::span{expected});
  if (!std::ranges::equal(bytes_.subspan(offset_, expected.size()), candidate))
    return false;
  offset_ += expected.size();
  return true;
}

auto Writer::u8(std::uint8_t value) noexcept -> Expected<void, Shortfall> {
  return put_be(value);
}

auto Writer::u16_be(std::uint16_t value) noexcept -> Expected<void, Shortfall> {
  return put_be(value);
}

auto Writer::u32_be(std::uint32_t value) noexcept -> Expected<void, Shortfall> {
  return put_be(value);
}

auto Writer::u64_be(std::uint64_t value) noexcept -> Expected<void, Shortfall> {
  return put_be(value);
}

auto Writer::i16_be(std::int16_t value) noexcept -> Expected<void, Shortfall> {
  return put_be(static_cast<std::uint16_t>(value));
}

auto Writer::u16_le(std::uint16_t value) noexcept -> Expected<void, Shortfall> {
  return put_le(value);
}

auto Writer::u32_le(std::uint32_t value) noexcept -> Expected<void, Shortfall> {
  return put_le(value);
}

auto Writer::bytes(std::span<const std::byte> value) noexcept
    -> Expected<void, Shortfall> {
  const auto target = claim(value.size());
  if (!target) return unexpected(target.error());
  std::ranges::copy(value, target.value().begin());
  return {};
}

auto Writer::text(std::string_view value) noexcept
    -> Expected<void, Shortfall> {
  return bytes(std::as_bytes(std::span{value}));
}

auto Writer::zeros(std::size_t count) noexcept -> Expected<void, Shortfall> {
  const auto target = claim(count);
  if (!target) return unexpected(target.error());
  std::ranges::fill(target.value(), std::byte{});
  return {};
}

auto Writer::claim(std::size_t count) noexcept
    -> Expected<std::span<std::byte>, Shortfall> {
  if (count > remaining())
    return unexpected(Shortfall{offset_, count, remaining()});
  const auto target = bytes_.subspan(offset_, count);
  offset_ += count;
  return target;
}

void Appender::u8(std::uint8_t value) { append_be(output_, value); }

void Appender::u16_be(std::uint16_t value) { append_be(output_, value); }

void Appender::u32_be(std::uint32_t value) { append_be(output_, value); }

void Appender::u64_be(std::uint64_t value) { append_be(output_, value); }

void Appender::i16_be(std::int16_t value) {
  append_be(output_, static_cast<std::uint16_t>(value));
}

void Appender::u16_le(std::uint16_t value) { append_le(output_, value); }

void Appender::u32_le(std::uint32_t value) { append_le(output_, value); }

void Appender::bytes(std::span<const std::byte> value) {
  output_.insert(output_.end(), value.begin(), value.end());
}

void Appender::text(std::string_view value) {
  bytes(std::as_bytes(std::span{value}));
}

void Appender::zeros(std::size_t count) {
  output_.insert(output_.end(), count, std::byte{});
}

}  // namespace libchiaking::bytes
