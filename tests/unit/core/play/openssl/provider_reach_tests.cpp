#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/testing/service_builder.hpp"
#include "src/core/play/play_core.hpp"
#include "tests/support/api/fakes.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/play/launch_rig.hpp"
#include "tests/support/play/scripted_cloud_ps.hpp"
#include "tests/support/play/session_rig.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/support/users/signed_in.hpp"
#include "tests/support/waiter.hpp"
#include "tests/unit/core/play/provider_watch.hpp"

#include <libchiaking/detail/catalog/catalog.hpp>
#include <libchiaking/detail/consoles/known_consoles.hpp>
#include <libchiaking/detail/play/play.hpp>
#include <libchiaking/platform/crypto/openssl/provider.hpp>
#include <libchiaking/platform/factory.hpp>
#include <libchiaking/platform/runtime/random_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/testing/fake_console.hpp>
#include <libchiaking/testing/in_memory_console_store.hpp>
#include <libchiaking/testing/in_memory_network.hpp>
#include <libchiaking/testing/in_memory_record_store.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace play_test {

using test_support::FakeWaiter;
using test_support::LiveProviders;
using test_support::play::LaunchCase;
using test_support::play::MemoryDatacenterStore;
using test_support::play::RecordedEvents;
using test_support::play::RecordedMedia;
using test_support::play::ScriptedClearance;
using test_support::play::ScriptedCloudPs;
using test_support::play::SignedInPlayer;
using test_support::play::Title;
namespace {

namespace consoles = libchiaking::consoles;
namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
using play::SessionState;
using Reach = ProviderWatch::Reach;
using FakeConsole = libchiaking::testing::FakeConsole;
using InMemoryNetwork = libchiaking::testing::InMemoryNetwork;

class CountedNetwork final : public platform::NetworkProvider {
 public:
  explicit CountedNetwork(platform::NetworkProvider& inner) noexcept
      : inner_(inner), sockets_(inner.sockets()) {}

  auto sockets() noexcept -> platform::SocketProvider& override {
    return sockets_;
  }
  auto pollers() noexcept -> platform::PollerProvider& override {
    return inner_.pollers();
  }
  auto dns() noexcept -> platform::DnsProvider& override {
    return inner_.dns();
  }
  auto interfaces() noexcept -> platform::InterfacesProvider& override {
    return inner_.interfaces();
  }
  auto capabilities() const noexcept -> platform::NetworkCapabilities override {
    return inner_.capabilities();
  }

  auto tcp_sockets() const noexcept -> std::size_t {
    return sockets_.tcp.load(std::memory_order_acquire);
  }

 private:
  class Sockets final : public platform::SocketProvider {
   public:
    explicit Sockets(platform::SocketProvider& inner) noexcept
        : inner_(inner) {}

    auto create_udp_socket(const platform::IpAddress& family_of)
        -> libchiaking::Expected<std::unique_ptr<platform::UdpSocket>,
                                 platform::NetError> override {
      return inner_.create_udp_socket(family_of);
    }
    auto create_tcp_socket(const platform::IpAddress& family_of)
        -> libchiaking::Expected<std::unique_ptr<platform::TcpSocket>,
                                 platform::NetError> override {
      tcp.fetch_add(1, std::memory_order_acq_rel);
      return inner_.create_tcp_socket(family_of);
    }
    auto create_tcp_listener(const platform::IpAddress& family_of)
        -> libchiaking::Expected<std::unique_ptr<platform::TcpListener>,
                                 platform::NetError> override {
      return inner_.create_tcp_listener(family_of);
    }

    std::atomic<std::size_t> tcp{};

   private:
    platform::SocketProvider& inner_;
  };

  platform::NetworkProvider& inner_;
  Sockets sockets_;
};

class WatchedHome {
 public:
  static constexpr std::string_view living_room = "AABBCCDDEEFF";
  static constexpr std::string_view wake_digits = "3a5f9c1e";
  static constexpr std::string_view cloud_title =
      "EP0109-PPSA07033_00-FAKE0030TITLE000";

  explicit WatchedHome(ProviderWatch& watch)
      : watch_(watch),
        network(watch, watch.steady()),
        counted(network),
        factory(platform::PlatformProviders{.threading = &watch,
                                            .clock = &watch.steady(),
                                            .wall_clock = &watch.wall(),
                                            .network = &counted,
                                            .crypto = &crypto,
                                            .http = &ps,
                                            .websocket = &websocket,
                                            .diagnostics = &log}),
        store(watch),
        records(watch),
        budget(watch, watch.steady()),
        catalog(libchiaking::testing::ServiceBuilder::build<
                libchiaking::catalog::Catalog>(
            offline, records, watch, watch.steady(), watch.wall(), budget)),
        players(signed_out, watch, watch.steady(), watch.wall()),
        pairer(factory, &log),
        known(libchiaking::testing::ServiceBuilder::build<
              consoles::KnownConsoles>(network, offline, crypto, random, store,
                                       watch, watch.steady(), &pairer, &log)),
        play(libchiaking::testing::ServiceBuilder::build<play::Play>(
            std::make_unique<play::Play::Core>(factory, known, catalog,
                                               datacenters, pairer, &log,
                                               starting_with()))) {}

