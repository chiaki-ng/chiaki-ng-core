#include "src/core/utils/json/strict.hpp"
#include "tests/unit/core/utils/json/strict_corpus.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

namespace json_test {
namespace {

using libchiaking::json::strict::Value;

class StrictJsonCorpus : public ::testing::Test {
 protected:
  static auto holds(const Case& entry) -> testing::AssertionResult {
    if (entry.kind == Case::Kind::build) {
      if (entry.canonical &&
          Blueprint<Value>::build(entry.input).canonical().value() ==
              *entry.canonical)
        return testing::AssertionSuccess();
      return testing::AssertionFailure() << "build differs";
    }
    const auto parsed = Value::parse(entry.input);
    if (!entry.canonical) {
      if (!parsed) return testing::AssertionSuccess();
      return testing::AssertionFailure() << "parse accepted a rejected input";
    }
    if (!parsed) return testing::AssertionFailure() << "parse refused";
    if (parsed->canonical().value() != *entry.canonical)
      return testing::AssertionFailure()
             << "canonical text is " << parsed->canonical().value();
    const auto again = Value::parse(*entry.canonical);
    if (again && again->canonical().value() == *entry.canonical)
      return testing::AssertionSuccess();
    return testing::AssertionFailure() << "canonical text does not round trip";
  }

  const std::vector<Case> cases = Corpus::load(LIBCHIAKING_JSON_STRICT_CORPUS);
};

TEST_F(StrictJsonCorpus, HoldsEveryLine) {
  for (std::size_t index = 0; index < cases.size(); ++index)
    ASSERT_TRUE(holds(cases[index])) << "strict json corpus line " << index + 1
                                     << ": " << cases[index].line();
}

TEST_F(StrictJsonCorpus, HoldsTheExpectedMixOfCases) {
  std::size_t parsed = 0;
  std::size_t built = 0;
  std::size_t accepted = 0;
  for (const auto& entry : cases) {
    parsed += entry.kind == Case::Kind::parse ? 1U : 0U;
    built += entry.kind == Case::Kind::build ? 1U : 0U;
    accepted += entry.kind == Case::Kind::parse && entry.canonical ? 1U : 0U;
  }
  EXPECT_EQ(parsed, 5792U);
  EXPECT_EQ(built, 1489U);
  EXPECT_EQ(accepted, 813U);
}

}  // namespace
}  // namespace json_test
