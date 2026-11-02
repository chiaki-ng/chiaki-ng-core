#include "src/core/play/adapters/links/internet/internet_link.hpp"
#include "src/core/play/adapters/start/link_request.hpp"
#include "tests/support/catalog/idle_catalog.hpp"
#include "tests/support/consoles/paired_consoles.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/real_session_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
#define LIBCHIAKING_TEST_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define LIBCHIAKING_TEST_SANITIZED 1
#endif

namespace play_test {
namespace {

class SecretScan {
 public:
  static constexpr std::string_view bearer =
      "SYNTHETIC_BEARER_SECRET_TOKEN_0123456789";
  static constexpr std::string_view npsso =
      "SYNTHETIC_NPSSO_SECRET_TOKEN_0123456789";
  static constexpr std::size_t header = alignof(std::max_align_t);

  static void inspect(const unsigned char* bytes, std::size_t size) noexcept {
    if (!watching.load(std::memory_order_relaxed)) return;
    inspected.fetch_add(1, std::memory_order_relaxed);
    if (holds(bytes, size, bearer) || holds(bytes, size, npsso))
      leaked.fetch_add(1, std::memory_order_relaxed);
  }

  static inline std::atomic<bool> watching{false};
  static inline std::atomic<unsigned> leaked{0};
  static inline std::atomic<unsigned> inspected{0};

 private:
  static auto holds(const unsigned char* bytes, std::size_t size,
                    std::string_view secret) noexcept -> bool {
    for (std::size_t index = 0; index + secret.size() <= size; ++index)
      if (std::memcmp(bytes + index, secret.data(), secret.size()) == 0)
        return true;
    return false;
  }
};

}  // namespace
}  // namespace play_test

#if !defined(LIBCHIAKING_TEST_SANITIZED)
void* operator new(std::size_t size) {
  constexpr auto header = play_test::SecretScan::header;
  auto* block = static_cast<unsigned char*>(std::malloc(size + header));
  if (!block) throw std::bad_alloc{};
  std::memcpy(block, &size, sizeof size);
  return block + header;
}

void operator delete(void* pointer) noexcept {
  if (!pointer) return;
  auto* block =
      static_cast<unsigned char*>(pointer) - play_test::SecretScan::header;
  std::size_t size{};
  std::memcpy(&size, block, sizeof size);
  play_test::SecretScan::inspect(static_cast<unsigned char*>(pointer), size);
  std::free(block);
}

void operator delete(void* pointer, std::size_t) noexcept {
  operator delete(pointer);
}
#endif

namespace play_test {
namespace {

namespace consoles = libchiaking::consoles;
namespace play = libchiaking::play;
using api_test::RealStream;
using libchiaking::net::protocol::remote_play::StreamError;
using libchiaking::net::protocol::remote_play::StreamState;
using test_support::catalog::IdleCatalog;
using test_support::play::SignedInPlayer;

class Watch {
 public:
  Watch() noexcept {
    SecretScan::leaked = 0;
    SecretScan::inspected = 0;
    SecretScan::watching = true;
  }
  ~Watch() { SecretScan::watching = false; }
  Watch(const Watch&) = delete;
  auto operator=(const Watch&) -> Watch& = delete;

  auto leaks() const noexcept -> unsigned {
    SecretScan::watching = false;
    return SecretScan::leaked.load();
  }
};

class LinkSecrets : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    ASSERT_TRUE(remote_player());
    ASSERT_TRUE(cloud_player());
  }

  static auto remote_player()
      -> const std::shared_ptr<libchiaking::users::PsUser>& {
    static const auto player = SignedInPlayer::fresh_for_remote_play(
        SecretScan::bearer, libchiaking::AccountId{0x0202020202020202});
    return player;
  }

  static auto cloud_player()
      -> const std::shared_ptr<libchiaking::users::PsUser>& {
    static const auto player =
        SignedInPlayer::fresh(SignedInPlayer::saved(SecretScan::npsso));
    return player;
  }

  static auto remote_configuration() -> play::InternetLinkConfiguration {
    auto configuration = RealStream::remote();
    configuration.user = remote_player();
    return configuration;
  }

  static auto cloud_configuration() -> play::CloudLinkConfiguration {
    auto configuration = RealStream::cloud();
    configuration.user = cloud_player();
    return configuration;
  }