  auto starting_with() -> play::StartCollaborators {
    auto settings = play::Session::Core::core_settings();
    settings.commands.rest_flush = std::chrono::milliseconds{20};
    return {clearance, waiter, time_zone, settings};
  }

  static auto keys() -> consoles::PairingKeys {
    std::array<std::byte, consoles::PairingKeys::registration_key_size>
        registration{};
    std::ranges::transform(wake_digits, registration.begin(), [](char digit) {
      return static_cast<std::byte>(digit);
    });
    std::array<std::byte, consoles::PairingKeys::remote_play_key_size>
        remote_play{};
    remote_play.fill(std::byte{0x5a});
    auto created = consoles::PairingKeys::create(registration, remote_play, 2);
    EXPECT_TRUE(created);
    return std::move(created).value();
  }

  auto paired(const libchiaking::users::PsUser& as, std::string_view address,
              std::optional<consoles::DeviceUid> device_uid = {})
      -> std::shared_ptr<consoles::Console> {
    const auto hardware_id = consoles::HardwareId::parse(living_room);
    EXPECT_TRUE(hardware_id);
    consoles::SavedConsole row{
        hardware_id.value(),
        consoles::Firmware::newest(consoles::Generation::ps5),
        consoles::ConsoleName::of(consoles::Generation::ps5), std::nullopt};
    if (!address.empty()) {
      auto host = consoles::Host::create(address, consoles::Generation::ps5);
      EXPECT_TRUE(host);
      row.last_address = std::move(host).value();
    }
    EXPECT_TRUE(store.save(row));
    EXPECT_TRUE(store.save(consoles::SavedPairing{
        as.account_id(), hardware_id.value(), keys(), device_uid}));
    EXPECT_TRUE(known.remember_saved());
    auto console = known.find(hardware_id.value());
    EXPECT_NE(console, nullptr);
    return console;
  }

  static auto device() -> consoles::DeviceUid {
    std::array<std::byte, consoles::DeviceUid::size> bytes{};
    bytes.fill(std::byte{7});
    const auto made = consoles::DeviceUid::from_bytes(bytes);
    EXPECT_TRUE(made);
    return made.value();
  }

  auto home_player() -> std::shared_ptr<libchiaking::users::PsUser> {
    const auto added = players.users.add_home_user(libchiaking::AccountId{40},
                                                   "Living Room Player");
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
      watch_.steady().skip(std::chrono::seconds{1});
      LiveProviders::nap(std::chrono::milliseconds{2});
    }
    return testing::AssertionSuccess();
  }

 private:
  ProviderWatch& watch_;

 public:
  InMemoryNetwork network;
  CountedNetwork counted;
  platform::openssl::OpenSslCryptoProvider crypto;
  platform::SystemSecureRandom random;
  ScriptedCloudPs ps;
  test_support::api::InterruptibleHttp offline;
  test_support::RefusingWebSocket websocket;
  test_support::FakeLog log;
  platform::DefaultPlatformFactory factory;
  libchiaking::testing::InMemoryConsoleStore store;
  libchiaking::testing::InMemoryRecordStore records;
  libchiaking::users::RequestBudget budget;
  libchiaking::catalog::Catalog catalog;
  MemoryDatacenterStore datacenters;
  ScriptedClearance clearance{Title{.product = cloud_title}};
  FakeWaiter waiter;
  platform::SystemUtcOffset time_zone;
  test_support::users::Offline signed_out;
  test_support::users::SignedIn players;
  play::InternetPairer pairer;
  consoles::KnownConsoles known;
  play::Play play;
  RecordedEvents events;
  RecordedMedia media;
};

class ProviderReach : public ::testing::Test {
 protected:
  void stayed_on_the_providers() const {
    EXPECT_GT(watch_.threads(), 0U);
    EXPECT_GT(watch_.reached(Reach::lock), 0U);
    EXPECT_GT(watch_.reached(Reach::clock), 0U);
    EXPECT_EQ(watch_.foreign(Reach::lock), 0U);
    EXPECT_EQ(watch_.foreign(Reach::wait), 0U);
    EXPECT_EQ(watch_.foreign(Reach::clock), 0U);
  }

  ProviderWatch watch_;
};

