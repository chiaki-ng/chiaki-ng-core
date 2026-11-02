#pragma once

#include "src/core/net/kakusan/protocol_error.hpp"
#include "libchiaking/platform/dtls/openssl/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/kakusan/datagram_security.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "src/core/net/kakusan/udp_transport_factory.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/secure_datagrams.hpp"
#include "tests/support/process_threading.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace net_kakusan_test {

class DatagramSecurityRig : public ::testing::Test {
 public:
  using Datagram = std::vector<std::byte>;
  using Batch = std::vector<Datagram>;
  using Security = std::unique_ptr<libchiaking::net::kakusan::DatagramSecurity>;
  using Filter = std::function<Batch(Batch)>;
  using Error = libchiaking::net::kakusan::Error;
  using Config = libchiaking::net::kakusan::DatagramSecurityConfig;
  using Role = libchiaking::net::kakusan::SecurityRole;
  using State = libchiaking::net::kakusan::SecurityState;
  using Digest = libchiaking::net::kakusan::Digest256;
  using Provider =
      libchiaking::platform::openssl::OpenSslSecureDatagramProvider;

  static constexpr std::uint64_t ms = 1000;
  static constexpr std::uint64_t sec = 1'000'000;

  class Pair {
   public:
    Security client;
    Security server;
  };

  class Outcome {
   public:
    std::optional<Error> client;
    std::optional<Error> server;
  };

  static auto provider() -> Provider& {
    return test_support::kakusan::SecureDatagrams<Provider>::provider();
  }

  static auto steady() -> const libchiaking::platform::SteadyClock& {
    static const libchiaking::platform::SteadyClock clock;
    return clock;
  }

  static auto real_now() -> std::uint64_t {
    return steady().now_microseconds();
  }

  static auto fails_with(const libchiaking::net::kakusan::Result<void>& result,
                         Error error) -> ::testing::AssertionResult {
    if (result) return ::testing::AssertionFailure() << "it succeeded";
    if (result.error() != error)
      return ::testing::AssertionFailure()
             << "it failed with " << ::testing::PrintToString(result.error());
    return ::testing::AssertionSuccess();
  }

  static auto pattern(std::size_t size, std::uint8_t seed) -> Datagram {
    Datagram bytes(size);
    for (std::size_t index = 0; index < size; ++index)
      bytes[index] = static_cast<std::byte>((index * 31U + seed) & 0xffU);
    return bytes;
  }

  static auto flipped(Digest digest) -> Digest {
    digest[7] ^= std::byte{0x01};
    return digest;
  }

  static auto make(Role role, Config config = {}) -> Security {
    auto created = libchiaking::net::kakusan::DatagramSecurity::open(
        provider(), role, std::move(config));
    EXPECT_TRUE(created);
    if (!created) return nullptr;
    return std::move(created).value();
  }

  static auto drain(libchiaking::net::kakusan::DatagramSecurity& security)
      -> Batch {
    Batch batch;
    while (auto datagram = security.take_datagram()) {
      EXPECT_FALSE(datagram->empty());
      EXPECT_LE(datagram->size(), libchiaking::net::kakusan::max_datagram_size);
      batch.push_back(std::move(*datagram));
    }
    return batch;
  }

  static void deliver(libchiaking::net::kakusan::DatagramSecurity& security,
                      const Batch& batch, std::uint64_t now) {
    for (const auto& datagram : batch)
      EXPECT_TRUE(security.receive(datagram, now));
  }

  static auto make_pair(Config client_config = {}, Config server_config = {})
      -> Pair {
    return Pair{make(Role::client, std::move(client_config)),
                make(Role::server, std::move(server_config))};
  }

  static void start(Pair& pair, std::uint64_t now,
                    std::optional<Digest> client_pin = std::nullopt,
                    std::optional<Digest> server_pin = std::nullopt) {
    EXPECT_TRUE(pair.server->start(
        server_pin.value_or(pair.client->certificate_digest()), now));
    EXPECT_TRUE(pair.client->start(
        client_pin.value_or(pair.server->certificate_digest()), now));
  }