  static auto paired_consoles() -> test_support::consoles::PairedConsoles& {
    static test_support::consoles::PairedConsoles consoles;
    return consoles;
  }

  static auto remote_console() -> const std::shared_ptr<consoles::Console>& {
    static const auto console = [] {
      std::array<std::byte, consoles::DeviceUid::size> bytes{};
      bytes.fill(std::byte{1});
      return paired_consoles().paired(*remote_player(), "aabbccddee01",
                                      consoles::Generation::ps5, {},
                                      consoles::DeviceUid::from_bytes(bytes));
    }();
    return console;
  }

  static auto remote_target() -> consoles::PairedPlay::InternetTarget {
    return consoles::PairedPlay{*remote_console(), *remote_player()}
        .over_the_internet()
        .value();
  }

  static auto cloud_launch() -> play::CloudLinkRequest {
    play::CloudLinkRequest launch{
        cloud_player(), IdleCatalog::shared(),
        IdleCatalog::title("EP0109-PPSA07043_00-FAKE0043TITLE000")};
    launch.time_zone = "UTC+00:00";
    return launch;
  }

  static auto logged_nothing_of(RealStream& opened, std::string_view secret)
      -> testing::AssertionResult {
    for (const auto& record : opened.drain_log())
      if (record.message().find(secret) != std::string_view::npos)
        return testing::AssertionFailure() << "logged the secret";
    return testing::AssertionSuccess();
  }
};

class SecretScanTest : public ::testing::Test {
 protected:
  void SetUp() override {
#if defined(LIBCHIAKING_TEST_SANITIZED)
    GTEST_SKIP() << "the scan replaces operator new, which a sanitizer owns";
#endif
  }
};

TEST_F(SecretScanTest, SeesAPlainCopyOfTheBearer) {
  Watch watch;
  {
    const auto plain = std::make_unique<std::string>(SecretScan::bearer);
    EXPECT_EQ(plain->size(), SecretScan::bearer.size());
  }
  EXPECT_GE(watch.leaks(), 1U);
}

TEST_F(SecretScanTest, SeesAPlainCopyOfTheNpsso) {
  Watch watch;
  {
    const auto plain = std::make_unique<std::string>(SecretScan::npsso);
    EXPECT_EQ(plain->size(), SecretScan::npsso.size());
  }
  EXPECT_GE(watch.leaks(), 1U);
}

TEST_F(SecretScanTest, FindsNothingInAWipedSecret) {
  Watch watch;
  {
    libchiaking::SecretString wiped{SecretScan::bearer};
    EXPECT_EQ(wiped.view(), SecretScan::bearer);
  }
  EXPECT_GT(SecretScan::inspected.load(), 0U);
  EXPECT_EQ(watch.leaks(), 0U);
}

TEST_F(LinkSecrets, RejectingTooManyPortGuessesFreesNoBearerCopy) {
  Watch watch;
  {
    auto configuration = remote_configuration();
    configuration.port_guess_count =
        play::InternetLinkConfiguration::port_guess_limit + 1;
    const auto checked = std::move(configuration).validated();
    ASSERT_FALSE(checked);
    EXPECT_EQ(checked.error(), StreamError::invalid_configuration);
  }
  EXPECT_EQ(watch.leaks(), 0U);
}

TEST_F(LinkSecrets, RejectingAnImpossibleLossLimitFreesNoBearerCopy) {
  Watch watch;
  {
    auto configuration = remote_configuration();
    configuration.tuning.packet_loss_max = 2.0;
    const auto checked = std::move(configuration).validated();
    EXPECT_FALSE(checked);
  }
  EXPECT_EQ(watch.leaks(), 0U);
}

TEST_F(LinkSecrets, RefusingAForgottenConsoleFreesNoBearerCopy) {
  Watch watch;
  {
    const auto forgotten = paired_consoles().paired(
        *remote_player(), "aabbccddee09", consoles::Generation::ps5);
    EXPECT_TRUE(paired_consoles().known.forget(*forgotten));
    const auto target =
        consoles::PairedPlay{*forgotten, *remote_player()}.over_the_internet();
    ASSERT_FALSE(target);
    EXPECT_EQ(target.error(), consoles::PairedPlay::Refusal::forgotten);
  }
  EXPECT_EQ(watch.leaks(), 0U);
}