TEST_F(ProviderReach, ReportsAThreadTheWatchDidNotStart) {
  platform::Mutex mutex{watch_};
  platform::DefaultThreadingProvider elsewhere;
  const auto touch = [&] {
    platform::Lock held{mutex};
    static_cast<void>(watch_.steady().now_microseconds());
  };
  {
    auto stranger = platform::Thread::start(
        elsewhere, "stranger", platform::ThreadRole::unspecified, touch);
    ASSERT_TRUE(stranger);
    EXPECT_TRUE(stranger->join());
  }
  EXPECT_EQ(watch_.foreign(Reach::lock), 1U);
  EXPECT_EQ(watch_.foreign(Reach::clock), 1U);
  {
    auto own = platform::Thread::start(
        watch_, "own", platform::ThreadRole::unspecified, touch);
    ASSERT_TRUE(own);
    EXPECT_TRUE(own->join());
  }
  EXPECT_EQ(watch_.threads(), 1U);
  EXPECT_EQ(watch_.foreign(), 2U);
  touch();
  EXPECT_EQ(watch_.foreign(), 2U);
}

TEST_F(ProviderReach, KeepsAHomeStartOnTheProviders) {
  {
    WatchedHome home{watch_};
    FakeConsole console{home.network,
                        FakeConsole::Identity{.hardware_id = std::string{
                                                  WatchedHome::living_room}}};
    console.rest();
    const auto player = home.home_player();
    const auto paired = home.paired(*player, "127.0.0.1");
    const auto started =
        home.play.start_on_console(player, paired, {}, home.media, home.events);
    ASSERT_TRUE(started);
    const auto session = started.value();
    ASSERT_TRUE(home.skip_until(
        [&] { return session->state() == SessionState::ended; }));
    static_cast<void>(session->wait_until_ended());
    console.serve();
    EXPECT_FALSE(console.wakeups().empty());
    EXPECT_GT(home.counted.tcp_sockets(), 0U);
  }
  stayed_on_the_providers();
}

TEST_F(ProviderReach, WakesAndStreamsAHomeStartOnTheNetworkItWasGiven) {
  {
    InMemoryNetwork elsewhere{watch_, watch_.steady()};
    auto routed = std::make_shared<CountedNetwork>(elsewhere);
    {
      WatchedHome home{watch_};
      FakeConsole console{
          elsewhere, FakeConsole::Identity{
                         .hardware_id = std::string{WatchedHome::living_room}}};
      console.rest();
      const auto player = home.home_player();
      const auto paired = home.paired(*player, "127.0.0.1");
      play::Preferences preferences;
      preferences.home = play::HomeOptions{routed};
      const auto started = home.play.start_on_console(
          player, paired, preferences, home.media, home.events);
      ASSERT_TRUE(started);
      EXPECT_GT(routed.use_count(), 1);
      const auto session = started.value();
      ASSERT_TRUE(home.skip_until(
          [&] { return session->state() == SessionState::ended; }));
      static_cast<void>(session->wait_until_ended());
      console.serve();
      EXPECT_FALSE(console.wakeups().empty());
      EXPECT_GT(routed->tcp_sockets(), 0U);
      EXPECT_FALSE(elsewhere.sent().empty());
      EXPECT_EQ(home.counted.tcp_sockets(), 0U);
      EXPECT_TRUE(home.network.sent().empty());
      ASSERT_TRUE(paired->last_address());
      EXPECT_EQ(paired->last_address()->text(), "127.0.0.1");
    }
    EXPECT_EQ(routed.use_count(), 1);
  }
  stayed_on_the_providers();
}

TEST_F(ProviderReach, KeepsAnInternetStartOnTheProviders) {
  {
    WatchedHome home{watch_};
    const auto player =
        home.players.as(SignedInPlayer::saved_for_remote_play());
    const auto paired = home.paired(*player, {}, WatchedHome::device());
    home.offline.answer =
        ScriptedCloudPs::account_listing("07", R"(["remotePlay"])");
    const auto started = home.play.start_over_internet(player, paired, {},
                                                       home.media, home.events);
    ASSERT_TRUE(started);
    const auto session = started.value();
    ASSERT_TRUE(home.skip_until(
        [&] { return session->state() == SessionState::ended; }));
    static_cast<void>(session->wait_until_ended());
    EXPECT_GT(home.ps.requests, 0U);
  }
  stayed_on_the_providers();
}

TEST_F(ProviderReach, KeepsACloudStartOnTheProviders) {
  {
    WatchedHome home{watch_};
    const auto player = home.players.as(SignedInPlayer::saved());
    const auto started = home.play.start_on_cloud(
        player, Title::product_id(WatchedHome::cloud_title), {},
        LaunchCase::named("dc1"), home.media, home.events);
    ASSERT_TRUE(started);
    const auto session = started.value();
    home.events.until("datacenter_chosen");
    static_cast<void>(session->leave());
    static_cast<void>(session->wait_until_ended());
    EXPECT_EQ(home.ps.starts, 1U);
  }
  stayed_on_the_providers();
}

}  // namespace
}  // namespace play_test
