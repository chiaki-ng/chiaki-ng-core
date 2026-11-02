#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <string>

namespace users_test {
namespace {

using libchiaking::users::DeviceId;

TEST(DeviceId, RefusesTheWrongLength) {
  EXPECT_FALSE(DeviceId::parse("short"));
  EXPECT_FALSE(DeviceId::parse(std::string(48, '1')));
}

TEST(DeviceId, RefusesANonHexDigit) {
  EXPECT_FALSE(
      DeviceId::parse("0000000700410080" + std::string(31, 'a') + "g"));
}

TEST(DeviceId, IsKeptInLowerCase) {
  const auto shouted =
      DeviceId::parse("0000000700410080" + std::string(32, 'A'));
  ASSERT_TRUE(shouted);
  EXPECT_EQ(shouted->view(), "0000000700410080" + std::string(32, 'a'));
}

}  // namespace
}  // namespace users_test
