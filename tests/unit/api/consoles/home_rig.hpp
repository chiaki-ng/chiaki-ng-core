#pragma once

#include "tests/support/api/fakes.hpp"
#include "tests/support/consoles/paired_consoles.hpp"
#include "tests/support/crypto/refusing.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include "libchiaking/consoles.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "libchiaking/testing/fake_console.hpp"
#include "libchiaking/testing/in_memory_console_store.hpp"
#include "libchiaking/testing/in_memory_credential_store.hpp"
#include "libchiaking/testing/in_memory_network.hpp"
#include "libchiaking/testing/service_builder.hpp"
#include "libchiaking/users.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace api_consoles_test {

class ScriptedClock final : public libchiaking::platform::ClockProvider {
 public:
  std::uint64_t now_microseconds() const noexcept override {
    if (on_read) on_read();
    return now.load();
  }

  mutable std::atomic<std::uint64_t> now{0};
  std::function<void()> on_read;
};

class Home {
 public:
  Home()
      : signed_in(libchiaking::testing::ServiceBuilder::build<
                  libchiaking::users::SignedInUsers>(
            http, credentials, threading, clock, wall, random)),
        known(libchiaking::testing::ServiceBuilder::build<
              libchiaking::consoles::KnownConsoles>(
            network, http, crypto, random, store, threading, clock)) {}

  static auto hardware_id(std::string_view text)
      -> libchiaking::consoles::HardwareId {
    return libchiaking::consoles::HardwareId::parse(text).value();
  }

  static auto keys(std::string_view registration)
      -> libchiaking::consoles::PairingKeys {
    std::array<std::byte,
               libchiaking::consoles::PairingKeys::registration_key_size>
        registration_key{};
    for (std::size_t index = 0; index < registration.size(); ++index)
      registration_key[index] = static_cast<std::byte>(registration[index]);
    std::array<std::byte,
               libchiaking::consoles::PairingKeys::remote_play_key_size>
        remote_play_key{};
    remote_play_key.fill(std::byte{0x5a});
    return libchiaking::consoles::PairingKeys::create(registration_key,
                                                      remote_play_key, 2)
        .value();
  }

  auto saved(std::string_view id, libchiaking::consoles::Generation generation,
             std::string_view address = {})
      -> std::shared_ptr<libchiaking::consoles::Console> {
    libchiaking::consoles::SavedConsole row{
        hardware_id(id), libchiaking::consoles::Firmware::newest(generation),
        libchiaking::consoles::ConsoleName::of(generation), std::nullopt};
    if (!address.empty())
      row.last_address =
          libchiaking::consoles::Host::create(address, generation).value();
    EXPECT_TRUE(store.save(row));
    EXPECT_TRUE(known.remember_saved());
    auto console = known.find(hardware_id(id));
    if (!console) throw std::logic_error{"the saved console was not known"};
    return console;
  }

  auto paired(std::string_view id, libchiaking::consoles::Generation generation,
              std::string_view address, libchiaking::AccountId account,
              std::string_view registration_key)
      -> std::shared_ptr<libchiaking::consoles::Console> {
    EXPECT_TRUE(store.save(libchiaking::consoles::SavedPairing{
        account, hardware_id(id), keys(registration_key), std::nullopt}));
    return saved(id, generation, address);
  }

  auto home_user(std::uint64_t account, std::string_view name)
      -> std::shared_ptr<libchiaking::users::PsUser> {
    return signed_in
        .add_home_user(libchiaking::AccountId{account}, std::string{name})
        .value();
  }

  libchiaking::platform::DefaultThreadingProvider& threading{
      test_support::process_threading()};
  libchiaking::platform::SteadyClock steady;
  libchiaking::testing::InMemoryNetwork network{threading, steady};
  test_support::api::OfflineHttp http;
  test_support::NoCrypto crypto;
  libchiaking::platform::SystemSecureRandom random;
  libchiaking::testing::InMemoryCredentialStore credentials{threading};
  libchiaking::testing::InMemoryConsoleStore store{threading};
  ScriptedClock clock;
  ScriptedClock wall;
  libchiaking::users::SignedInUsers signed_in;
  libchiaking::consoles::KnownConsoles known;
};

class Tunnel final : public libchiaking::platform::NetworkProvider {
 public:
  explicit Tunnel(bool broadcasts) noexcept : broadcasts_(broadcasts) {}

