#pragma once

#include "src/core/consoles/adapters/search/beacon.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/unit/core/consoles/adapters/search/scripted_network.hpp"
#include "tests/support/log.hpp"

#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/testing/fake_console.hpp>
#include <libchiaking/testing/in_memory_network.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace consoles_test {

class SearchSink final : public libchiaking::consoles::Beacon::Watcher {
 public:
  using Probe =
      std::pair<libchiaking::platform::SocketEndpoint,
                std::optional<libchiaking::consoles::Beacon::ProbeFailure>>;

  void on_reply(libchiaking::consoles::SearchReply reply) override {
    hosts.push_back(std::move(reply));
  }

  void on_probe(libchiaking::platform::SocketEndpoint destination,
                std::optional<libchiaking::consoles::Beacon::ProbeFailure>
                    failure) override {
    probes.emplace_back(destination, failure);
  }

  void on_datagram(bool accepted) override { datagrams.push_back(accepted); }

  std::vector<libchiaking::consoles::SearchReply> hosts;
  std::vector<Probe> probes;
  std::vector<bool> datagrams;
};

class SearchRig {
 public:
  static auto threading() -> libchiaking::platform::DefaultThreadingProvider& {
    return test_support::process_threading();
  }

  static auto clock() -> const libchiaking::platform::SteadyClock& {
    static const libchiaking::platform::SteadyClock steady;
    return steady;
  }

  static auto now() -> std::chrono::microseconds {
    return std::chrono::microseconds{clock().now_microseconds()};
  }

  static void pause(std::chrono::nanoseconds duration) {
    libchiaking::platform::Sleeper{threading(), clock()}.sleep_for(duration);
  }

  static auto ip(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d)
      -> libchiaking::platform::IpAddress {
    return libchiaking::platform::IpAddress::ipv4({a, b, c, d});
  }

  static auto on_loopback() -> libchiaking::consoles::Beacon::Settings {
    libchiaking::consoles::Beacon::Settings settings;
    settings.bind_address = libchiaking::platform::IpAddress::loopback_ipv4();
    settings.first_bind_port = 0;
    settings.last_bind_port = 0;
    return settings;
  }

  static auto loopback(const libchiaking::testing::FakeConsole& ps4,
                       const libchiaking::testing::FakeConsole& ps5)
      -> libchiaking::consoles::Beacon::Settings {
    auto settings = on_loopback();
    settings.targets = {libchiaking::platform::IpAddress::loopback_ipv4()};
    settings.include_interface_broadcasts = false;
    settings.ps4_destination_port = ps4.port();
    settings.ps5_destination_port = ps5.port();
    return settings;
  }

  static auto destinations(
      const std::vector<libchiaking::platform::SocketEndpoint>& sent)
      -> std::vector<libchiaking::platform::IpAddress> {
    std::vector<libchiaking::platform::IpAddress> addresses;
    for (const auto& endpoint : sent)
      if (std::ranges::find(addresses, endpoint.address) == addresses.end())
        addresses.push_back(endpoint.address);
    return addresses;
  }

  static auto contains(const std::string& datagram, std::string_view part)
      -> testing::AssertionResult {
    if (datagram.find(part) != std::string::npos)
      return testing::AssertionSuccess();
    return testing::AssertionFailure()
           << "\"" << datagram << "\" does not contain \"" << part << "\"";
  }

  static auto starts_with(const std::string& datagram, std::string_view part)
      -> testing::AssertionResult {
    if (datagram.find(part) == 0) return testing::AssertionSuccess();
    return testing::AssertionFailure()
           << "\"" << datagram << "\" does not start with \"" << part << "\"";
  }

  template <class Waiter>
  static auto wake_interrupts_wait(Waiter& waiter) -> testing::AssertionResult {
    auto waker = libchiaking::platform::Thread::start(
                     threading(), "test-waker",
                     libchiaking::platform::ThreadRole::unspecified,
                     [&waiter] {
                       pause(std::chrono::milliseconds{40});
                       waiter.wake();
                     })
                     .value();
    const auto started = now();
    waiter.wait(std::chrono::milliseconds{5000});
    const auto elapsed = now() - started;
    static_cast<void>(waker.join());
    return within_wake_window(elapsed);
  }

  static auto within_wake_window(std::chrono::microseconds elapsed)
      -> testing::AssertionResult {
    if (elapsed >= std::chrono::milliseconds{30} &&
        elapsed < std::chrono::seconds{2})
      return testing::AssertionSuccess();
    return testing::AssertionFailure() << "the wait lasted " << elapsed.count()
                                       << " us, outside [30 ms, 2 s)";
  }
};

}  // namespace consoles_test
