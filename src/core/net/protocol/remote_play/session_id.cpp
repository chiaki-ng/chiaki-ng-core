#include "session_id.hpp"

#include "limits.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/base64.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <ranges>
#include <string_view>

namespace libchiaking::net::protocol::remote_play {

auto SessionId::parse(std::span<const std::byte> payload) noexcept
    -> Expected<SessionId, SessionIdError> {
  if (payload.size() < session_id_min_len)
    return unexpected(SessionIdError::too_short);
  const std::size_t length = payload.size() - 1;
  if (length >= session_id_max_len) return unexpected(SessionIdError::too_long);
  SessionId result;
  result.unexpected_marker_ = payload[0] != session_id_marker;
  const auto as_char = [](std::byte value) { return static_cast<char>(value); };
  const auto characters = payload.subspan(1) | std::views::transform(as_char);
  if (!std::ranges::all_of(characters, text::ascii_alnum))
    return unexpected(SessionIdError::invalid_character);
  std::ranges::copy(characters, result.bytes_.begin());
  result.size_ = length;
  return result;
}

auto SessionId::fallback(std::uint64_t monotonic_seconds,
                         std::span<const std::byte> random48) noexcept
    -> Expected<SessionId, SessionIdError> {
  if (random48.size() != session_id_random_size)
    return unexpected(SessionIdError::invalid_random_length);

  char prefix[16]{};
  const auto rendered = std::to_chars(prefix, prefix + 15, monotonic_seconds);
  if (rendered.ec != std::errc{}) return unexpected(SessionIdError::too_long);

  SessionId result;
  const auto out = static_cast<std::size_t>(rendered.ptr - prefix);
  std::ranges::copy_n(prefix, static_cast<std::ptrdiff_t>(out),
                      result.bytes_.begin());
  const auto encoded = text::base64::encode_into(
      random48, {result.bytes_.data() + out, result.bytes_.size() - out},
      text::base64::Alphabet::standard);
  if (!encoded) return unexpected(SessionIdError::too_long);
  result.size_ = out + *encoded;
  result.synthesized_ = true;
  return result;
}

}  // namespace libchiaking::net::protocol::remote_play
