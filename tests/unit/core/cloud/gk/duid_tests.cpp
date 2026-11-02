#include "tests/support/random.hpp"

#include <libchiaking/users_types.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <string_view>

namespace cloud_test {
namespace {

TEST(GkDuid, IsGeneratedFromSixteenRandomBytes) {
  test_support::CountingRandom random;
  const auto duid = libchiaking::users::DeviceId::generate(random);
  ASSERT_TRUE(duid);
  EXPECT_EQ(
      duid->view(),
      std::string_view{"0000000700410080000102030405060708090a0b0c0d0e0f"});
  EXPECT_EQ(duid->view().size(), std::size_t{48});
  EXPECT_EQ(random.calls, 1U);
  EXPECT_EQ(random.last_size, std::size_t{16});
}

TEST(GkDuid, FailsWhenTheRandomSourceFails) {
  test_support::CountingRandom random;
  ASSERT_TRUE(libchiaking::users::DeviceId::generate(random));
  random.fail = true;
  const auto failed = libchiaking::users::DeviceId::generate(random);
  EXPECT_FALSE(failed);
  EXPECT_EQ(random.calls, 2U);
  EXPECT_EQ(random.last_size, std::size_t{16});
}

}  // namespace
}  // namespace cloud_test
