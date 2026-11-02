/// @file
/// @brief Declares the library's one Base64 encoder and decoder, in the
/// standard alphabet and the URL-safe one, built on simdutf.
///
/// Remote Play session messages, account identifiers, PSN client credentials
/// and peer-link control messages travel as Base64; the URL-safe decoders are
/// strict, accepting only the one canonical unpadded spelling of each value.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/detail/secure_zero.hpp"

#include <simdutf.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::text::base64 {

/// @brief Which Base64 alphabet, and padding rule, to use.
enum class Alphabet : std::uint8_t { standard, url };

/// @var Alphabet::standard
/// @brief RFC 4648 section 4: `+` and `/`, padded with `=` to a multiple of
/// four characters.
/// @var Alphabet::url
/// @brief RFC 4648 section 5: `-` and `_`, written without padding.

/// @brief Why decode_into() could not decode a text.
enum class DecodeError : std::uint8_t { invalid, too_long };

/// @var DecodeError::invalid
/// @brief The text is not Base64 in the chosen alphabet.
/// @var DecodeError::too_long
/// @brief The decoded bytes would not fit in the output buffer.

/// @brief Maps an alphabet to simdutf's option for it.
///
/// @param alphabet The alphabet.
/// @return `simdutf::base64_url` for Alphabet::url, otherwise
///     `simdutf::base64_default`.
constexpr auto simdutf_options(Alphabet alphabet) noexcept
    -> simdutf::base64_options {
  return alphabet == Alphabet::url ? simdutf::base64_url
                                   : simdutf::base64_default;
}

/// @brief Says how many characters encoding a number of bytes produces.
///
/// @param bytes The number of bytes to encode.
/// @param alphabet The alphabet, which decides whether padding is counted.
/// @return The encoded length in characters.
inline auto encoded_size(std::size_t bytes, Alphabet alphabet) noexcept
    -> std::size_t {
  return simdutf::base64_length_from_binary(bytes, simdutf_options(alphabet));
}

/// @brief Encodes bytes into a caller-provided character buffer, writing no
/// terminator.
///
/// @param bytes The bytes to encode.
/// @param output The buffer; must hold at least encoded_size() characters.
/// @param alphabet The alphabet.
/// @return The number of characters written, or nothing, with nothing
///     written, when the buffer is too small.
[[nodiscard]] inline auto encode_into(std::span<const std::byte> bytes,
                                      std::span<char> output,
                                      Alphabet alphabet) noexcept
    -> std::optional<std::size_t> {
  if (output.size() < encoded_size(bytes.size(), alphabet)) return std::nullopt;
  return simdutf::binary_to_base64(reinterpret_cast<const char*>(bytes.data()),
                                   bytes.size(), output.data(),
                                   simdutf_options(alphabet));
}

/// @brief Appends the Base64 text of bytes to a string-like container.
///
/// Encodes in 48-byte pieces through a small buffer that is wiped
/// afterwards, so that encoding a secret into a wiping string leaves no
/// copy of it behind.
///
/// @tparam Output A character container with `append(std::string_view)`,
///     such as `std::string` or a secret string.
/// @param output The container to grow.
/// @param bytes The bytes to encode.
/// @param alphabet The alphabet.
template <class Output>
void append(Output& output, std::span<const std::byte> bytes,
            Alphabet alphabet) {
  constexpr std::size_t chunk = 48;
  SecretArray<64, char> text;
  while (!bytes.empty()) {
    const auto piece = bytes.first(std::min(chunk, bytes.size()));
    const auto written =
        encode_into(piece, {text.data(), text.size()}, alphabet);
    output.append(std::string_view{text.data(), *written});
    bytes = bytes.subspan(piece.size());
  }
}

/// @brief Encodes bytes as a new Base64 string.
///
/// @tparam Output The string type to build; needs `reserve` and
///     `append(std::string_view)`.
/// @param bytes The bytes to encode.
/// @param alphabet The alphabet.
/// @return The Base64 text.
template <class Output = std::string>
auto encode(std::span<const std::byte> bytes, Alphabet alphabet) -> Output {
  Output output;
  output.reserve(encoded_size(bytes.size(), alphabet));
  append(output, bytes, alphabet);
  return output;
}

