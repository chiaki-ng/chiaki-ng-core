#include "src/core/play/adapters/links/internet/internet_link_configuration.hpp"
#include "tests/unit/core/net/protocol/remote_play/stream/local_prepare_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace remote_play_test {
namespace {

namespace play = libchiaking::play;
namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;

class LocalStartControlProfile : public LocalPrepareRig {};

TEST_F(LocalStartControlProfile, PresentsADualShock4ToEveryPs4) {
  for (const auto target : {remote_play::RegistrationTarget::ps4_8,
                            remote_play::RegistrationTarget::ps4_9,
                            remote_play::RegistrationTarget::ps4_10}) {
    const auto profile =
        remote_play::LocalStart::select_active_control_profile(target);
    ASSERT_TRUE(profile);
    EXPECT_EQ(profile->controller_type,
              takion::ControllerConnectionType::dualshock4);
  }
}

TEST_F(LocalStartControlProfile, PresentsADualSenseToAPs5) {
  const auto ps5 = remote_play::LocalStart::select_active_control_profile(
      remote_play::RegistrationTarget::ps5_1);
  ASSERT_TRUE(ps5);
  EXPECT_EQ(ps5->controller_type, takion::ControllerConnectionType::dualsense);
}

TEST_F(LocalStartControlProfile,
       HeartbeatsAsTheConsoleExpectsAndGivesUpOnSilence) {
  const auto ps4 = remote_play::LocalStart::select_active_control_profile(
      remote_play::RegistrationTarget::ps4_10);
  const auto ps5 = remote_play::LocalStart::select_active_control_profile(
      remote_play::RegistrationTarget::ps5_1);
  ASSERT_TRUE(ps4);
  ASSERT_TRUE(ps5);
  EXPECT_TRUE(ps4->periodic_heartbeat);
  EXPECT_FALSE(ps5->periodic_heartbeat);
  EXPECT_EQ(ps4->peer_silence_timeout_us,
            remote_play::console_peer_silence_timeout_us);
  EXPECT_EQ(ps5->peer_silence_timeout_us,
            remote_play::console_peer_silence_timeout_us);
}

TEST_F(LocalStartControlProfile, FollowsAnExplicitDualSenseChoice) {
  for (const auto target : {remote_play::RegistrationTarget::ps4_10,
                            remote_play::RegistrationTarget::ps5_1}) {
    const auto enabled =
        remote_play::LocalStart::select_active_control_profile(target, true);
    ASSERT_TRUE(enabled);
    EXPECT_EQ(enabled->controller_type,
              takion::ControllerConnectionType::dualsense);
    const auto disabled =
        remote_play::LocalStart::select_active_control_profile(target, false);
    ASSERT_TRUE(disabled);
    EXPECT_EQ(disabled->controller_type,
              takion::ControllerConnectionType::dualshock4);
  }
}

TEST_F(LocalStartControlProfile, IsRefusedForAnUnknownConsole) {
  EXPECT_FALSE(remote_play::LocalStart::select_active_control_profile(
      static_cast<remote_play::RegistrationTarget>(0xff)));
  EXPECT_FALSE(remote_play::LocalStart::select_active_control_profile(
      static_cast<remote_play::RegistrationTarget>(0xff), true));
}

class LocalStartActive : public LocalPrepareRig {
 protected:
  void SetUp() override {
    ASSERT_TRUE(id);
    ASSERT_TRUE(calibrated);
  }

  auto inputs(LocalPrepareRandom& with_random, LocalPrepareCrypto& with_crypto,
              const remote_play::RegisteredConsole& with_console)
      -> remote_play::LocalActiveInputs {
    return {clock,
            threading(),
            with_random,
            with_crypto,
            takion_crypto,
            with_console,
            remote_play::VideoCodec::h264,
            {1280, 720, 60, 10000},
            *id,
            nonce};
  }

  auto route(std::shared_ptr<LocalPrepareRoute> state)
      -> remote_play::EstablishedRoute {
    return established_route(std::move(state), *calibrated);
  }

