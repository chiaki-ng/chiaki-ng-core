#pragma once

#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/testing/service_builder.hpp"
#include "src/core/consoles/adapters/roster/console_facts.hpp"
#include "src/core/play/play_core.hpp"
#include "tests/support/api/fakes.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/play/launch_rig.hpp"
#include "tests/support/play/scripted_cloud_ps.hpp"
#include "tests/support/play/session_rig.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/waiter.hpp"
#include "tests/support/websocket.hpp"

#include <libchiaking/chiaki_ng.hpp>
#include <libchiaking/platform/crypto/openssl/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/platform/websocket/provider.hpp>
#include <libchiaking/testing/fake_console.hpp>
#include <libchiaking/testing/fake_secure_datagram.hpp>
#include <libchiaking/testing/in_memory_console_store.hpp>
#include <libchiaking/testing/in_memory_credential_store.hpp>
#include <libchiaking/testing/in_memory_network.hpp>
#include <libchiaking/testing/in_memory_record_store.hpp>
#include <libchiaking/testing/play/scripted_link.hpp>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace chiaki_ng_test {

using test_support::FakeWaiter;

namespace catalog = libchiaking::catalog;
namespace consoles = libchiaking::consoles;
namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
namespace kakusan = libchiaking::kakusan;
namespace fakes = libchiaking::testing;
namespace users = libchiaking::users;
using consoles::Generation;
using libchiaking::AccountId;
using libchiaking::ChiakiNg;
using play::SessionState;
using play::StartRefusal;
using play::Where;
using test_support::play::RecordedEvents;
using test_support::play::RecordedMedia;
using test_support::play::SignedInPlayer;

class SkippingClock final : public platform::ClockProvider {
 public:
  explicit SkippingClock(const platform::ClockProvider& base) : base_(base) {}

  std::uint64_t now_microseconds() const noexcept override {
    return base_.now_microseconds() + skipped_.load(std::memory_order_acquire);
  }

  void skip(std::chrono::seconds ahead) noexcept {
    skipped_.fetch_add(static_cast<std::uint64_t>(ahead.count()) * 1'000'000U,
                       std::memory_order_acq_rel);
  }

 private:
  const platform::ClockProvider& base_;
  std::atomic<std::uint64_t> skipped_{};
};

class MemoryDevices final : public kakusan::DeviceStore {
 public:
  auto load_identity()
      -> libchiaking::Expected<std::optional<kakusan::SavedIdentity>,
                               Error> override {
    if (!identity_) return std::optional<kakusan::SavedIdentity>{};
    return std::optional<kakusan::SavedIdentity>{identity_->clone()};
  }
  auto save_identity(const kakusan::SavedIdentity& identity)
      -> libchiaking::Expected<void, Error> override {
    identity_.emplace(identity.clone());
    return {};
  }
  auto load_devices()
      -> libchiaking::Expected<std::vector<kakusan::SavedDevice>,
                               Error> override {
    return std::vector<kakusan::SavedDevice>{};
  }
  auto save(const kakusan::SavedDevice&)
      -> libchiaking::Expected<void, Error> override {
    return {};
  }
  auto remove(const kakusan::IdentityKey&)
      -> libchiaking::Expected<void, Error> override {
    return {};
  }

 private:
  std::optional<kakusan::SavedIdentity> identity_;
};

class Household {
 public:
  static constexpr std::string_view living_room = "aabbccddeeff";
  static constexpr std::string_view cloud_title =
      "EP0109-PPSA07033_00-FAKE0030TITLE000";
  static constexpr std::uint64_t home_account = 40;

  explicit Household(bool with_devices = true)
      : factory(
            platform::PlatformProviders{.clock = &clock,
                                        .network = &network,
                                        .crypto = &crypto,
                                        .http = &ps,
                                        .websocket = &websocket,
                                        .diagnostics = &log,
                                        .secure_datagram = &secure_datagram}),
        chiaki(opened(with_devices)) {}

  static auto keys() -> consoles::PairingKeys {
    std::array<std::byte, consoles::PairingKeys::registration_key_size>
        registration{};
    registration.fill(std::byte{'1'});
    std::array<std::byte, consoles::PairingKeys::remote_play_key_size>
        remote_play{};
    remote_play.fill(std::byte{0x5a});
    auto created = consoles::PairingKeys::create(registration, remote_play, 2);
    EXPECT_TRUE(created);
    return std::move(created.value());
  }

  static auto hardware(std::string_view id) -> consoles::HardwareId {
    const auto parsed = consoles::HardwareId::parse(id);
    EXPECT_TRUE(parsed);
    return parsed.value();
  }

