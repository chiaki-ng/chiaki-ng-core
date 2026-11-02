#include "src/core/utils/json/json.hpp"
#include "src/core/utils/json/wire_dialect.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>
#include <glaze/glaze.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace json_test {

class ReplyJsonShapes {
 public:
  struct Outer {
    std::string key;
  };

  struct Document {
    Outer outer;
    std::vector<glz::raw_json> list;
  };

  struct Flags {
    bool yes{};
    bool no{};
    std::optional<bool> empty;
  };

  struct Duplicate {
    std::int64_t a{};
    std::int64_t b{};
  };

  struct Padded {
    std::string pad;
    std::optional<std::int64_t> id;
  };
};

namespace {

namespace json = libchiaking::json;
using json::ParseError;
using Shapes = ReplyJsonShapes;

class ReplyJsonText {
 public:
  static auto padded() -> std::string {
    std::string large = R"({"pad":")";
    large.append(64 * 1024, 'x');
    large += R"(","id":7})";
    return large;
  }

  static auto oversized() -> std::string {
    return std::string(json::max_document_bytes + 1, ' ');
  }

  static auto nested(std::size_t depth) -> std::string {
    std::string text(depth, '[');
    text.append(depth, ']');
    return text;
  }
};

class ReplyStrictJson : public ::testing::Test {
 protected:
  static auto invalid(std::string_view text) -> testing::AssertionResult {
    const auto parsed = json::read<glz::generic_u64>(text);
    if (parsed) return testing::AssertionFailure() << "accepted " << text;
    if (parsed.error() != ParseError::invalid_json)
      return testing::AssertionFailure()
             << "refused " << text << " as "
             << testing::PrintToString(parsed.error());
    return testing::AssertionSuccess();
  }
};

TEST_F(ReplyStrictJson, ReadsNestedObjectsAndRawValues) {
  const auto tree = json::read<Shapes::Document>(
      R"({"outer":{"key":"val😀"},"list":[1,"two",null]})");
  ASSERT_TRUE(tree);
  EXPECT_EQ(tree.value().outer.key, "val\xF0\x9F\x98\x80");
  ASSERT_EQ(tree.value().list.size(), 3U);
  EXPECT_EQ(tree.value().list[1].str, "\"two\"");
  EXPECT_EQ(tree.value().list[2].str, "null");
}

TEST_F(ReplyStrictJson, ReadsBooleansAndNull) {
  const auto booleans =
      json::read<Shapes::Flags>(R"({"yes":true,"no":false,"empty":null})");
  ASSERT_TRUE(booleans);
  EXPECT_TRUE(booleans.value().yes);
  EXPECT_FALSE(booleans.value().no);
  EXPECT_FALSE(booleans.value().empty);
}

TEST_F(ReplyStrictJson, RejectsMisspelledLiterals) {
  EXPECT_TRUE(invalid(R"({"yes":tru})"));
  EXPECT_TRUE(invalid("[trueX]"));
}

TEST_F(ReplyStrictJson, KeepsTheLastDuplicateKey) {
  const auto duplicate =
      json::read<Shapes::Duplicate>(R"({"a":1,"b":3,"a":2})");
  ASSERT_TRUE(duplicate);
  EXPECT_EQ(duplicate.value().a, 2);
  EXPECT_EQ(duplicate.value().b, 3);
}

TEST_F(ReplyStrictJson, RejectsIncompleteTrailingOrBlankText) {
  EXPECT_TRUE(invalid("{"));
  EXPECT_TRUE(invalid("1 x"));
  EXPECT_TRUE(invalid("{\"a\":true}\n<html>"));
  EXPECT_TRUE(invalid(" \n"));
}

TEST_F(ReplyStrictJson, ReadsALargeDocument) {
  const auto large_parsed = json::read<Shapes::Padded>(ReplyJsonText::padded());
  ASSERT_TRUE(large_parsed);
  EXPECT_EQ(large_parsed.value().id, 7);
}

TEST_F(ReplyStrictJson, RejectsAnOversizedDocument) {
  const auto too_large =
      json::read<glz::generic_u64>(ReplyJsonText::oversized());
  ASSERT_FALSE(too_large);
  EXPECT_EQ(too_large.error(), ParseError::too_large);
}

TEST_F(ReplyStrictJson, LimitsTheNestingDepth) {
  EXPECT_TRUE(json::read<glz::generic_u64>(ReplyJsonText::nested(64)));
  const auto too_deep = json::read<glz::generic_u64>(ReplyJsonText::nested(66));
  ASSERT_FALSE(too_deep);
  EXPECT_EQ(too_deep.error(), ParseError::too_deep);
}

TEST_F(ReplyStrictJson, RejectsLeadingZeros) {
  EXPECT_TRUE(invalid(R"({"reqId":04,"n":-007,"f":00.5})"));
  EXPECT_TRUE(invalid(R"({"reqId":04})"));
  EXPECT_TRUE(invalid(R"({"n":-007})"));
  EXPECT_TRUE(invalid(R"({"f":00.5})"));
}

