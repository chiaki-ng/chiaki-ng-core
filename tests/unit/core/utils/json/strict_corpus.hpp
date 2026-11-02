#pragma once

#include "src/core/utils/text/hex.hpp"
#include "tests/support/setup.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json_test {

class Hex {
 public:
  static auto encode(std::string_view bytes) -> std::string {
    std::string text;
    libchiaking::text::append_hex(
        text, std::as_bytes(std::span{bytes.data(), bytes.size()}));
    return text;
  }

  static auto decode(std::string_view text) -> std::string {
    std::string bytes(text.size() / 2, '\0');
    test_support::Setup::require(libchiaking::text::decode_hex(
                    text, std::as_writable_bytes(
                              std::span{bytes.data(), bytes.size()})),
                "the corpus hex decodes");
    return bytes;
  }

  static auto is_digit(char symbol) noexcept -> bool {
    return libchiaking::text::lower_hex_digit(symbol);
  }
};

template <class Value>
class Blueprint {
 public:
  static auto build(std::string_view text) -> Value {
    Blueprint blueprint{text};
    auto value = blueprint.value();
    test_support::Setup::require(blueprint.offset_ == text.size(),
                "the blueprint is used up");
    return value;
  }

 private:
  explicit Blueprint(std::string_view text) noexcept : text_(text) {}

  auto value() -> Value {
    test_support::Setup::require(offset_ < text_.size(), "the blueprint continues");
    const auto tag = text_[offset_++];
    if (tag == 'i') {
      std::uint64_t number = 0;
      const auto* first = text_.data() + offset_;
      const auto* last = text_.data() + text_.size();
      const auto parsed = std::from_chars(first, last, number);
      test_support::Setup::require(parsed.ec == std::errc{} && parsed.ptr != first,
                  "the blueprint number parses");
      offset_ += static_cast<std::size_t>(parsed.ptr - first);
      return Value{number};
    }
    if (tag == 's') return Value{run()};
    if (tag == 'a') {
      typename Value::Array items;
      take('[');
      while (!accept(']')) {
        if (!items.empty()) take(',');
        items.push_back(value());
      }
      return Value{std::move(items)};
    }
    test_support::Setup::require(tag == 'o', "the blueprint tag is known");
    typename Value::Object members;
    take('{');
    while (!accept('}')) {
      if (!members.empty()) take(',');
      auto key = run();
      take(':');
      members.push_back({std::move(key), value()});
    }
    return Value{std::move(members)};
  }

  auto run() -> std::string {
    const auto start = offset_;
    while (offset_ < text_.size() && Hex::is_digit(text_[offset_])) ++offset_;
    return Hex::decode(text_.substr(start, offset_ - start));
  }

  auto accept(char symbol) noexcept -> bool {
    if (offset_ >= text_.size() || text_[offset_] != symbol) return false;
    ++offset_;
    return true;
  }

  void take(char symbol) {
    test_support::Setup::require(accept(symbol), "the blueprint has the expected symbol");
  }

  std::string_view text_;
  std::size_t offset_{};
};

struct Case {
  enum class Kind : std::uint8_t { parse, build };

  Kind kind{};
  std::string input;
  std::optional<std::string> canonical;

  auto line() const -> std::string {
    std::string text{kind == Kind::parse ? "parse " : "build "};
    text.append(kind == Kind::parse ? Hex::encode(input) : input);
    if (!canonical) return text.append(" reject");
    return text.append(" accept ").append(Hex::encode(*canonical));
  }
};

class Corpus {
 public:
  static auto load(const char* path) -> std::vector<Case> {
    const auto contents = read(path);
    std::vector<Case> cases;
    std::size_t start = 0;
    while (start < contents.size()) {
      const auto end = contents.find('\n', start);
      test_support::Setup::require(end != std::string::npos,
                  "every corpus line ends in a newline");
      cases.push_back(
          parse(std::string_view{contents}.substr(start, end - start)));
      start = end + 1;
    }
    test_support::Setup::require(!cases.empty(), "the corpus has cases");
    return cases;
  }

  static void store(const char* path, const std::vector<Case>& cases) {
    std::FILE* file = std::fopen(path, "wb");
    test_support::Setup::require(file != nullptr, "the corpus opens for writing");
    for (const auto& entry : cases) {
      const auto text = entry.line() + "\n";
      test_support::Setup::require(std::fwrite(text.data(), 1, text.size(), file) == text.size(),
                  "the corpus line is written");
    }
    std::fclose(file);
  }

 private:
  static auto read(const char* path) -> std::string {
    std::FILE* file = std::fopen(path, "rb");
    test_support::Setup::require(file != nullptr, "the corpus opens");
    std::string contents;
    std::array<char, 65536> buffer{};
    while (true) {
      const auto count = std::fread(buffer.data(), 1, buffer.size(), file);
      if (count == 0) break;
      contents.append(buffer.data(), count);
    }
    std::fclose(file);
    return contents;
  }

  static auto parse(std::string_view line) -> Case {
    const auto first = line.find(' ');
    test_support::Setup::require(first != std::string_view::npos, "the line has a kind");
    const auto second = line.find(' ', first + 1);
    test_support::Setup::require(second != std::string_view::npos, "the line has a verdict");
    const auto kind = line.substr(0, first);
    const auto input = line.substr(first + 1, second - first - 1);
    const auto verdict = line.substr(second + 1);
    test_support::Setup::require(kind == "parse" || kind == "build", "the kind is known");
    Case entry;
    entry.kind = kind == "parse" ? Case::Kind::parse : Case::Kind::build;
    entry.input = entry.kind == Case::Kind::parse ? Hex::decode(input)
                                                  : std::string{input};
    if (verdict == "reject") return entry;
    constexpr std::string_view accepted = "accept ";
    test_support::Setup::require(verdict.substr(0, accepted.size()) == accepted,
                "the verdict is accept or reject");
    entry.canonical = Hex::decode(verdict.substr(accepted.size()));
    return entry;
  }
};

}  // namespace json_test
