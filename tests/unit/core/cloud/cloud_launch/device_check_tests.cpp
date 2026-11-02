#include "libchiaking/platform/net/default.hpp"
#include "libchiaking/testing/service_builder.hpp"
#include "src/core/play/adapters/cloud_launch/cloud_launch_core.hpp"
#include "src/core/users/ports/play_calls.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/play/launch_rig.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/random.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/support/users/signed_in.hpp"
#include "tests/support/waiter.hpp"

#include <libchiaking/detail/users/signed_in_users.hpp>
#include <libchiaking/testing/in_memory_credential_store.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cloud_test {
namespace {

namespace catalog = libchiaking::catalog;
namespace play = libchiaking::play;
namespace platform = libchiaking::platform;
namespace users = libchiaking::users;
using IdentityDetail = libchiaking::psn::gk::Error::IdentityDetail;
using Identified = libchiaking::Expected<void, libchiaking::psn::gk::Error>;

class DeviceCheckProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    EXPECT_NE(random, nullptr);
    if (random != nullptr) {
      EXPECT_EQ(random->calls, 0U);
    }
    ++calls;
    const std::string body{reinterpret_cast<const char*>(request.body.data()),
                           request.body.size()};
    EXPECT_NE(body.find("0000000700410080000102030405060708090a0b0c0d0e0f"),
              std::string::npos);
    if (error) return libchiaking::unexpected(*error);
    platform::HttpResponse response{};
    response.status_code = status;
    return response;
  }

  test_support::FailingRandom* random{};
  std::optional<platform::HttpError> error;
  std::uint16_t status{204};
  unsigned calls{};
};

class Admission {
 public:
  explicit Admission(test_support::CountingRandom& random)
      : users(libchiaking::testing::ServiceBuilder::build<users::SignedInUsers>(
            offline, credentials, test_support::process_threading(), clock,
            wall, random)) {}

  auto admit(users::SavedSignIn saved) -> std::shared_ptr<users::PsUser> {
    const auto account = saved.account;
    EXPECT_TRUE(credentials.save(saved));
    EXPECT_TRUE(users.welcome_back());
    return users.find(account);
  }

  static auto saved(std::string_view npsso, std::uint64_t account)
      -> users::SavedSignIn {
    return users::SavedSignIn{libchiaking::AccountId{account}, "player",
                              libchiaking::psn::auth::Npsso{npsso}};
  }

  test_support::users::Offline offline;
  test_support::users::SystemWall clock;
  test_support::users::SystemWall wall;
  libchiaking::testing::InMemoryCredentialStore credentials{
      test_support::process_threading()};
  users::SignedInUsers users;
};

class CloudLaunchDeviceCheck : public ::testing::Test {
 protected:
  static constexpr std::string_view device =
      "0000000700410080000102030405060708090a0b0c0d0e0f";

  void SetUp() override {
    provider.random = &random;
    waiter.answer = false;
    minted = admission.admit(Admission::saved("SYNTHETIC_TOKEN", 1));
    ASSERT_TRUE(minted);
    EXPECT_EQ(minting.calls, 1U);
    EXPECT_EQ(minting.last_size, std::size_t{16});
  }

  static auto launching(platform::StopToken stop,
                        std::shared_ptr<users::PsUser> user)
      -> test_support::play::LaunchCase {
    test_support::play::LaunchCase launched;
    launched.user = std::move(user);
    launched.stop = std::move(stop);
    return launched;
  }

  auto establish(const test_support::play::LaunchCase& launched) -> Identified {
    auto clearance = launched.clearance();
    test_support::play::RecordingListener listener;
    play::CloudLaunch::Core core{
        {provider, network, clock, test_support::process_threading(), random,
         waiter, clearance, launched.utc_offset},
        launched.request(),
        listener};
    auto checked = core.check_device();
    EXPECT_EQ(clearance.clears, 0);
    return checked;
  }

  auto establish(std::shared_ptr<users::PsUser> user,
                 platform::StopToken stop = {}) -> Identified {
    return establish(launching(std::move(stop), std::move(user)));
  }

  static auto failed(const Identified& result, IdentityDetail detail)
      -> testing::AssertionResult {
    if (result) return testing::AssertionFailure() << "the device was checked";
    if (result.error().stage() != libchiaking::psn::gk::Error::Stage::identity)
      return testing::AssertionFailure() << "failed outside the identity stage";
    if (result.error().detail() != static_cast<std::uint32_t>(detail))
      return testing::AssertionFailure()
             << "detail " << result.error().detail();
    return testing::AssertionSuccess();
  }

