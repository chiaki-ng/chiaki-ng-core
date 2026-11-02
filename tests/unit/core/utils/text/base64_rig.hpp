#pragma once

#include <array>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace text_test {

class Base64Rig {
 public:
  struct Vector {
    std::string_view plain;
    std::string_view encoded;
  };

  static constexpr std::array<std::byte, 3> high{
      std::byte{0xfb}, std::byte{0xff}, std::byte{0xbf}};

  static auto bytes_of(std::string_view text) -> std::vector<std::byte> {
    const auto view = std::as_bytes(std::span{text});
    return {view.begin(), view.end()};
  }

  static auto text_of(std::span<const std::byte> bytes) -> std::string {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
  }

  static auto pattern_byte(std::size_t length) -> std::byte {
    return static_cast<std::byte>((length * 151 + 7) & 0xffU);
  }

  static auto load(std::string_view name) -> std::optional<std::string> {
    std::string path{LIBCHIAKING_BASE64URL_FIXTURES};
    path.push_back('/');
    path.append(name);
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return std::nullopt;
    std::string contents;
    std::array<char, 4096> buffer{};
    while (true) {
      const auto count = std::fread(buffer.data(), 1, buffer.size(), file);
      if (count == 0) break;
      contents.append(buffer.data(), count);
    }
    std::fclose(file);
    return contents;
  }
};

}  // namespace text_test
