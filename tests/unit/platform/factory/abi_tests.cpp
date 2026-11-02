#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/default_factory.hpp>
#include <libchiaking/platform/net/default.hpp>
#include <libchiaking/platform/platform.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

namespace {

namespace platform = libchiaking::platform;
using platform::PlatformCapability;

TEST(PlatformAbi, IsVersionNine) {
  EXPECT_EQ(platform::platform_abi_version, 9U);
}

TEST(PlatformAbi, ProvidersAreDestroyedThroughTheirInterfaces) {
  EXPECT_TRUE(std::has_virtual_destructor_v<platform::PlatformFactory>);
  EXPECT_TRUE(std::has_virtual_destructor_v<platform::native::Thread>);
  EXPECT_TRUE(std::has_virtual_destructor_v<platform::native::Mutex>);
  EXPECT_TRUE(
      std::has_virtual_destructor_v<platform::native::ConditionVariable>);
  EXPECT_TRUE(std::has_virtual_destructor_v<platform::native::StopSource>);
  EXPECT_TRUE(std::has_virtual_destructor_v<platform::ClockProvider>);
  EXPECT_TRUE(std::has_virtual_destructor_v<platform::SecureRandomProvider>);
  EXPECT_TRUE(std::has_virtual_destructor_v<platform::CryptoProvider>);
}

TEST(PlatformAbi, LocksAndThreadsMoveButNeverCopy) {
  EXPECT_FALSE(std::is_copy_constructible_v<platform::Lock>);
  EXPECT_FALSE(std::is_copy_constructible_v<platform::Thread>);
  EXPECT_TRUE(std::is_move_constructible_v<platform::Thread>);
}

TEST(PlatformAbi, StockProvidersImplementTheirInterfaces) {
  EXPECT_TRUE(
      (std::is_base_of_v<platform::ClockProvider, platform::SteadyClock>));
  EXPECT_TRUE((std::is_base_of_v<platform::SecureRandomProvider,
                                 platform::SystemSecureRandom>));
}

TEST(PlatformAbi, CapabilityBitsKeepTheirPositions) {
  const auto bit = [](PlatformCapability capability) {
    return static_cast<std::uint32_t>(capability);
  };
  EXPECT_EQ(bit(PlatformCapability::threading), 1U << 0);
  EXPECT_EQ(bit(PlatformCapability::monotonic_clock), 1U << 1);
  EXPECT_EQ(bit(PlatformCapability::secure_random), 1U << 2);
  EXPECT_EQ(bit(PlatformCapability::network), 1U << 3);
  EXPECT_EQ(bit(PlatformCapability::crypto), 1U << 4);
  EXPECT_EQ(bit(PlatformCapability::http), 1U << 6);
  EXPECT_EQ(bit(PlatformCapability::diagnostics), 1U << 7);
  EXPECT_EQ(bit(PlatformCapability::socket_dontfrag_v4), 1U << 8);
  EXPECT_EQ(bit(PlatformCapability::socket_dontfrag_v6), 1U << 9);
  EXPECT_EQ(bit(PlatformCapability::websocket), 1U << 10);
}

}  // namespace