TEST_F(LinkSecrets, RefusingRejectedPreferencesFreesNoBearerCopy) {
  Watch watch;
  {
    play::ConsolePreferences preferences;
    preferences.tuning.packet_loss_max = 2.0;
    const auto built = play::InternetLinks::configured(
        remote_target(), remote_player(), {}, preferences, {},
        play::InternetLinkConfiguration::default_start_timeout);
    ASSERT_FALSE(built);
    EXPECT_EQ(built.error(), play::ConfigurationError::rejected);
  }
  EXPECT_EQ(watch.leaks(), 0U);
}

TEST_F(LinkSecrets, DestroyingARemoteStreamFreesNoBearerCopy) {
  Watch watch;
  {
    RealStream opened{remote_configuration()};
    EXPECT_EQ(opened->state(), StreamState::idle);
  }
  EXPECT_EQ(watch.leaks(), 0U);
}

TEST_F(LinkSecrets, DestroyingAConfiguredRemoteStreamFreesNoBearerCopy) {
  Watch watch;
  {
    auto built = play::InternetLinks::configured(
        remote_target(), remote_player(), {}, {}, {},
        play::InternetLinkConfiguration::default_start_timeout);
    ASSERT_TRUE(built);
    EXPECT_EQ(built.value().user, remote_player());
    EXPECT_EQ(built.value().user->account_id(),
              libchiaking::AccountId{0x0202020202020202});
    RealStream opened{std::move(built.value())};
    EXPECT_EQ(opened->state(), StreamState::idle);
  }
  EXPECT_EQ(watch.leaks(), 0U);
}

TEST_F(LinkSecrets, ACancelledRemoteStartFreesNoBearerCopy) {
  Watch watch;
  {
    RealStream opened{remote_configuration()};
    opened->cancel();
    const auto started = opened->start();
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error(), StreamError::route_failed);
    EXPECT_TRUE(opened->stop());
    EXPECT_TRUE(logged_nothing_of(opened, SecretScan::bearer));
  }
  EXPECT_EQ(watch.leaks(), 0U);
}

TEST_F(LinkSecrets, RejectingAZeroBitrateFreesNoNpssoCopy) {
  Watch watch;
  {
    auto configuration = cloud_configuration();
    configuration.bitrate_kbps = 0;
    const auto checked = std::move(configuration).validated();
    ASSERT_FALSE(checked);
    EXPECT_EQ(checked.error(), StreamError::invalid_configuration);
  }
  EXPECT_EQ(watch.leaks(), 0U);
}

TEST_F(LinkSecrets, RejectingACloudLaunchsLossLimitFreesNoNpssoCopy) {
  Watch watch;
  {
    auto launch = cloud_launch();
    launch.tuning.packet_loss_max = 2.0;
    const auto built = play::CloudLinkConfiguration::create(std::move(launch));
    EXPECT_FALSE(built);
  }
  EXPECT_EQ(watch.leaks(), 0U);
}

TEST_F(LinkSecrets, DestroyingACloudStreamFreesNoNpssoCopy) {
  Watch watch;
  {
    RealStream opened{cloud_configuration()};
    EXPECT_EQ(opened->state(), StreamState::idle);
  }
  {
    auto built = play::CloudLinkConfiguration::create(cloud_launch());
    ASSERT_TRUE(built);
    EXPECT_EQ(built.value().user, cloud_player());
    RealStream opened{std::move(built.value())};
    EXPECT_EQ(opened->state(), StreamState::idle);
  }
  EXPECT_EQ(watch.leaks(), 0U);
}

TEST_F(LinkSecrets, ACancelledCloudStartFreesNoNpssoCopy) {
  Watch watch;
  {
    RealStream opened{cloud_configuration()};
    opened->cancel();
    const auto started = opened->start();
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error(), StreamError::route_failed);
    EXPECT_TRUE(opened->stop());
    EXPECT_TRUE(logged_nothing_of(opened, SecretScan::npsso));
  }
  EXPECT_EQ(watch.leaks(), 0U);
}

}  // namespace
}  // namespace play_test
