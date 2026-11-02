#include "libchiaking/users_types.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <concepts>
#include <expected>
#include <string>

namespace detail_test {
namespace {

class ExpectedAlias : public ::testing::Test {
 protected:
  enum class Error { failed = 3 };

  template <class Access>
  static auto throws_bad_access(Access access) -> ::testing::AssertionResult {
    try {
      access();
    } catch (const std::bad_expected_access<Error>& caught) {
      if (caught.error() == Error::failed) return ::testing::AssertionSuccess();
      return ::testing::AssertionFailure() << "threw another error";
    }
    return ::testing::AssertionFailure() << "did not throw";
  }

  static auto refuse() -> libchiaking::Expected<void, Error> {
    return libchiaking::unexpected(Error::failed);
  }

  static auto produce() -> libchiaking::Expected<std::string, Error> {
    return std::string{"ready"};
  }
};

TEST_F(ExpectedAlias, IsTheStandardExpected) {
  static_assert(std::same_as<libchiaking::Expected<int, Error>,
                             std::expected<int, Error>>);
  static_assert(std::same_as<decltype(libchiaking::unexpected(Error::failed)),
                             std::unexpected<Error>>);
}

TEST_F(ExpectedAlias, HoldsAVoidSuccess) {
  const libchiaking::Expected<void, Error> success;
  EXPECT_TRUE(success.has_value());
  success.value();
}

TEST_F(ExpectedAlias, ThrowsTheErrorWhenAVoidFailureIsRead) {
  const auto failure = refuse();
  ASSERT_FALSE(failure);
  EXPECT_EQ(failure.error(), Error::failed);
  EXPECT_TRUE(throws_bad_access([&] { failure.value(); }));
}

TEST_F(ExpectedAlias, HoldsAValue) {
  const libchiaking::Expected<int, Error> value = 4;
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 4);
}

TEST_F(ExpectedAlias, ThrowsTheErrorOrFallsBackWhenAValueIsRefused) {
  const libchiaking::Expected<int, Error> refused =
      libchiaking::unexpected(Error::failed);
  EXPECT_TRUE(throws_bad_access([&] { (void)refused.value(); }));
  EXPECT_EQ(refused.value_or(7), 7);
}

TEST_F(ExpectedAlias, CarriesAProducedString) {
  EXPECT_EQ(produce().value(), "ready");
}

}  // namespace
}  // namespace detail_test