TEST_F(ReplyStrictJson, RejectsInvalidUtf8AndRawControls) {
  EXPECT_TRUE(invalid("[\"\xC3\x28\t\"]"));
  EXPECT_TRUE(invalid("[\"\xC3\x28\"]"));
  EXPECT_TRUE(invalid("[\"\t\"]"));
}

class ReplyWireDialect : public ::testing::Test {
 protected:
  static auto lenient(std::string_view text)
      -> libchiaking::Expected<std::string, ParseError> {
    return json::WireDialect::canonical_text(text).transform(
        [](const libchiaking::SecretString& canonical) {
          return std::string{canonical.view()};
        });
  }

  static auto refused(std::string_view text, ParseError reason)
      -> testing::AssertionResult {
    const auto canonical = lenient(text);
    if (canonical)
      return testing::AssertionFailure()
             << "accepted " << text << " as " << canonical.value();
    if (canonical.error() != reason)
      return testing::AssertionFailure()
             << "refused " << text << " as "
             << testing::PrintToString(canonical.error());
    return testing::AssertionSuccess();
  }
};

TEST_F(ReplyWireDialect, KeepsNestedObjectsAndUtf8) {
  EXPECT_EQ(
      lenient(R"({"outer":{"key":"val😀"},"list":[1,"two",null]})").value(),
      "{\"outer\":{\"key\":\"val\xF0\x9F\x98\x80\"},\"list\":[1,\"two\",null]"
      "}");
  EXPECT_EQ(lenient("[\"val\xF0\x9F\x98\x80\"]").value(),
            "[\"val\xF0\x9F\x98\x80\"]");
}

TEST_F(ReplyWireDialect, KeepsBooleansAndNull) {
  EXPECT_EQ(lenient(R"({"yes":true,"no":false,"empty":null})").value(),
            R"({"yes":true,"no":false,"empty":null})");
}

TEST_F(ReplyWireDialect, RejectsMisspelledLiterals) {
  EXPECT_TRUE(refused(R"({"yes":tru})", ParseError::invalid_json));
  EXPECT_TRUE(refused("[trueX]", ParseError::invalid_json));
}

TEST_F(ReplyWireDialect, KeepsTheLastDuplicateKey) {
  EXPECT_EQ(lenient(R"({"a":1,"b":3,"a":2})").value(), R"({"a":2,"b":3})");
}

TEST_F(ReplyWireDialect, RejectsIncompleteOrBlankText) {
  EXPECT_TRUE(refused("{", ParseError::invalid_json));
  EXPECT_TRUE(refused(" \n", ParseError::invalid_json));
}

TEST_F(ReplyWireDialect, DropsTrailingText) {
  EXPECT_EQ(lenient("1 x").value(), "1");
  EXPECT_EQ(lenient("{\"a\":true}\n<html>").value(), R"({"a":true})");
}

TEST_F(ReplyWireDialect, KeepsALargeDocument) {
  EXPECT_TRUE(
      lenient(ReplyJsonText::padded()).value().ends_with(R"(","id":7})"));
}

TEST_F(ReplyWireDialect, RejectsAnOversizedDocument) {
  EXPECT_TRUE(refused(ReplyJsonText::oversized(), ParseError::too_large));
}

TEST_F(ReplyWireDialect, LimitsTheNestingDepth) {
  EXPECT_TRUE(lenient(ReplyJsonText::nested(64)));
  EXPECT_TRUE(refused(ReplyJsonText::nested(66), ParseError::too_deep));
}

TEST_F(ReplyWireDialect, ReplacesLoneSurrogates) {
  EXPECT_EQ(lenient(R"(["\uD800x","\uDC00"])").value(),
            "[\"\xEF\xBF\xBDx\",\"\xEF\xBF\xBD\"]");
}

TEST_F(ReplyWireDialect, DropsLeadingZerosButKeepsFractions) {
  EXPECT_EQ(
      lenient(R"({"reqId":04,"n":-007,"f":00.5,"e":0.05e01,"zero":0})").value(),
      R"({"reqId":4,"n":-7,"f":0.5,"e":0.05e01,"zero":0})");
}

TEST_F(ReplyWireDialect, EscapesControlsAndPassesInvalidUtf8Through) {
  EXPECT_EQ(lenient("[\"\xC3\x28\t\"]").value(), "[\"\xC3\x28\\t\"]");
  EXPECT_EQ(lenient("{\"k\xFF\x01\":\"\x80\"}").value(),
            "{\"k\xFF\\u0001\":\"\x80\"}");
  EXPECT_EQ(lenient(R"(["a\"b\\c\n\u0001é"])").value(),
            "[\"a\\\"b\\\\c\\n\\u0001\xC3\xA9\"]");
}

TEST_F(ReplyWireDialect, IsMoreLenientThanTheStrictReader) {
  const auto strict = json::read<glz::generic_u64>(R"({"reqId":04})");
  ASSERT_FALSE(strict);
  EXPECT_EQ(strict.error(), ParseError::invalid_json);
}

}  // namespace
}  // namespace json_test