/// @brief Decodes Base64 text as simdutf accepts it for the alphabet, into a
/// caller-provided buffer.
///
/// More lenient than the URL-safe decoders below: it leaves the padding and
/// white-space rules to simdutf and does not check that the text is the
/// canonical spelling.
///
/// @param text The Base64 text.
/// @param output The buffer for the decoded bytes. On failure it is wiped
///     with memory::secure_zero, so no partial secret is left in it.
/// @param alphabet The alphabet.
/// @return The number of bytes written.
/// @retval DecodeError::too_long The output buffer is too small.
/// @retval DecodeError::invalid The text is not valid Base64.
[[nodiscard]] inline auto decode_into(std::string_view text,
                                      std::span<std::byte> output,
                                      Alphabet alphabet) noexcept
    -> Expected<std::size_t, DecodeError> {
  auto written = output.size();
  const auto result = simdutf::base64_to_binary_safe(
      text.data(), text.size(), reinterpret_cast<char*>(output.data()), written,
      simdutf_options(alphabet));
  if (result.error == simdutf::error_code::SUCCESS) return written;
  memory::secure_zero(output);
  if (result.error == simdutf::error_code::OUTPUT_BUFFER_TOO_SMALL)
    return unexpected(DecodeError::too_long);
  return unexpected(DecodeError::invalid);
}

/// @brief Says how many bytes an unpadded Base64 text of a given length
/// decodes to.
///
/// @param characters The text's length.
/// @return The decoded size, or nothing when no unpadded Base64 text has that
///     length (one more than a multiple of four).
[[nodiscard]] constexpr auto url_decoded_size(std::size_t characters) noexcept
    -> std::optional<std::size_t> {
  const auto rest = characters % 4;
  if (rest == 1) return std::nullopt;
  return characters / 4 * 3 + (rest == 0 ? 0U : rest - 1U);
}

/// @brief Strictly decodes unpadded URL-safe Base64 that must fill a buffer
/// exactly.
///
/// Refuses padding, white space and any character outside the URL-safe
/// alphabet, and refuses a final group whose unused bits are not zero, so
/// that each value has exactly one accepted spelling.
///
/// @param text The Base64 text.
/// @param output The buffer; its size must be exactly the decoded size. On
///     failure it is wiped with memory::secure_zero, so no partial secret is
///     left in it.
/// @return True when the text decoded to exactly `output.size()` bytes and is
///     canonical; false otherwise.
[[nodiscard]] inline auto decode_url_into(std::string_view text,
                                          std::span<std::byte> output) noexcept
    -> bool {
  const auto decoded = [text, output]() noexcept {
    if (url_decoded_size(text.size()) != output.size()) return false;
    auto written = output.size();
    const auto result = simdutf::base64_to_binary_safe(
        text.data(), text.size(), reinterpret_cast<char*>(output.data()),
        written, simdutf::base64_url);
    if (result.error != simdutf::error_code::SUCCESS ||
        written != output.size())
      return false;
    const auto tail = output.last(output.size() % 3);
    if (tail.empty()) return true;
    SecretArray<4, char> canonical;
    const auto size =
        encode_into(tail, {canonical.data(), canonical.size()}, Alphabet::url);
    return std::ranges::equal(std::string_view{canonical.data(), *size},
                              text.substr(text.size() - *size));
  }();
  if (!decoded) memory::secure_zero(output);
  return decoded;
}

/// @brief Strictly decodes unpadded URL-safe Base64 of any length into a new
/// container.
///
/// @tparam Output A byte container constructible with a size, such as
///     `std::vector<std::byte>` or a secret byte buffer.
/// @param text The Base64 text, with the rules of decode_url_into().
/// @return The decoded bytes, or nothing when the text is not canonical
///     unpadded URL-safe Base64.
template <class Output = std::vector<std::byte>>
[[nodiscard]] auto decode_url(std::string_view text) -> std::optional<Output> {
  const auto size = url_decoded_size(text.size());
  if (!size) return std::nullopt;
  Output output(*size);
  if (!decode_url_into(text, output)) return std::nullopt;
  return output;
}

/// @brief Strictly decodes unpadded URL-safe Base64 that must decode to a
/// fixed number of bytes, such as a key or nonce.
///
/// @tparam Size The exact decoded size in bytes.
/// @param text The Base64 text, with the rules of decode_url_into().
/// @return The bytes, or nothing when the text has the wrong length or is
///     not canonical unpadded URL-safe Base64.
template <std::size_t Size>
[[nodiscard]] auto decode_url_exact(std::string_view text)
    -> std::optional<std::array<std::byte, Size>> {
  std::array<std::byte, Size> output{};
  if (!decode_url_into(text, output)) return std::nullopt;
  return output;
}

}  // namespace libchiaking::text::base64