  static auto exchange(Pair& pair, std::uint64_t now, const Filter& filter = {})
      -> Outcome {
    Outcome outcome;
    for (int round = 0; round < 64; ++round) {
      auto from_client = drain(*pair.client);
      auto from_server = drain(*pair.server);
      if (from_client.empty() && from_server.empty()) return outcome;
      if (filter) {
        from_client = filter(std::move(from_client));
        from_server = filter(std::move(from_server));
      }
      for (const auto& datagram : from_client) {
        const auto received = pair.server->receive(datagram, now);
        if (!received && !outcome.server) outcome.server = received.error();
      }
      for (const auto& datagram : from_server) {
        const auto received = pair.client->receive(datagram, now);
        if (!received && !outcome.client) outcome.client = received.error();
      }
    }
    ADD_FAILURE() << "the exchange did not settle within 64 rounds";
    return outcome;
  }

  static auto established_pair(Config client_config = {},
                               Config server_config = {}) -> Pair {
    auto pair = make_pair(std::move(client_config), std::move(server_config));
    const auto now = real_now();
    start(pair, now);
    const auto outcome = exchange(pair, now);
    EXPECT_FALSE(outcome.client);
    EXPECT_FALSE(outcome.server);
    EXPECT_TRUE(pair.client->established());
    EXPECT_TRUE(pair.server->established());
    return pair;
  }

  static void expect_record(
      libchiaking::net::kakusan::DatagramSecurity& security,
      const Datagram& expected) {
    const auto record = security.take_record();
    ASSERT_TRUE(record);
    EXPECT_TRUE(std::ranges::equal(record->span(), expected));
  }

  static void transfer(libchiaking::net::kakusan::DatagramSecurity& from,
                       libchiaking::net::kakusan::DatagramSecurity& to,
                       const Datagram& record) {
    EXPECT_TRUE(from.send(record));
    const auto batch = drain(from);
    ASSERT_EQ(batch.size(), 1U);
    EXPECT_TRUE(to.receive(batch[0], real_now()));
    const auto received = to.take_record();
    ASSERT_TRUE(received);
    EXPECT_TRUE(std::ranges::equal(received->span(), record));
    EXPECT_FALSE(to.take_record());
  }

  static void expect_failed(
      libchiaking::net::kakusan::DatagramSecurity& security,
      Error error = Error::protocol) {
    EXPECT_EQ(security.state(), State::failed);
    EXPECT_FALSE(security.established());
    EXPECT_EQ(security.max_record_size(), 0U);
    EXPECT_FALSE(security.take_record());
    EXPECT_FALSE(security.next_deadline_us());
    EXPECT_TRUE(fails_with(security.send(pattern(8, 1)), error));
    std::array<std::byte, 32> secret{};
    EXPECT_TRUE(fails_with(
        security.export_secret("EXPORTER-RCP-1-pairing", {}, secret), error));
  }

  static auto wait_for_retransmission(Pair& pair, std::uint64_t since)
      -> Outcome {
    while (real_now() - since < 2 * sec) {
      const auto now = real_now();
      EXPECT_TRUE(pair.client->poll(now));
      EXPECT_TRUE(pair.server->poll(now));
      auto from_client = drain(*pair.client);
      auto from_server = drain(*pair.server);
      if (!from_client.empty() || !from_server.empty()) {
        EXPECT_GE(now - since, 350 * ms);
        deliver(*pair.server, from_client, now);
        deliver(*pair.client, from_server, now);
        return exchange(pair, real_now());
      }
      EXPECT_TRUE(pair.client->next_deadline_us());
      libchiaking::platform::Sleeper{test_support::process_threading(),
                                     steady()}
          .sleep_for(std::chrono::milliseconds(5));
    }
    ADD_FAILURE() << "nothing was retransmitted within two seconds";
    return {};
  }
};

}  // namespace net_kakusan_test
