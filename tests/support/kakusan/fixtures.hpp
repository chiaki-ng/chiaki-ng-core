#pragma once

#include "src/core/utils/json/strict.hpp"
#include "tests/support/setup.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace test_support::kakusan {

class Fixtures {
 public:
  using Value = libchiaking::json::strict::Value;

  static auto text(std::string_view name) -> std::string {
    std::ifstream file{
        std::string{LIBCHIAKING_WIRE_FIXTURES} + "/" + std::string{name},
        std::ios::binary};
    Setup::require(file.good(), "the wire fixture opens");
    std::string contents{std::istreambuf_iterator<char>{file},
                         std::istreambuf_iterator<char>{}};
    Setup::require(!contents.empty(), "the wire fixture has contents");
    return contents;
  }

  static auto line(std::string_view name) -> std::string {
    auto contents = text(name);
    while (!contents.empty() &&
           (contents.back() == '\n' || contents.back() == '\r'))
      contents.pop_back();
    return contents;
  }

  static auto string(std::string_view document, std::string_view key)
      -> std::string {
    const auto needle = "\"" + std::string{key} + "\": \"";
    const auto start = document.find(needle);
    Setup::require(start != std::string_view::npos,
                   "the fixture names the key");
    std::string value;
    for (auto index = start + needle.size(); index < document.size(); ++index) {
      const auto symbol = document[index];
      if (symbol == '"') return value;
      if (symbol == '\\') {
        ++index;
        Setup::require(index < document.size(), "the escape is complete");
        value.push_back(document[index]);
        continue;
      }
      value.push_back(symbol);
    }
    throw SetupFailure{"the fixture string is unterminated"};
  }

  static auto member(const Value& value, std::string_view key) -> const Value& {
    const auto* object = value.object();
    Setup::require(object != nullptr, "the fixture value is an object");
    for (const auto& item : *object) {
      if (item.key == key) return item.value;
    }
    throw SetupFailure{"the fixture object has the member"};
  }

  static auto string_of(const Value& value) -> const std::string& {
    const auto* text = value.string();
    Setup::require(text != nullptr, "the fixture value is a string");
    return *text;
  }
};

}  // namespace test_support::kakusan
