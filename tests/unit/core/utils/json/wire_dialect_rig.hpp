#pragma once

#include "src/core/utils/json/wire_dialect.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>
#include <glaze/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace json_test {

class WireDialectRig {
 public:
  using Dialect = libchiaking::json::WireDialect;
  using ParseError = libchiaking::json::ParseError;

  static auto canonical(std::string_view input) -> std::optional<std::string> {
    auto result = Dialect::canonical_text(input);
    if (!result) return std::nullopt;
    return std::string{result.value().view()};
  }

  static auto rejected(std::string_view input,
                       ParseError error = ParseError::invalid_json)
      -> testing::AssertionResult {
    auto result = Dialect::canonical_text(input);
    if (result)
      return testing::AssertionFailure()
             << "accepted as " << result.value().view();
    if (result.error() != error)
      return testing::AssertionFailure()
             << "refused with " << testing::PrintToString(result.error());
    return testing::AssertionSuccess();
  }

  static auto tree_of(std::string_view text) -> std::optional<std::string> {
    glz::generic document{};
    if (glz::read<libchiaking::json::document_options>(document, text))
      return std::nullopt;
    return document.dump().value_or(std::string{});
  }

  static auto matches_strict(std::string_view input)
      -> testing::AssertionResult {
    if (!libchiaking::json::well_formed(input))
      return testing::AssertionSuccess();
    const auto strict = tree_of(input);
    auto current = Dialect::canonical_text(input);
    if (!strict)
      return testing::AssertionFailure() << "the strict reader refused it";
    if (!current)
      return testing::AssertionFailure() << "the dialect refused it";
    const auto again = tree_of(current.value().view());
    if (again != strict)
      return testing::AssertionFailure()
             << "the dialect's text reads as " << again.value_or("nothing")
             << " rather than " << *strict;
    return testing::AssertionSuccess();
  }
};

}  // namespace json_test
