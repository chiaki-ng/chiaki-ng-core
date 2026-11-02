#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/manual_pair_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kakusan_test {
namespace {

using manual::Error;
using manual::Progress;

TEST(ManualPairErrors, HaveOneDistinctNameEach) {
  std::vector<std::string_view> names;
  for (auto value = static_cast<std::uint8_t>(Error::invalid_block);
       value <= static_cast<std::uint8_t>(Error::invalid_argument); ++value) {
    const auto name = manual::error_name(static_cast<Error>(value));
    EXPECT_FALSE(name.empty());
    EXPECT_EQ(std::ranges::find(names, name), names.end()) << name;
    names.push_back(name);
  }
  EXPECT_EQ(manual::error_name(Error::no_path), "NO_PATH");
  EXPECT_EQ(manual::error_name(Error::pairing_failed), "PAIRING_FAILED");
  EXPECT_EQ(manual::error_name(Error::internal), "INTERNAL");
  EXPECT_EQ(manual::error_name(Error::invalid_argument), "INVALID_ARGUMENT");
  EXPECT_EQ(manual::error_name(static_cast<Error>(200)), "INTERNAL");
}

TEST(ManualPairIdentity, IsFreshEachTimeAndKeepsItsKeyThroughACopyOrRestore) {
  libchiaking::platform::SystemSecureRandom random;
  auto& crypto = TestPlatform::crypto();
  const auto first = manual::DeviceIdentity::create(crypto, random);
  const auto second = manual::DeviceIdentity::create(crypto, random);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  const std::array<std::byte, libchiaking::kakusan::SavedIdentity::seed_size>
      blank{};
  const auto first_saved = first.value().saved();
  const auto second_saved = second.value().saved();
  EXPECT_FALSE(std::ranges::equal(first_saved.seed, second_saved.seed));
  EXPECT_FALSE(std::ranges::equal(first_saved.seed, blank));
  const auto identity = ManualPair::derive(first.value());
  const auto again = ManualPair::derive(first.value());
  EXPECT_EQ(identity.key, again.key);
  EXPECT_EQ(identity.fingerprint, again.fingerprint);
  EXPECT_NE(identity.key.bytes(), identity.fingerprint.bytes());
  const auto other = ManualPair::derive(second.value());
  EXPECT_NE(other.key, identity.key);
  EXPECT_NE(other.fingerprint, identity.fingerprint);
  const auto copy = first.value().clone();
  const auto copy_saved = copy.saved();
  EXPECT_TRUE(std::ranges::equal(copy_saved.seed, first_saved.seed));
  EXPECT_EQ(ManualPair::derive(copy).key, identity.key);

  const auto restored = manual::DeviceIdentity::restore(crypto, first_saved);
  ASSERT_TRUE(restored);
  EXPECT_EQ(ManualPair::derive(restored.value()).key, identity.key);
  EXPECT_EQ(restored.value().fingerprint(), identity.fingerprint);
}

TEST(ManualPairSharingRules, AcceptOnlyAllowedSeatsAndButtons) {
  using Rules = manual::SharingRules;
  const auto l2 = static_cast<std::uint32_t>(manual::Button::l2);
  EXPECT_EQ(manual::required_buttons, 0x39fffU);
  EXPECT_FALSE(Rules::create(0, 0));
  EXPECT_FALSE(Rules::create(4, 0));
  EXPECT_FALSE(Rules::create(4, 5));
  EXPECT_FALSE(Rules::create(5, 1));
  EXPECT_FALSE(Rules::create(4, 1, manual::required_buttons & ~l2));
  EXPECT_FALSE(Rules::create(4, 1, manual::all_buttons | (1U << 18U)));
  EXPECT_FALSE(Rules::create(4, 1, 0));
  const auto required = Rules::create(4, 1, manual::required_buttons);
  ASSERT_TRUE(required);
  EXPECT_EQ(required.value().allowed_buttons(), manual::required_buttons);
  const auto defaulted = Rules::create(2, 2);
  ASSERT_TRUE(defaulted);
  EXPECT_EQ(defaulted.value().capacity(), 2U);
  EXPECT_EQ(defaulted.value().local_seats(), 2U);
  EXPECT_EQ(defaulted.value().allowed_buttons(), manual::all_buttons);
}

TEST(ManualPairEnvironment, CreatesAGuestWithStunServers) {
  const std::vector<std::string> servers{"192.0.2.1:3478"};
  {
    auto environment = manual::Environment::system(
        TestPlatform::crypto(), TestPlatform::secure_datagram(), servers);
    ASSERT_TRUE(environment);
    ManualPlayer player{0x31, std::move(environment)};
    ASSERT_TRUE(player.guest);
    EXPECT_EQ(player.guest->state(), Progress::waiting);
    EXPECT_TRUE(ManualPair::pump(*player.guest));
    EXPECT_LE(player.guest->next_wait(std::chrono::milliseconds{25}),
              std::chrono::milliseconds{25});
  }
  {
    const std::vector<std::string> full(manual::Environment::max_stun_servers,
                                        "192.0.2.1:3478");
    auto environment = manual::Environment::system(
        TestPlatform::crypto(), TestPlatform::secure_datagram(), full);
    ASSERT_TRUE(environment);
    ManualPlayer player{0x32, std::move(environment)};
    ASSERT_TRUE(player.guest);
    EXPECT_TRUE(ManualPair::pump(*player.guest));
  }
  Lan lan;
  Machine machine{lan, 11};
  test_support::SeededRandom random{3};
  VirtualClock clock;
  auto environment = manual::Environment::with(
      TestPlatform::crypto(), TestPlatform::secure_datagram(), machine, random,
      clock.source(), servers);
  ASSERT_TRUE(environment);
  ManualPlayer player{0x33, std::move(environment)};
  ASSERT_TRUE(player.guest);
  EXPECT_EQ(player.guest->state(), Progress::waiting);
  EXPECT_TRUE(ManualPair::pump(*player.guest));
}

TEST(ManualPairEnvironment, RefusesBadStunServersAndAMissingClock) {
  const std::vector<std::string> empty_server{""};
  EXPECT_TRUE(ManualPair::failed_with(
      manual::Environment::system(TestPlatform::crypto(),
                                  TestPlatform::secure_datagram(),
                                  empty_server),
      Error::invalid_argument));
  const std::vector<std::string> long_server{
      std::string(manual::Environment::max_stun_server_size + 1, 'a')};
  EXPECT_TRUE(ManualPair::failed_with(
      manual::Environment::system(TestPlatform::crypto(),
                                  TestPlatform::secure_datagram(), long_server),
      Error::invalid_argument));
  std::vector<std::string> servers(manual::Environment::max_stun_servers + 1,
                                   "192.0.2.1:3478");
  EXPECT_TRUE(ManualPair::failed_with(
      manual::Environment::system(TestPlatform::crypto(),
                                  TestPlatform::secure_datagram(), servers),
      Error::invalid_argument));
  servers.pop_back();
  ManualHostSide side;
  const auto config = manual::SharingRules::create(4, 1);
  ASSERT_TRUE(config);
  auto environment = manual::Environment::system(
      TestPlatform::crypto(), TestPlatform::secure_datagram(), servers);
  ASSERT_TRUE(environment);
  const auto host =
      manual::Host::create(config.value(), TestPlatform::identity(0x42),
                           std::move(environment).value(), side);
  EXPECT_TRUE(host);

  Lan lan;
  Machine machine{lan, 9};
  test_support::SeededRandom random{1};
  VirtualClock clock;
  EXPECT_TRUE(ManualPair::failed_with(
      manual::Environment::with(TestPlatform::crypto(),
                                TestPlatform::secure_datagram(), machine,
                                random, manual::Environment::ClockSource{}),
      Error::invalid_argument));
  EXPECT_TRUE(ManualPair::failed_with(
      manual::Environment::with(TestPlatform::crypto(),
                                TestPlatform::secure_datagram(), machine,
                                random, clock.source(), empty_server),
      Error::invalid_argument));
  const auto injected = manual::Environment::with(
      TestPlatform::crypto(), TestPlatform::secure_datagram(), machine, random,
      clock.source());
  ASSERT_TRUE(injected);
  EXPECT_EQ(injected.value().now().now_us, clock.now_us.load());
}

TEST(ManualPairEnvironment, UsesTheDefaultTransportOnTheSystemNetwork) {
  const auto seed = TestPlatform::identity(0x22);
  ManualHostSide side;
  const auto config =
      manual::SharingRules::create(2, 1, manual::required_buttons);
  ASSERT_TRUE(config);
  const std::vector<std::string> servers{"192.0.2.1:3478", "192.0.2.2"};
  for (std::size_t round = 0; round < 16; ++round) {
    auto environment = manual::Environment::system(
        TestPlatform::crypto(), TestPlatform::secure_datagram(), servers);
    ASSERT_TRUE(environment);
    auto host = manual::Host::create(config.value(), seed,
                                     std::move(environment).value(), side);
    ASSERT_TRUE(host);
    side.host = &host.value();
    EXPECT_TRUE(ManualPair::pump(host.value()));
    EXPECT_TRUE(
        ManualPair::failed_with(host.value().invite(), Error::source_ended));
  }
  side.host = nullptr;
  {
    ManualPlayer player{0x22};
    ASSERT_TRUE(player.guest);
    EXPECT_TRUE(ManualPair::pump(*player.guest));
  }

  Lan lan;
  Machine machine{lan, 7};
  test_support::SeededRandom random{1};
  VirtualClock clock;
  auto environment = manual::Environment::with(TestPlatform::crypto(),
                                               TestPlatform::secure_datagram(),
                                               machine, random, clock.source());
  ASSERT_TRUE(environment);
  const auto injected = manual::Host::create(
      config.value(), seed, std::move(environment).value(), side);
  ASSERT_TRUE(injected);
  EXPECT_EQ(injected.value().next_wait(std::chrono::milliseconds{40}),
            std::chrono::milliseconds{40});
}

TEST(ManualPairPreview, RefusesWhatIsNotAnInvitation) {
  EXPECT_TRUE(ManualPair::failed_with(
      manual::Guest::preview(TestPlatform::crypto(), {}),
      Error::invalid_argument));
  EXPECT_TRUE(ManualPair::failed_with(
      manual::Guest::preview(TestPlatform::crypto(), "RCP1:x"),
      Error::invalid_block));
  EXPECT_TRUE(ManualPair::failed_with(
      manual::Guest::preview(TestPlatform::crypto(), "gar"),
      Error::invalid_block));
  const std::string oversized(manual::max_block_size + 1, 'A');
  EXPECT_TRUE(ManualPair::failed_with(
      manual::Guest::preview(TestPlatform::crypto(), oversized),
      Error::invalid_block));
}

}  // namespace
}  // namespace kakusan_test