  auto sockets() noexcept -> libchiaking::platform::SocketProvider& override {
    return memory.sockets();
  }
  auto pollers() noexcept -> libchiaking::platform::PollerProvider& override {
    return memory.pollers();
  }
  auto dns() noexcept -> libchiaking::platform::DnsProvider& override {
    return memory.dns();
  }
  auto interfaces() noexcept
      -> libchiaking::platform::InterfacesProvider& override {
    return memory.interfaces();
  }
  auto capabilities() const noexcept
      -> libchiaking::platform::NetworkCapabilities override {
    auto capabilities = memory.capabilities();
    capabilities.broadcast = broadcasts_;
    return capabilities;
  }

  auto destinations() const -> std::vector<libchiaking::platform::IpAddress> {
    std::vector<libchiaking::platform::IpAddress> addresses;
    for (const auto& endpoint : memory.sent())
      if (std::ranges::find(addresses, endpoint.address) == addresses.end())
        addresses.push_back(endpoint.address);
    return addresses;
  }

 private:
  libchiaking::platform::SteadyClock steady_;
  bool broadcasts_;

 public:
  libchiaking::testing::InMemoryNetwork memory{
      test_support::process_threading(), steady_};
};

class Views final : public libchiaking::consoles::NetworkWatcher {
 public:
  void on_view(const libchiaking::consoles::NetworkView& view) override {
    seen.push_back(view);
    if (on_seen) on_seen(view);
  }

  std::vector<libchiaking::consoles::NetworkView> seen;
  std::function<void(const libchiaking::consoles::NetworkView&)> on_seen;
};

class HomeConsolesTest : public ::testing::Test {
 protected:
  static constexpr std::string_view living_room = "AABBCCDDEEFF";
  static constexpr std::string_view den = "001122334455";
  static constexpr std::uint64_t living_room_credential = 0x3a5f9c1e;

  static constexpr std::string_view ps5_reply =
      "HTTP/1.1 620 Server Standby\nhost-id:AABBCCDDEEFF\nhost-type:PS5\n"
      "host-name:Loopback fixture\nhost-request-port:997\n"
      "system-version:08050001\ndevice-discovery-protocol-version:00030010\n";
  static constexpr std::string_view ps5_renamed =
      "HTTP/1.1 200 Ok\nhost-id:AABBCCDDEEFF\nhost-type:PS5\n"
      "host-name:Renamed\nhost-request-port:997\n"
      "system-version:08050001\ndevice-discovery-protocol-version:00030010\n";
  static constexpr std::string_view ps4_reply =
      "HTTP/1.1 200 Ok\nhost-id:001122334455\nhost-type:PS4\n"
      "host-name:PS4 fixture\n";
  static constexpr std::string_view anonymous_reply =
      "HTTP/1.1 200 Ok\nhost-id:not-a-mac-id\nhost-type:PS4\n";

  static auto loopback_at(std::uint16_t port)
      -> libchiaking::platform::SocketEndpoint {
    return {libchiaking::platform::IpAddress::loopback_ipv4(), port};
  }

  static auto hardware_id(std::string_view text)
      -> libchiaking::consoles::HardwareId {
    return Home::hardware_id(text);
  }

  static auto ranges(const std::vector<std::string>& subnets, bool unicast)
      -> libchiaking::consoles::SearchRanges {
    return libchiaking::consoles::SearchRanges::parse(subnets, unicast).value();
  }

  static void wait_for(const std::atomic<bool>& flag) {
    libchiaking::platform::SteadyClock steady;
    libchiaking::platform::Sleeper sleeper{test_support::process_threading(),
                                           steady};
    while (!flag.load()) sleeper.sleep_for(std::chrono::milliseconds{1});
  }

  static auto sent_to(const libchiaking::testing::InMemoryNetwork& network,
                      libchiaking::platform::IpAddress address) -> bool {
    return std::ranges::any_of(
        network.sent(),
        [&](const libchiaking::platform::SocketEndpoint& endpoint) {
          return endpoint.address == address;
        });
  }

  static auto sent_to(const libchiaking::testing::InMemoryNetwork& network,
                      libchiaking::platform::IpAddress address,
                      std::uint16_t port) -> bool {
    return std::ranges::any_of(
        network.sent(),
        [&](const libchiaking::platform::SocketEndpoint& endpoint) {
          return endpoint.address == address && endpoint.port == port;
        });
  }

  Home home;
};

}  // namespace api_consoles_test