  void save_pairing(AccountId account, std::string_view id,
                    std::string_view address = "127.0.0.1") {
    consoles::SavedConsole row{
        hardware(id), consoles::Firmware::newest(Generation::ps5),
        consoles::ConsoleName::of(Generation::ps5), std::nullopt};
    if (!address.empty()) {
      auto host = consoles::Host::create(address, Generation::ps5);
      ASSERT_TRUE(host);
      row.last_address = std::move(host.value());
    }
    const auto console_saved = console_store.save(row);
    std::array<std::byte, consoles::DeviceUid::size> uid{};
    uid.fill(std::byte{7});
    const auto pairing_saved = console_store.save(consoles::SavedPairing{
        account, hardware(id), keys(), consoles::DeviceUid::from_bytes(uid)});
    EXPECT_TRUE(console_saved);
    EXPECT_TRUE(pairing_saved);
  }

  void sighted(std::string_view id, bool in_rest_mode = false) {
    auto host = consoles::Host::create("127.0.0.1", Generation::ps5);
    ASSERT_TRUE(host);
    consoles::ConsoleFacts{chiaki.consoles()}.sighted(
        {hardware(id), consoles::Firmware::newest(Generation::ps5),
         std::nullopt, std::move(host.value()), in_rest_mode, std::nullopt});
  }

  void gone(consoles::Console& console) {
    consoles::ConsoleFacts{chiaki.consoles()}.out_of_sight(console, false);
  }

  void end(play::Session& session) {
    static_cast<void>(session.leave());
    const auto deadline = steady.now_microseconds() + 60'000'000;
    platform::Sleeper sleeper{threading, steady};
    while (session.state() != SessionState::ended) {
      ASSERT_LT(steady.now_microseconds(), deadline);
      clock.skip(std::chrono::seconds{1});
      sleeper.sleep_for(std::chrono::milliseconds{2});
    }
    static_cast<void>(session.wait_until_ended());
    EXPECT_EQ(chiaki.play().live(), nullptr);
  }

  platform::DefaultThreadingProvider& threading{
      test_support::process_threading()};
  platform::SteadyClock steady;
  SkippingClock clock{steady};
  fakes::InMemoryNetwork network{threading, steady};
  platform::openssl::OpenSslCryptoProvider crypto;
  test_support::play::ScriptedCloudPs ps;
  test_support::RefusingWebSocket websocket;
  test_support::FakeLog log;
  fakes::FakeSecureDatagramProvider secure_datagram;
  platform::DefaultPlatformFactory factory;
  fakes::InMemoryCredentialStore credentials{threading};
  fakes::InMemoryConsoleStore console_store{threading};
  fakes::InMemoryRecordStore records{threading};
  test_support::play::MemoryDatacenterStore datacenters;
  MemoryDevices devices;
  test_support::play::ScriptedClearance clearance{
      test_support::play::Title{.product = cloud_title}};
  test_support::FakeWaiter waiter;
  test_support::play::SwitchableClearance answering{clearance};
  platform::SystemUtcOffset time_zone;
  ChiakiNg chiaki;
  RecordedEvents events;
  RecordedMedia media;

 private:
  auto opened(bool with_devices) -> ChiakiNg {
    auto settings = play::Session::Core::core_settings();
    settings.commands.rest_flush = std::chrono::milliseconds{20};
    auto made = fakes::ServiceBuilder::open_starting_with<ChiakiNg>(
        factory,
        ChiakiNg::Stores{credentials, console_store, datacenters,
                         with_devices ? &devices : nullptr, records},
        play::StartCollaborators{answering, waiter, time_zone, settings});
    EXPECT_TRUE(made);
    return std::move(made.value());
  }
};

class ChiakiNgTest : public ::testing::Test {
 protected:
  static constexpr std::string_view living_room = Household::living_room;
  static constexpr std::string_view cloud_title = Household::cloud_title;
  static constexpr std::uint64_t home_account = Household::home_account;

  static auto refusal(
      const libchiaking::Expected<std::shared_ptr<play::Session>, StartRefusal>&
          started) -> std::optional<StartRefusal::Kind> {
    return started ? std::nullopt : std::optional{started.error().kind()};
  }

  static auto route(
      const libchiaking::Expected<consoles::Route, StartRefusal>& chosen)
      -> std::optional<consoles::Route> {
    return chosen ? std::optional{chosen.value()} : std::nullopt;
  }

  static auto seat(std::uint8_t index) -> libchiaking::controllers::SeatNumber {
    return libchiaking::controllers::SeatNumber::create(index).value();
  }

  static auto carried(std::initializer_list<platform::HttpCredential> kinds)
      -> platform::HttpCredentials {
    platform::HttpCredentials set;
    for (const auto kind : kinds) set = set.with(kind);
    return set;
  }
};

}  // namespace chiaki_ng_test