  auto held_device_user(std::uint64_t account, std::string_view text)
      -> std::shared_ptr<users::PsUser> {
    auto kept = Admission::saved("SYNTHETIC_TOKEN", account);
    kept.device = users::DeviceId::parse(text);
    return admission.admit(std::move(kept));
  }

  test_support::FailingRandom random;
  test_support::CountingRandom minting;
  Admission admission{minting};
  DeviceCheckProvider provider;
  platform::SteadyClock steady;
  platform::DefaultNetworkProvider network{test_support::process_threading(),
                                           steady};
  test_support::FakeClock clock{1'000};
  test_support::FakeWaiter waiter;
  std::shared_ptr<users::PsUser> minted;
};

TEST_F(CloudLaunchDeviceCheck, ChecksTheMintedDeviceInOneRequest) {
  EXPECT_TRUE(establish(minted));
  EXPECT_EQ(minting.calls, 1U);
  EXPECT_EQ(random.calls, 0U);
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(CloudLaunchDeviceCheck, IsCancelledOnceStopped) {
  EXPECT_TRUE(failed(
      establish(minted,
                platform::StopToken{std::make_shared<test_support::Stopped>()}),
      IdentityDetail::cancelled));
  EXPECT_EQ(minting.calls, 1U);
  EXPECT_EQ(provider.calls, 0U);
}

TEST_F(CloudLaunchDeviceCheck, AdmitsNoUserWhenMintingFails) {
  minting.fail = true;
  EXPECT_FALSE(admission.admit(Admission::saved("SYNTHETIC_TOKEN", 2)));
  EXPECT_EQ(minting.calls, 2U);
  EXPECT_EQ(provider.calls, 0U);
}

TEST_F(CloudLaunchDeviceCheck, IsRejectedByAnErrorStatus) {
  provider.status = 401;
  EXPECT_TRUE(failed(establish(minted), IdentityDetail::rejected));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(CloudLaunchDeviceCheck, ReportsABackendFailureAsTransport) {
  provider.error = platform::HttpError::backend_failure;
  EXPECT_TRUE(failed(establish(minted), IdentityDetail::transport_failure));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(CloudLaunchDeviceCheck, IsCancelledWithTheRequest) {
  provider.error = platform::HttpError::cancelled;
  EXPECT_TRUE(failed(establish(minted), IdentityDetail::cancelled));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(CloudLaunchDeviceCheck, RefusesAMissingUserBeforeSending) {
  EXPECT_FALSE(admission.admit(Admission::saved("BAD TOKEN", 3)));
  EXPECT_TRUE(failed(establish(nullptr), IdentityDetail::invalid_request));
  EXPECT_EQ(provider.calls, 0U);
}

TEST_F(CloudLaunchDeviceCheck, KeepsAHeldDeviceForEitherService) {
  const auto minted_before = minting.calls;
  minting.fail = true;
  auto stable = launching({}, held_device_user(4, device));
  ASSERT_TRUE(stable.user);
  stable.title.service = catalog::ServiceGeneration::old_service;
  EXPECT_TRUE(establish(stable));
  stable.title.service = catalog::ServiceGeneration::ps5_cloud;
  EXPECT_TRUE(establish(stable));
  const auto held_device = users::PlayCalls{*stable.user}.cloud_device();
  ASSERT_TRUE(held_device);
  EXPECT_EQ(held_device.value().view(), device);
  EXPECT_EQ(minting.calls, minted_before);
}

TEST_F(CloudLaunchDeviceCheck, ChecksAHeldUppercaseDeviceInLowercase) {
  const auto minted_before = minting.calls;
  minting.fail = true;
  auto stable = launching(
      {},
      held_device_user(5, "0000000700410080000102030405060708090A0B0C0D0E0F"));
  ASSERT_TRUE(stable.user);
  EXPECT_TRUE(establish(stable));
  EXPECT_EQ(minting.calls, minted_before);
}

TEST_F(CloudLaunchDeviceCheck, ParsesNoMalformedDuid) {
  const auto calls_before_prefix = provider.calls;
  EXPECT_FALSE(users::DeviceId::parse(
      "1111111111111111000102030405060708090a0b0c0d0e0f"));
  EXPECT_EQ(provider.calls, calls_before_prefix);
  EXPECT_FALSE(users::DeviceId::parse("invalid"));
  EXPECT_EQ(minting.calls, 1U);
  EXPECT_EQ(random.calls, 0U);
}

}  // namespace
}  // namespace cloud_test
