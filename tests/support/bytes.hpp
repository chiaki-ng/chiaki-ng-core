#pragma once

#include "src/core/utils/text/hex.hpp"
#include "tests/support/setup.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace test_support {

class Bytes {
 public:
  static auto hex(std::span<const std::byte> bytes) -> std::string {
    std::string text;
    text.reserve(bytes.size() * 2);
    libchiaking::text::append_hex(text, bytes);
    return text;
  }

  static auto unhex(std::string_view text) -> std::vector<std::byte> {
    Setup::require(text.size() % 2 == 0, "the hex has whole bytes");
    std::vector<std::byte> bytes(text.size() / 2);
    Setup::require(libchiaking::text::decode_hex(text, bytes),
                   "the hex decodes");
    return bytes;
  }

  template <std::size_t Size>
  static auto unhex_array(std::string_view text)
      -> std::array<std::byte, Size> {
    std::array<std::byte, Size> bytes{};
    Setup::require(libchiaking::text::decode_hex(text, bytes),
                   "the hex decodes to the expected size");
    return bytes;
  }

  static auto of(std::string_view text) -> std::vector<std::byte> {
    const auto bytes = std::as_bytes(std::span{text});
    return {bytes.begin(), bytes.end()};
  }

  static void append(std::vector<std::byte>& output,
                     std::span<const std::byte> bytes) {
    output.insert(output.end(), bytes.begin(), bytes.end());
  }
};

}  // namespace test_support