  LocalPrepareCrypto crypto;
  LocalPrepareRandom random;
  test_support::FakeClock clock{1'000};
  LocalPrepareUnusedTakionCrypto takion_crypto;
  remote_play::RegisteredConsole console = registered_console();
  const std::array<std::byte, 16> nonce = ctrl_nonce();
  const std::optional<remote_play::SessionId> id = session_id();
  const std::optional<SenkushaResult> calibrated = calibration(12);
};

class LocalStartActiveProfile
    : public LocalStartActive,
      public ::testing::WithParamInterface<
          std::tuple<remote_play::RegistrationTarget, std::uint32_t>> {};

TEST_P(LocalStartActiveProfile, FollowsTheConsoleRatherThanTheCalibration) {
  const auto [target, selected] = GetParam();
  console.target = target;
  auto state = std::make_shared<LocalPrepareRoute>();
  auto established = route(state);
  const auto chosen = calibration(selected);
  ASSERT_TRUE(chosen);
  established.calibration = *chosen;
  auto active = remote_play::LocalStart::start_active(
      std::move(established), inputs(random, crypto, console));
  ASSERT_TRUE(active);
  const auto version = console_version(target);
  EXPECT_EQ(active.value()->snapshot().active_version, version);
  EXPECT_EQ(crypto.generated_curve,
            (version == 10 || version >= 13 ? platform::EcdhCurve::secp521r1
                                            : platform::EcdhCurve::secp256k1));
  EXPECT_TRUE(active.value()->request_stop());
  EXPECT_EQ(state->close_count, 1U);
}

INSTANTIATE_TEST_SUITE_P(
    ConsolesAndCalibratedVersions, LocalStartActiveProfile,
    ::testing::Combine(
        ::testing::Values(remote_play::RegistrationTarget::ps4_9,
                          remote_play::RegistrationTarget::ps5_1),
        ::testing::Values(9U, 12U, 20U)));

TEST_F(LocalStartActive, PassesRequestedAudioChannelsToThePs5) {
  console.target = remote_play::RegistrationTarget::ps5_1;
  auto active_inputs = inputs(random, crypto, console);
  active_inputs.tuning.requested_audio_channels = 12;
  auto accepted_state = std::make_shared<LocalPrepareRoute>();
  auto accepted = remote_play::LocalStart::start_active(route(accepted_state),
                                                        active_inputs);
  ASSERT_TRUE(accepted);
  EXPECT_TRUE(accepted.value()->request_stop());
}

TEST_F(LocalStartActive, ClosesTheRouteForAnUnsupportedAudioLayout) {
  console.target = remote_play::RegistrationTarget::ps5_1;
  auto active_inputs = inputs(random, crypto, console);
  active_inputs.tuning.requested_audio_channels = 7;
  auto state = std::make_shared<LocalPrepareRoute>();
  const auto rejected =
      remote_play::LocalStart::start_active(route(state), active_inputs);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(),
            remote_play::LocalActiveStartError::prepare_failed);
  EXPECT_EQ(state->close_count, 1U);
  EXPECT_TRUE(state->sent.empty());
}

TEST_F(LocalStartActive, ReportsABangKeyMismatchForASynthesizedSessionId) {
  const std::array<std::byte, remote_play::session_id_random_size> random48{};
  auto fallback = remote_play::SessionId::fallback(15, random48);
  ASSERT_TRUE(fallback);
  EXPECT_TRUE(fallback.value().synthesized());
  const remote_play::LocalActiveInputs synthesized_inputs{
      clock,
      threading(),
      random,
      crypto,
      takion_crypto,
      console,
      remote_play::VideoCodec::h264,
      {1280, 720, 60, 10000},
      fallback.value(),
      nonce};
  auto state = std::make_shared<LocalPrepareRoute>();
  auto active =
      remote_play::LocalStart::start_active(route(state), synthesized_inputs);
  ASSERT_TRUE(active);
  EXPECT_EQ(active.value()->control_profile().bang_session_key,
            remote_play::BangSessionKeyPolicy::report_mismatch);
  EXPECT_TRUE(active.value()->request_stop());
}

TEST_F(LocalStartActive, StartsOnceOverACalibratedRouteWithTheDefaultProfile) {
  auto route_state = std::make_shared<LocalPrepareRoute>();
  auto active = remote_play::LocalStart::start_active(
      route(route_state), inputs(random, crypto, console));
  ASSERT_TRUE(active);
  EXPECT_FALSE(active.value()->control_profile().enable_idr_on_fec_failure);
  EXPECT_FALSE(id->synthesized());
  EXPECT_EQ(active.value()->control_profile().bang_session_key,
            remote_play::BangSessionKeyPolicy::required);
  EXPECT_EQ(active.value()->control_profile().source_tap, nullptr);
  EXPECT_TRUE(active.value()->control_profile().takion_offer.empty());
  EXPECT_EQ(route_state->sent.size(), 1U);
  EXPECT_EQ(route_state->close_count, 0U);
  EXPECT_EQ(crypto.generate_calls, 1U);
  EXPECT_EQ(active.value()->state(),
            remote_play::ActiveHandshakeState::awaiting_connection);
  EXPECT_TRUE(active.value()->request_stop());
  active.value().reset();
  EXPECT_EQ(route_state->close_count, 1U);
}

TEST_F(LocalStartActive, LeavesIdrOnFecFailureOffInTheLinkDefaults) {
  EXPECT_FALSE(play::HomeLinkConfiguration{}.enable_idr_on_fec_failure);
  EXPECT_FALSE(play::InternetLinkConfiguration{}.enable_idr_on_fec_failure);
}

class LocalStartIgnoringTap final : public takion::SourceTap {
 public:
  void on_source_keys(std::uint32_t,
                      const takion::SourceReceiveKeys&) noexcept override {}
  void on_source_datagram(const takion::SourceDatagram&) noexcept override {}
};

TEST_F(LocalStartActive, CarriesIdrTapAndNegotiationIntoTheProfile) {
  LocalPrepareCrypto idr_crypto;
  LocalPrepareRandom idr_random;
  auto idr_inputs = inputs(idr_random, idr_crypto, console);
  idr_inputs.enable_idr_on_fec_failure = true;
  LocalStartIgnoringTap tap;
  idr_inputs.tuning.source_tap = &tap;
  idr_inputs.tuning.negotiate_takion_version = true;
  auto idr_state = std::make_shared<LocalPrepareRoute>();
  auto idr_active =
      remote_play::LocalStart::start_active(route(idr_state), idr_inputs);
  ASSERT_TRUE(idr_active);
  EXPECT_TRUE(idr_active.value()->control_profile().enable_idr_on_fec_failure);
  EXPECT_EQ(idr_active.value()->control_profile().source_tap, &tap);
  const std::vector<std::uint32_t> remote_play_offer{9,  10, 11, 13, 14, 15,
                                                     16, 17, 18, 19, 20};
  EXPECT_EQ(idr_active.value()->control_profile().takion_offer,
            remote_play_offer);
  EXPECT_TRUE(idr_active.value()->request_stop());
}

TEST_F(LocalStartActive, ClosesTheRouteWhenPreparationFails) {
  auto invalid_state = std::make_shared<LocalPrepareRoute>();
  auto invalid_inputs = inputs(random, crypto, console);
  invalid_inputs.nonce = std::span<const std::byte>{nonce}.first(15);
  const auto failed = remote_play::LocalStart::start_active(
      route(invalid_state), invalid_inputs);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), remote_play::LocalActiveStartError::prepare_failed);
  EXPECT_EQ(invalid_state->close_count, 1U);
  EXPECT_EQ(random.calls, 0U);
}

