#include "libchiaking/detail/play/internet_pairer.hpp"
#include "libchiaking/platform/crypto/openssl/provider.hpp"
#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/factory.hpp"
#include "src/core/play/adapters/links/internet/internet_pairer_core.hpp"
#include "tests/support/api/fakes.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/live_providers.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/support/users/signed_in.hpp"
#include "tests/support/websocket.hpp"
#include "tests/unit/core/play/openssl/adapters/links/internet/remote_pairing_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace play_test {

using test_support::LiveProviders;
using test_support::play::SignedInPlayer;
namespace {

namespace consoles = libchiaking::consoles;
namespace platform = libchiaking::platform;
using Rig = RemotePairingRig;
using Stage = Rig::Stage;
using StreamError = Rig::StreamError;
using Reason = consoles::PairingFailure::Reason;

class InternetPairerOverRealProviders : public ::testing::Test {
 protected:
  static constexpr std::string_view bearer = "SYNTHETIC_BEARER_TOKEN";
  static constexpr std::uint64_t whole_run_us = 30'000'000;

  auto bearer_kept_out_of_the_log() const -> testing::AssertionResult {
    for (const auto& line : log_.messages)
      if (line.find(bearer) != std::string::npos)
        return testing::AssertionFailure() << "logged: " << line;
    return testing::AssertionSuccess();
  }

  platform::openssl::OpenSslCryptoProvider crypto_;
  test_support::api::OfflineHttp http_;
  test_support::RefusingWebSocket websocket_;
  platform::DefaultPlatformFactory factory_{platform::PlatformProviders{
      .crypto = &crypto_, .http = &http_, .websocket = &websocket_}};
  test_support::FakeLog log_;
  std::shared_ptr<libchiaking::users::PsUser> user_ =
      SignedInPlayer::fresh_for_remote_play(
          bearer, libchiaking::AccountId{0x0202020202020202});
};

TEST_F(InternetPairerOverRealProviders,
       BuildsARealSessionFromAValidRemoteRequest) {
  Rig::Pairing registrar{factory_, log_};
  Rig::Pairing::Request request;
  request.user = user_;
  request.device_uid.fill(std::byte{1});
  const auto began = LiveProviders::steady_us();
  const auto result = registrar.run(std::move(request));
  EXPECT_LT(LiveProviders::steady_us() - began, whole_run_us);
  ASSERT_FALSE(result);
  const auto& failure = result.error();
  EXPECT_FALSE(failure.cancelled);
  EXPECT_NE(failure.error, StreamError::invalid_configuration);
  EXPECT_NE(failure.error, StreamError::invalid_argument);
  ASSERT_TRUE(failure.detail);
  EXPECT_EQ(failure.detail->route(), libchiaking::play::Venue::internet());
  EXPECT_GE(http_.requests + websocket_.connects.load(), 1);
  EXPECT_TRUE(bearer_kept_out_of_the_log());
  const auto logged = [&](std::string_view text) {
    return std::ranges::any_of(log_.messages, [&](const std::string& line) {
      return line.find(text) != std::string::npos;
    });
  };
  EXPECT_TRUE(logged("Starting remote connection"));
  EXPECT_TRUE(logged("session stop succeeded"));

  const auto repeated = registrar.run(Rig::Pairing::Request{});
  EXPECT_FALSE(repeated);
}

class InternetPairerPort : public InternetPairerOverRealProviders {
 protected:
  void SetUp() override {
    std::array<std::byte, 32> uid{};
    uid.fill(std::byte{1});
    const auto made = consoles::DeviceUid::from_bytes(uid);
    ASSERT_TRUE(made);
    which_ = *made;
  }

  libchiaking::play::InternetPairer pairer_{factory_, &log_};
  consoles::InternetPairing& port_ = pairer_;
  consoles::DeviceUid which_ = consoles::DeviceUid::main_ps4();
};

TEST_F(InternetPairerPort, AnswersWithTheStageTheRunFailedIn) {
  StagesHeard heard;
  const auto began = LiveProviders::steady_us();
  const auto failed =
      port_.pair(*user_, which_, consoles::Generation::ps5, &heard, {});
  EXPECT_LT(LiveProviders::steady_us() - began, whole_run_us);
  ASSERT_FALSE(failed);
  EXPECT_NE(failed.error().reason, Reason::malformed);
  EXPECT_NE(failed.error().reason, Reason::cancelled);
  EXPECT_EQ(failed.error().internet_stage, Stage::opening_session);
  EXPECT_EQ(heard.stages, std::vector{Stage::opening_session});
  EXPECT_GE(http_.requests + websocket_.connects.load(), 1);
  EXPECT_TRUE(bearer_kept_out_of_the_log());
  EXPECT_FALSE(pairer_.core().is_pairing());
}

TEST_F(InternetPairerPort, AsksNothingOnceTheCallerHasStopped) {
  const auto requests = http_.requests;
  const auto stopped = port_.pair(*user_, consoles::DeviceUid::main_ps4(),
                                  consoles::Generation::ps4, nullptr,
                                  test_support::Stopped::token());
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error().reason, Reason::cancelled);
  EXPECT_EQ(http_.requests, requests);
}

TEST_F(InternetPairerPort, AsksAHomeUserToSignInFirst) {
  const auto requests = http_.requests;
  test_support::users::SignedIn household;
  const auto home =
      household.users.add_home_user(libchiaking::AccountId{9}, "Home Player");
  ASSERT_TRUE(home);
  const auto offline =
      port_.pair(*home.value(), which_, consoles::Generation::ps5, nullptr, {});
  ASSERT_FALSE(offline);
  EXPECT_EQ(offline.error().reason, Reason::unanswered);
  ASSERT_TRUE(offline.error().unanswered);
  EXPECT_EQ(offline.error().unanswered->reason,
            libchiaking::users::Unanswered::Reason::signed_out);
  EXPECT_EQ(http_.requests, requests);
  EXPECT_EQ(household.offline.requests, 0);
}

}  // namespace
}  // namespace play_test
