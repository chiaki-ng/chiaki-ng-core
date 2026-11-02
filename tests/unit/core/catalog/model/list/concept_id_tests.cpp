#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <string>

namespace catalog_test {
namespace {

using libchiaking::catalog::ConceptId;

TEST(ConceptIds, AreReadFromPositiveNumbers) {
  EXPECT_EQ(ConceptId::parse("90000016")->text(), "90000016");
  EXPECT_EQ(ConceptId::of(90000016), ConceptId::parse("90000016"));
}

TEST(ConceptIds, RefuseZero) {
  EXPECT_FALSE(ConceptId::parse(""));
  EXPECT_FALSE(ConceptId::parse("0"));
  EXPECT_FALSE(ConceptId::parse("000"));
  EXPECT_FALSE(ConceptId::of(0));
}

TEST(ConceptIds, RefuseTextThatIsNotADigitRun) {
  EXPECT_FALSE(ConceptId::parse("10 000"));
  EXPECT_FALSE(ConceptId::parse("-5"));
  EXPECT_FALSE(ConceptId::parse("abc"));
  EXPECT_FALSE(ConceptId::parse(std::string(ConceptId::limit + 1, '1')));
}

}  // namespace
}  // namespace catalog_test
