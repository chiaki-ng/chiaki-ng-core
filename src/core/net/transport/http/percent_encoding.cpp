#include "src/core/net/transport/http/percent_encoding.hpp"

#include <algorithm>

namespace libchiaking::net::http {

auto PercentEncoding::component(std::string_view value) -> std::string {
  std::string result;
  result.reserve(value.size() * escape_width);
  append(result, value);
  return result;
}

auto PercentEncoding::component_truncated(std::string_view value,
                                          std::size_t capacity) -> std::string {
  const auto limit = capacity - std::min(capacity, escape_reserve);
  std::string result;
  for (const char raw : text::c_string(value)) {
    if (result.size() >= limit) break;
    append(result, std::string_view{&raw, 1});
  }
  return result;
}

auto PercentEncoding::decode_visible(std::string_view input,
                                     DecodeLimits limits)
    -> std::optional<SecretString> {
  if (input.empty() || input.size() > limits.encoded_bytes) return std::nullopt;
  SecretString output;
  for (std::size_t index = 0; index < input.size(); ++index) {
    char character = input[index];
    if (character == '%') {
      if (index + 2 >= input.size()) return std::nullopt;
      const auto high = text::hex_digit(input[index + 1]);
      const auto low = text::hex_digit(input[index + 2]);
      if (!high || !low) return std::nullopt;
      character = static_cast<char>((*high << 4U) | *low);
      index += 2;
    } else if (character == '+') {
      return std::nullopt;
    }
    if (!text::ascii_visible(character)) return std::nullopt;
    output.push_back(character);
  }
  if (output.size() > limits.decoded_bytes) return std::nullopt;
  return output;
}

auto PercentEncoding::form(std::string_view value) -> std::string {
  std::string result;
  result.reserve(value.size() * escape_width);
  append(result, value, Escape::form);
  return result;
}

}  // namespace libchiaking::net::http
