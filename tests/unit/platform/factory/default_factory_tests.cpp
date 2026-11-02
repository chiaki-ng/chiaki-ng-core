#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/conformance.hpp>
#include <libchiaking/platform/default_factory.hpp>
#include <libchiaking/platform/net/default.hpp>
#include <libchiaking/platform/platform.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

namespace platform = libchiaking::platform;
using platform::DefaultPlatformFactory;
using platform::PlatformCapability;
using platform::PlatformFactory;

class DefaultFactory : public ::testing::Test {
 protected:
  DefaultPlatformFactory factory;
  PlatformFactory& base = factory;
};

TEST_F(DefaultFactory, ReportsTheAbiAndTheCoreCapabilities) {
  const auto info = factory.info();
  EXPECT_EQ(info.abi_version, platform::platform_abi_version);
  EXPECT_EQ(info.capabilities & PlatformCapability::threading,
            PlatformCapability::threading);
  EXPECT_EQ(info.capabilities & PlatformCapability::monotonic_clock,
            PlatformCapability::monotonic_clock);
  EXPECT_EQ(info.capabilities & PlatformCapability::secure_random,
            PlatformCapability::secure_random);
  EXPECT_EQ(info.capabilities & PlatformCapability::network,
            PlatformCapability::network);
  EXPECT_NE(info.capabilities & PlatformCapability::crypto,
            PlatformCapability::crypto);
}

TEST_F(DefaultFactory, HandsOutItsOwnProvidersThroughTheBase) {
  EXPECT_EQ(&base.network(),
            &static_cast<platform::NetworkProvider&>(factory.network()));
  EXPECT_EQ(&base.threading(),
            &static_cast<platform::ThreadingProvider&>(factory.threading()));
}

TEST_F(DefaultFactory, LeavesOptionalProvidersEmpty) {
  EXPECT_FALSE(base.supplies(platform::PlatformCapability::crypto));
  EXPECT_FALSE(base.supplies(platform::PlatformCapability::http));
  EXPECT_FALSE(base.supplies(platform::PlatformCapability::websocket));
  EXPECT_FALSE(base.supplies(platform::PlatformCapability::diagnostics));
}

TEST_F(DefaultFactory, ClockNeverRunsBackwards) {
  const auto first = base.clock().now_microseconds();
  const auto second = base.clock().now_microseconds();
  EXPECT_GE(second, first);
}

TEST_F(DefaultFactory, RandomFillsABuffer) {
  std::array<std::byte, 8> buffer{};
  EXPECT_TRUE(base.random().fill_bytes(buffer));
}

TEST_F(DefaultFactory, EachFactoryOwnsSeparateProviders) {
  DefaultPlatformFactory other;
  EXPECT_NE(&base.threading(), &other.threading());
  EXPECT_NE(&base.clock(), &other.clock());
  EXPECT_NE(&base.random(), &other.random());
}

TEST(StopToken, AnEmptyTokenIsNeverStopped) {
  platform::StopToken empty_token;
  EXPECT_FALSE(empty_token.stop_requested());
  EXPECT_FALSE(empty_token);
}

TEST(SystemSecureRandom, FillsAByte) {
  platform::SystemSecureRandom random;
  std::array<std::byte, 1> one{};
  EXPECT_TRUE(random.fill_bytes(one));
}

class ConsumedClock final : public platform::ClockProvider {
 public:
  [[nodiscard]] auto now_microseconds() const noexcept
      -> std::uint64_t override {
    platform::SteadyClock steady;
    return steady.now_microseconds();
  }
};

TEST(ClockProvider, CanBeImplementedOverTheSteadyClock) {
  ConsumedClock clock;
  EXPECT_NE(clock.now_microseconds(), 0U);
}

TEST_F(DefaultFactory, PassesThePlatformConformanceSuite) {
  DefaultPlatformFactory foreign;
  const auto report = platform::conformance::run(factory, foreign);
  EXPECT_TRUE(report.ok) << "failed mask 0x" << std::hex
                         << static_cast<unsigned>(report.failed_mask);
}

}  // namespace
