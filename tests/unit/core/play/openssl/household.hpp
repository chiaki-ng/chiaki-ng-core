#pragma once

#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/testing/service_builder.hpp"
#include "src/core/consoles/adapters/roster/console_facts.hpp"
#include "src/core/play/adapters/links/internet/internet_link.hpp"
#include "src/core/play/adapters/links/internet/internet_pairer_core.hpp"
#include "src/core/play/play_core.hpp"
#include "tests/support/api/fakes.hpp"
#include "tests/support/catalog/idle_catalog.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/play/launch_rig.hpp"
#include "tests/support/play/scripted_cloud_ps.hpp"
#include "tests/support/play/session_rig.hpp"
#include "tests/support/waiter.hpp"
#include "tests/unit/core/play/outcome_checks.hpp"

#include <libchiaking/detail/consoles/known_consoles.hpp>
#include <libchiaking/detail/play/play.hpp>
#include <libchiaking/platform/crypto/openssl/provider.hpp>
#include <libchiaking/platform/factory.hpp>
#include <libchiaking/platform/runtime/random_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/platform/websocket/provider.hpp>
#include <libchiaking/testing/fake_console.hpp>
#include <libchiaking/testing/in_memory_console_store.hpp>
#include <libchiaking/testing/in_memory_network.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test {

using test_support::play::SessionRig;

using test_support::FakeWaiter;
using test_support::LiveProviders;
using test_support::catalog::IdleCatalog;
using test_support::play::MemoryDatacenterStore;
using test_support::play::RecordedEvents;
using test_support::play::RecordedMedia;
using test_support::play::ScriptedClearance;
using test_support::play::ScriptedCloudPs;
using test_support::play::SwitchableClearance;
using test_support::play::Title;

class SkippingClock final : public libchiaking::platform::ClockProvider {
 public:
  std::uint64_t now_microseconds() const noexcept override {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        LiveProviders::now().time_since_epoch());
    return static_cast<std::uint64_t>(elapsed.count()) +
           skipped_.load(std::memory_order_acquire);
  }

  void skip(std::chrono::seconds ahead) noexcept {
    skipped_.fetch_add(static_cast<std::uint64_t>(ahead.count()) * 1'000'000U,
                       std::memory_order_acq_rel);
  }

 private:
  std::atomic<std::uint64_t> skipped_{};
};

class Household {
 public:
  using Generation = libchiaking::consoles::Generation;
  using SessionState = libchiaking::play::SessionState;
  using StartRefusal = libchiaking::play::StartRefusal;
  using Cause = libchiaking::play::EndReason::Cause;
  using Started =
      libchiaking::Expected<std::shared_ptr<libchiaking::play::Session>,
                            StartRefusal>;

  static constexpr std::string_view living_room = "AABBCCDDEEFF";
  static constexpr std::string_view wake_digits = "3a5f9c1e";
  static constexpr std::string_view cloud_title =
      "EP0109-PPSA07033_00-FAKE0030TITLE000";

  Household()
      : factory(
            libchiaking::platform::PlatformProviders{.clock = &clock,
                                                     .network = &network,
                                                     .crypto = &crypto,
                                                     .http = &ps,
                                                     .websocket = &websocket,
                                                     .diagnostics = &log}),
        pairer(factory, &log),
        known(libchiaking::testing::ServiceBuilder::build<
              libchiaking::consoles::KnownConsoles>(
            network, offline, crypto, random, store, factory.threading(), clock,
            &pairer, &log)),
        play(libchiaking::testing::ServiceBuilder::build<
             libchiaking::play::Play>(
            std::make_unique<libchiaking::play::Play::Core>(
                factory, known, IdleCatalog::shared(), datacenters, pairer,
                &log, starting_with()))) {}

  static auto keys() -> libchiaking::consoles::PairingKeys {
    using libchiaking::consoles::PairingKeys;
    std::array<std::byte, PairingKeys::registration_key_size> registration{};
    std::ranges::transform(wake_digits, registration.begin(), [](char digit) {
      return static_cast<std::byte>(digit);
    });
    std::array<std::byte, PairingKeys::remote_play_key_size> remote_play{};
    remote_play.fill(std::byte{0x5a});
    auto created = PairingKeys::create(registration, remote_play, 2);
    EXPECT_TRUE(created);
    return std::move(created).value();
  }

  static auto device(std::byte fill) -> libchiaking::consoles::DeviceUid {
    std::array<std::byte, libchiaking::consoles::DeviceUid::size> bytes{};
    bytes.fill(fill);
    const auto made = libchiaking::consoles::DeviceUid::from_bytes(bytes);
    EXPECT_TRUE(made);
    return made.value();
  }

  auto paired(const libchiaking::users::PsUser& as, std::string_view id,
              Generation generation, std::string_view address = {},
              std::optional<libchiaking::consoles::DeviceUid> device_uid = {})
      -> std::shared_ptr<libchiaking::consoles::Console> {
    namespace consoles = libchiaking::consoles;
    const auto hardware_id = consoles::HardwareId::parse(id);
    EXPECT_TRUE(hardware_id);
    consoles::SavedConsole row{
        hardware_id.value(), consoles::Firmware::newest(generation),
        consoles::ConsoleName::of(generation), std::nullopt};
    if (!address.empty()) {
      auto host = consoles::Host::create(address, generation);
      EXPECT_TRUE(host);
      row.last_address = std::move(host).value();
    }
    const auto console_saved = store.save(row);
    const auto pairing_saved = store.save(consoles::SavedPairing{
        as.account_id(), hardware_id.value(), keys(), device_uid});
    EXPECT_TRUE(console_saved);
    EXPECT_TRUE(pairing_saved);
    const auto remembered = known.remember_saved();
    EXPECT_TRUE(remembered);
    auto console = known.find(hardware_id.value());
    EXPECT_NE(console, nullptr);
    return console;
  }