TEST_F(LocalStartActive, ClosesTheRouteWhenTheSessionCannotStart) {
  LocalPrepareRandom start_fail_random;
  start_fail_random.fail_on_call = 2;
  LocalPrepareCrypto start_fail_crypto;
  auto start_fail_state = std::make_shared<LocalPrepareRoute>();
  const auto start_failed = remote_play::LocalStart::start_active(
      route(start_fail_state),
      inputs(start_fail_random, start_fail_crypto, console));
  ASSERT_FALSE(start_failed);
  EXPECT_EQ(start_failed.error(),
            remote_play::LocalActiveStartError::start_failed);
  EXPECT_EQ(start_fail_random.calls, 2U);
  EXPECT_EQ(start_fail_state->close_count, 1U);
}

TEST_F(LocalStartActive, RefusesARouteWithoutATransport) {
  remote_play::EstablishedRoute missing{
      {nullptr,
       {},
       {},
       remote_play::RouteKind::local_direct,
       remote_play::DontFragmentPolicy::enabled},
      *calibrated};
  const auto missing_result = remote_play::LocalStart::start_active(
      std::move(missing), inputs(random, crypto, console));
  ASSERT_FALSE(missing_result);
  EXPECT_EQ(missing_result.error(),
            remote_play::LocalActiveStartError::invalid_route);
  EXPECT_EQ(random.calls, 0U);
}

TEST_F(LocalStartActive, RefusesAnUnsupportedConsoleAndClosesTheRoute) {
  auto unsupported_console = registered_console();
  unsupported_console.target =
      static_cast<remote_play::RegistrationTarget>(0xff);
  auto unsupported_state = std::make_shared<LocalPrepareRoute>();
  const auto unsupported = remote_play::LocalStart::start_active(
      route(unsupported_state), inputs(random, crypto, unsupported_console));
  ASSERT_FALSE(unsupported);
  EXPECT_EQ(unsupported.error(),
            remote_play::LocalActiveStartError::invalid_route);
  EXPECT_EQ(unsupported_state->close_count, 1U);
  EXPECT_EQ(random.calls, 0U);
}

class LocalStartServerType : public LocalPrepareRig {
 protected:
  static auto apply(remote_play::VideoCodec codec,
                    remote_play::LaunchSpecVideo video,
                    std::optional<std::uint8_t> type, bool downgrade) {
    return remote_play::LocalStart::apply_server_type({codec, video}, type,
                                                      downgrade);
  }

