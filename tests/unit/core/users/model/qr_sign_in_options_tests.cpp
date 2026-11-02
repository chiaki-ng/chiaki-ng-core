#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <string>

namespace users_test {
namespace {

using libchiaking::users::QrSignInOptions;

TEST(QrSignInOptions, NamesTheDeviceAsTheWebSignInDoesByDefault) {
  const QrSignInOptions options;
  EXPECT_EQ(options.device_name(), "my device");
  EXPECT_TRUE(options.device_name_usable());
}

TEST(QrSignInOptions, ChainsTheChosenDeviceName) {
  const auto options = QrSignInOptions{}.device_name("Living room Switch");
  EXPECT_EQ(options.device_name(), "Living room Switch");
  EXPECT_TRUE(options.device_name_usable());
  EXPECT_NE(options, QrSignInOptions{});

  QrSignInOptions renamed;
  renamed.device_name("first").device_name("second");
  EXPECT_EQ(renamed.device_name(), "second");
}

TEST(QrSignInOptions, AcceptsAnyUtf8UpToTheLimit) {
  EXPECT_TRUE(
      QrSignInOptions{}.device_name("任天堂 Switch").device_name_usable());
  EXPECT_TRUE(QrSignInOptions{}
                  .device_name(std::string(
                      QrSignInOptions::maximum_device_name_bytes, 'a'))
                  .device_name_usable());
}

TEST(QrSignInOptions, RefusesAnUnusableDeviceName) {
  EXPECT_FALSE(QrSignInOptions{}.device_name("").device_name_usable());
  EXPECT_FALSE(QrSignInOptions{}
                   .device_name(std::string(
                       QrSignInOptions::maximum_device_name_bytes + 1, 'a'))
                   .device_name_usable());
  EXPECT_FALSE(
      QrSignInOptions{}.device_name("line\nbreak").device_name_usable());
  EXPECT_FALSE(QrSignInOptions{}.device_name("\xff").device_name_usable());
}

}  // namespace
}  // namespace users_test