  auto home_user(std::uint64_t account = 40)
      -> std::shared_ptr<libchiaking::users::PsUser> {
    const auto added = household.users.add_home_user(
        libchiaking::AccountId{account}, "Living Room Player");
    EXPECT_TRUE(added);
    return added.value();
  }

  template <class Ended>
  auto skip_until(const Ended& ended) -> testing::AssertionResult {
    const auto deadline = LiveProviders::now() + std::chrono::seconds{60};
    while (!ended()) {
      if (LiveProviders::now() >= deadline)
        return testing::AssertionFailure()
               << "the session had not ended after a minute";
      clock.skip(std::chrono::seconds{1});
      LiveProviders::nap(std::chrono::milliseconds{2});
    }
    return testing::AssertionSuccess();
  }

  static auto refusal(const Started& started)
      -> std::optional<StartRefusal::Kind> {
    return started ? std::nullopt : std::optional{started.error().kind()};
  }

  static auto description(std::uint32_t width, std::uint32_t height)
      -> libchiaking::media::StreamDescription {
    libchiaking::media::StreamDescription description;
    description.audio.channels = 2;
    description.audio.rate = 48000;
    description.profiles.push_back(
        {width, height, {std::byte{1}, std::byte{2}, std::byte{3}}});
    description.picture = libchiaking::media::GrantedPicture{
        libchiaking::media::VideoCodec::h265, width, height, 60, 10000};
    return description;
  }

  static auto frame(std::uint16_t index) -> libchiaking::media::VideoFrame {
    libchiaking::media::VideoFrame::Facts facts;
    facts.bytes = {std::byte{4}, std::byte{5}, std::byte{6}, std::byte{7}};
    facts.frame_index = index;
    return libchiaking::media::VideoFrame{facts};
  }

  static auto seat(std::uint8_t index) -> libchiaking::controllers::SeatNumber {
    return libchiaking::controllers::SeatNumber::create(index).value();
  }

  static void reconnecting(const RecordedEvents& events, std::uint32_t attempt,
                           libchiaking::play::ConnectStep step, Cause cause) {
    namespace shared = libchiaking::progress;
    namespace play = libchiaking::play;
    play::ConnectProgress expected{step, {}, attempt, true};
    expected.wait = shared::Wait{step == play::ConnectStep::waking_console
                                     ? shared::WaitReason::console_waking
                                     : shared::WaitReason::connection_retry,
                                 play::StartPolicy::rest_between_attempts};
    expected.previous_failure =
        play::AttemptFailure{cause, std::nullopt, false};
    SessionRig::eventually([&] {
      const auto progress = events.progress();
      return !progress.empty() && progress.back().attempt == attempt &&
             progress.back().previous_failure &&
             progress.back().previous_failure->cause == cause;
    });
    auto last = events.progress().back();
    ASSERT_TRUE(last.previous_failure);
    last.previous_failure->console_quit.reset();
    EXPECT_EQ(last, expected);
    EXPECT_EQ(last.status(),
              (shared::Status{std::nullopt, attempt,
                              std::chrono::microseconds{0}, expected.wait}));
  }

  auto starting_with() -> libchiaking::play::StartCollaborators {
    auto settings = libchiaking::play::Session::Core::core_settings();
    settings.commands.rest_flush = std::chrono::milliseconds{20};
    return {answering, waiter, time_zone, settings};
  }

  static void playing_again(const RecordedEvents& events, std::size_t times) {
    SessionRig::eventually([&] {
      return std::ranges::count(events.states(), SessionState::playing) ==
             static_cast<std::ptrdiff_t>(times);
    });
  }

  SkippingClock clock;
  libchiaking::testing::InMemoryNetwork network{LiveProviders::threading(),
                                                clock};
  libchiaking::platform::openssl::OpenSslCryptoProvider crypto;
  libchiaking::platform::SystemSecureRandom random;
  ScriptedCloudPs ps;
  test_support::api::InterruptibleHttp offline;
  test_support::RefusingWebSocket websocket;
  test_support::FakeLog log;
  libchiaking::platform::DefaultPlatformFactory factory;
  libchiaking::testing::InMemoryConsoleStore store{LiveProviders::threading()};
  MemoryDatacenterStore datacenters;
  ScriptedClearance clearance{Title{.product = cloud_title}};
  FakeWaiter waiter;
  SwitchableClearance answering{clearance};
  libchiaking::platform::SystemUtcOffset time_zone;
  test_support::users::SignedIn household;
  libchiaking::play::InternetPairer pairer;
  libchiaking::consoles::KnownConsoles known;
  libchiaking::play::Play play;
  RecordedEvents events;
  RecordedMedia media;
};

}  // namespace play_test
