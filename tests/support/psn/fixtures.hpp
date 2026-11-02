#pragma once

#include "tests/support/json/tree.hpp"
#include "tests/support/setup.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

namespace test_support::psn {

class Fixtures {
 public:
  static auto text(std::string_view name) -> std::string {
    std::ifstream file{
        std::string{LIBCHIAKING_PSN_FIXTURES} + "/" + std::string{name},
        std::ios::binary};
    Setup::require(file.good(), "the PSN fixture opens");
    return std::string{std::istreambuf_iterator<char>{file},
                       std::istreambuf_iterator<char>{}};
  }

  static auto json(std::string_view body) -> JsonTree::Tree {
    return Setup::value(JsonTree::parse(body), "the PSN fixture parses");
  }

  static auto data(std::string_view name) -> JsonTree::Tree {
    const auto root = json(text(name));
    const auto* field = JsonTree::member(root, "data");
    Setup::require(field != nullptr, "the PSN fixture has data");
    return *field;
  }
};

}  // namespace test_support::psn