  static constexpr remote_play::LaunchSpecVideo full{1920, 1080, 60, 15000};
};

TEST_F(LocalStartServerType, DowngradesAPs4To720pAvc) {
  const auto ps4 = apply(remote_play::VideoCodec::h265, full, 0, true);
  EXPECT_EQ(ps4.codec, remote_play::VideoCodec::h264);
  EXPECT_EQ(ps4.video.width, 1280U);
  EXPECT_EQ(ps4.video.height, 720U);
  EXPECT_EQ(ps4.video.max_fps, 60U);
  EXPECT_EQ(ps4.video.bitrate_kbps, 10000U);
}

TEST_F(LocalStartServerType, DropsAnUncommonFrameRateToThirty) {
  const auto thirty =
      apply(remote_play::VideoCodec::h264, {1920, 1080, 50, 15000}, 0, true);
  EXPECT_EQ(thirty.video.height, 720U);
  EXPECT_EQ(thirty.video.max_fps, 30U);
}

TEST_F(LocalStartServerType, KeepsTheResolutionWithoutAutoDowngrade) {
  const auto kept = apply(remote_play::VideoCodec::h265, full, 0, false);
  EXPECT_EQ(kept.codec, remote_play::VideoCodec::h264);
  EXPECT_EQ(kept.video.height, 1080U);
  EXPECT_EQ(kept.video.bitrate_kbps, 15000U);
}

TEST_F(LocalStartServerType, GivesAPs4ProAvcAt1080p) {
  const auto pro = apply(remote_play::VideoCodec::h265_hdr, full, 1, true);
  EXPECT_EQ(pro.codec, remote_play::VideoCodec::h264);
  EXPECT_EQ(pro.video.height, 1080U);
}

TEST_F(LocalStartServerType, KeepsAPs5Request) {
  const auto ps5 = apply(remote_play::VideoCodec::h265_hdr, full, 2, true);
  EXPECT_EQ(ps5.codec, remote_play::VideoCodec::h265_hdr);
  EXPECT_EQ(ps5.video.height, 1080U);
}

TEST_F(LocalStartServerType, KeepsTheRequestForAnUnknownServer) {
  const auto unknown =
      apply(remote_play::VideoCodec::h265, full, std::nullopt, true);
  EXPECT_EQ(unknown.codec, remote_play::VideoCodec::h265);
  EXPECT_EQ(unknown.video.height, 1080U);
}

TEST_F(LocalStartServerType, LeavesASmallRequestAlone) {
  const auto small =
      apply(remote_play::VideoCodec::h264, {1280, 720, 30, 5000}, 0, true);
  EXPECT_EQ(small.video.width, 1280U);
  EXPECT_EQ(small.video.max_fps, 30U);
  EXPECT_EQ(small.video.bitrate_kbps, 5000U);
}

class LocalStartRouteConfig : public LocalPrepareRig {
 protected:
  void SetUp() override {
    base.local = {platform::IpAddress::any_ipv4(), 0};
    base.remote = {platform::IpAddress::any_ipv4(), 9297};
    base.dont_fragment = remote_play::DontFragmentPolicy::enabled;
    ASSERT_TRUE(fallback);
  }

  auto established(remote_play::DontFragmentPolicy policy)
      -> remote_play::EstablishedRoute {
    return {{nullptr,
             {},
             {console, 9297},
             remote_play::RouteKind::local_direct,
             policy},
            *fallback};
  }

  remote_play::DirectRouteConfig base;
  const platform::IpAddress console =
      platform::IpAddress::ipv4({192, 168, 1, 5});
  const std::optional<SenkushaResult> fallback =
      remote_play::LocalStreamPreparation::make_reference_fallback_calibration(
          remote_play::RegistrationTarget::ps5_1);
};

TEST_F(LocalStartRouteConfig, KeepsDontFragmentClearedOnARetainedRoute) {
  const auto cleared = remote_play::LocalStart::active_route_config(
      base, established(remote_play::DontFragmentPolicy::disabled), 9296);
  EXPECT_EQ(cleared.dont_fragment, remote_play::DontFragmentPolicy::disabled);
  EXPECT_EQ(cleared.remote.address, console);
  EXPECT_EQ(cleared.remote.port, 9296U);
}

TEST_F(LocalStartRouteConfig, KeepsDontFragmentOnACalibratedRoute) {
  const auto kept = remote_play::LocalStart::active_route_config(
      base, established(remote_play::DontFragmentPolicy::enabled), 9296);
  EXPECT_EQ(kept.dont_fragment, remote_play::DontFragmentPolicy::enabled);
}

}  // namespace
}  // namespace remote_play_test
