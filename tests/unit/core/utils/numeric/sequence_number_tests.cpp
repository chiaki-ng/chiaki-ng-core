#include "src/core/utils/numeric/sequence_number.hpp"

#include <gtest/gtest.h>

#include <compare>
#include <cstdint>

namespace numeric_test {
namespace {

class SequenceNumber : public ::testing::Test {
 protected:
  using Sequence = libchiaking::numeric::SequenceNumber<std::uint16_t>;
  using Wide = libchiaking::numeric::SequenceNumber<std::uint32_t>;

  static auto replaced_rule(std::uint16_t candidate, std::uint16_t current)
      -> bool {
    return candidate != current &&
           static_cast<std::uint16_t>(candidate - current) < 0x8000U;
  }

  static auto agrees_with_the_replaced_rule() -> testing::AssertionResult {
    for (std::uint32_t candidate = 0; candidate <= 0xffff; candidate += 0x0101)
      for (std::uint32_t current = 0; current <= 0xffff; current += 0x00ff) {
        const auto left = static_cast<std::uint16_t>(candidate);
        const auto right = static_cast<std::uint16_t>(current);
        const bool newer = Sequence{left}.is_newer_than(Sequence{right});
        if (newer != replaced_rule(left, right))
          return testing::AssertionFailure()
                 << left << " is_newer_than " << right << " gave " << newer
                 << " but the replaced rule gave " << !newer;
      }
    return testing::AssertionSuccess();
  }
};

TEST_F(SequenceNumber, CountsALaterNumberAsNewer) {
  static_assert(Sequence{5}.is_newer_than(Sequence{4}));
  static_assert(!Sequence{4}.is_newer_than(Sequence{5}));
  static_assert(!Sequence{4}.is_newer_than(Sequence{4}));
  static_assert(Sequence{5} > Sequence{4} && Sequence{4} < Sequence{5});
}

TEST_F(SequenceNumber, KeepsNewnessAcrossTheWrap) {
  static_assert(Sequence{0}.is_newer_than(Sequence{0xffff}));
  static_assert(Sequence{0x10}.is_newer_than(Sequence{0xfff0}));
  static_assert(!Sequence{0xfff0}.is_newer_than(Sequence{0x10}));
  static_assert(Sequence{0xffff}.next() == Sequence{0});
  static_assert(Sequence{0xfffe}.advanced_by(4) == Sequence{2});
}

TEST_F(SequenceNumber, CountsDistanceForwardModuloTheRange) {
  static_assert(Sequence{0xfffe}.distance_to(Sequence{2}) == 4);
  static_assert(Sequence{2}.distance_to(Sequence{0xfffe}) == 0xfffc);
  static_assert(Sequence{9}.distance_to(Sequence{9}) == 0);
}

TEST_F(SequenceNumber, LeavesNumbersHalfTheRangeApartUnordered) {
  static_assert(Sequence::half_range == 0x8000);
  static_assert(!Sequence{0x8000}.is_newer_than(Sequence{0}));
  static_assert(!Sequence{0}.is_newer_than(Sequence{0x8000}));
  static_assert((Sequence{0} <=> Sequence{0x8000}) ==
                std::partial_ordering::unordered);
  static_assert(Sequence{0x7fff} > Sequence{0});
  static_assert(Sequence{0x8001} < Sequence{0});
}

TEST_F(SequenceNumber, MatchesTheRuleItReplaced) {
  EXPECT_TRUE(agrees_with_the_replaced_rule());
}

TEST_F(SequenceNumber, AppliesTheSameRuleToWiderCounters) {
  static_assert(Wide{0}.is_newer_than(Wide{0xffffffff}));
  static_assert(Wide::half_range == 0x80000000U);
}

}  // namespace
}  // namespace numeric_test
