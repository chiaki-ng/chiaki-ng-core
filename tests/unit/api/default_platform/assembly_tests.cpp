#include "libchiaking/platform/default.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>

namespace default_platform_test {
namespace {

TEST(DefaultAssembly, SuppliesEveryProvider) {
  libchiaking::platform::DefaultPlatform platform;
  auto& factory = platform.factory();
  EXPECT_TRUE(
      factory.supplies(libchiaking::platform::PlatformCapability::crypto));
  EXPECT_TRUE(
      factory.supplies(libchiaking::platform::PlatformCapability::http));
  EXPECT_TRUE(factory.supplies(
      libchiaking::platform::PlatformCapability::websocket));
  EXPECT_EQ(&factory.diagnostics(), &platform.log());
  EXPECT_EQ(&platform.factory(), &factory);
  EXPECT_EQ(&platform.http(), &factory.http());
  EXPECT_EQ(&platform.websocket(), &factory.websocket());
  EXPECT_EQ(&platform.crypto(), &factory.crypto());
  EXPECT_EQ(&platform.threading(), &factory.threading());
  EXPECT_EQ(&platform.clock(), &factory.clock());
  EXPECT_EQ(&platform.random(), &factory.random());
  EXPECT_EQ(&platform.network(), &factory.network());
}

TEST(DefaultAssembly, PoolsItsHttpTransport) {
  libchiaking::platform::DefaultPlatform platform;
  EXPECT_EQ(platform.http_transport().transport(),
            libchiaking::platform::HttpTransport::pooled);
}

TEST(DefaultAssembly, LogsUnderItsOwnAttempt) {
  libchiaking::platform::DefaultPlatform platform;
  EXPECT_FALSE(platform.log().read());
  platform.log().write({libchiaking::platform::LogSeverity::info, "assembled"});
  const auto record = platform.log().read();
  ASSERT_TRUE(record);
  EXPECT_EQ(record->message(), "assembled");
  EXPECT_EQ(record->attempt(), platform.log().attempt());
}

TEST(DefaultAssembly, CapturesNothingBeforeAnyCall) {
  libchiaking::platform::DefaultPlatform platform;
  std::array<char, 64> capture{};
  platform.api_capture().enable(true);
  EXPECT_EQ(platform.api_capture().read(capture), 0U);
}

TEST(DefaultAssembly, RunsOnTheHostProvidersItIsGiven) {
  libchiaking::platform::DefaultThreadingProvider threading;
  libchiaking::platform::SteadyClock clock;
  libchiaking::platform::SystemClock wall;
  libchiaking::platform::DefaultPlatform platform{
      libchiaking::platform::HttpTransportSettings{}, threading, clock, wall};
  auto& factory = platform.factory();
  EXPECT_EQ(&factory.threading(), &threading);
  EXPECT_EQ(&factory.clock(), &clock);
  EXPECT_EQ(&factory.wall_clock(), &wall);
  EXPECT_EQ(&platform.threading(), &threading);
  EXPECT_EQ(&platform.clock(), &clock);
  EXPECT_EQ(&platform.wall_clock(), &wall);
}

}  // namespace
}  // namespace default_platform_test
