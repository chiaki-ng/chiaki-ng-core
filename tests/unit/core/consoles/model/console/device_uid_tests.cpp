#include <libchiaking/consoles_types.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <type_traits>

namespace consoles_test {
namespace {

using libchiaking::consoles::DeviceUid;

static_assert(!std::is_default_constructible_v<DeviceUid>);

class DeviceUidText : public ::testing::Test {
 protected:
  const std::string hex = std::string(DeviceUid::text_length, 'c');
};

TEST_F(DeviceUidText, RoundTripsThroughTextAndBytes) {
  const auto uid = DeviceUid::parse(hex);
  ASSERT_TRUE(uid);
  EXPECT_EQ(uid->text(), hex);
  EXPECT_FALSE(uid->is_main_ps4());
  EXPECT_EQ(DeviceUid::parse(std::string(DeviceUid::text_length, 'C')), uid);
  EXPECT_EQ(DeviceUid::from_bytes(uid->bytes()), uid);
}

TEST_F(DeviceUidText, RefusesAShortNonHexOrZeroText) {
  EXPECT_FALSE(DeviceUid::parse(hex.substr(1)));
  EXPECT_FALSE(DeviceUid::parse(std::string(DeviceUid::text_length, 'z')));
  EXPECT_FALSE(DeviceUid::parse(std::string(DeviceUid::text_length, '0')));
}

TEST(DeviceUidMainPs4, IsEveryByteA) {
  const auto main = DeviceUid::main_ps4();
  EXPECT_TRUE(main.is_main_ps4());
  std::string expected;
  for (std::size_t index = 0; index < DeviceUid::size; ++index)
    expected += "41";
  EXPECT_EQ(main.text(), expected);
  EXPECT_TRUE(std::ranges::all_of(
      main.bytes(), [](std::byte value) { return value == std::byte{'A'}; }));
}

TEST(DeviceUidMainPs4, IsRecognisedWhenParsed) {
  const auto parsed = DeviceUid::parse(DeviceUid::main_ps4().text());
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(parsed->is_main_ps4());
}

}  // namespace
}  // namespace consoles_test
