#pragma once

#include "libchiaking/testing/fake_secure_datagram.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fake_secure_datagram_test {

class Association {
 public:
  using Session = libchiaking::platform::SecureDatagramSession;
  using Datagrams = std::vector<std::vector<std::byte>>;

  explicit Association(std::string server_alpn = "test/1",
                       std::uint64_t record_limit = 16)
      : client(open(libchiaking::platform::SecureDatagramRole::client, "test/1",
                    record_limit)),
        server(open(libchiaking::platform::SecureDatagramRole::server,
                    std::move(server_alpn), record_limit)) {}

  static auto profile(libchiaking::platform::SecureDatagramRole role,
                      std::string alpn, std::uint64_t record_limit)
      -> libchiaking::platform::SecureDatagramProfile {
    using namespace std::chrono_literals;
    libchiaking::platform::SecureDatagramProfile chosen;
    chosen.role = role;
    chosen.alpn = std::move(alpn);
    chosen.mtu = 1200;
    chosen.max_certificate_size = 4096;
    chosen.handshake_deadline = 15s;
    chosen.retransmit_first = 400ms;
    chosen.retransmit_max = 60s;
    chosen.record_limit = record_limit;
    return chosen;
  }

  static auto bytes(std::size_t size, std::uint8_t seed)
      -> std::vector<std::byte> {
    std::vector<std::byte> pattern(size);
    for (std::size_t index = 0; index < size; ++index)
      pattern[index] = static_cast<std::byte>((index * 13U + seed) & 0xffU);
    return pattern;
  }

  static auto drain(Session& session) -> Datagrams {
    Datagrams datagrams;
    while (auto datagram = session.take_datagram())
      datagrams.push_back(std::move(*datagram));
    return datagrams;
  }

  static auto deliver(Session& session, const Datagrams& datagrams,
                      Session::Instant now)
      -> std::optional<libchiaking::platform::SecureDatagramError> {
    std::optional<libchiaking::platform::SecureDatagramError> failure;
    for (const auto& datagram : datagrams) {
      const auto received = session.receive(datagram, now);
      if (!received && !failure) failure = received.error();
    }
    return failure;
  }

  auto opened() const -> ::testing::AssertionResult {
    if (client && server) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << "a session did not open";
  }

  auto start(Session::Instant now) -> ::testing::AssertionResult {
    if (!server->start(client->certificate_digest(), now))
      return ::testing::AssertionFailure() << "the server did not start";
    if (!client->start(server->certificate_digest(), now))
      return ::testing::AssertionFailure() << "the client did not start";
    return ::testing::AssertionSuccess();
  }

  auto settle(Session::Instant now) -> ::testing::AssertionResult {
    for (int round = 0; round < 16; ++round) {
      const auto from_client = drain(*client);
      const auto from_server = drain(*server);
      if (from_client.empty() && from_server.empty())
        return ::testing::AssertionSuccess();
      client_failure =
          client_failure ? client_failure : deliver(*client, from_server, now);
      server_failure =
          server_failure ? server_failure : deliver(*server, from_client, now);
    }
    return ::testing::AssertionFailure()
           << "the sessions were still talking after sixteen rounds";
  }

  auto establish(Session::Instant now) -> ::testing::AssertionResult {
    if (auto started = start(now); !started) return started;
    if (auto settled = settle(now); !settled) return settled;
    if (client->state() !=
        libchiaking::platform::SecureDatagramState::established)
      return ::testing::AssertionFailure() << "the client is not established";
    if (server->state() !=
        libchiaking::platform::SecureDatagramState::established)
      return ::testing::AssertionFailure() << "the server is not established";
    return ::testing::AssertionSuccess();
  }

  libchiaking::testing::FakeSecureDatagramProvider provider;
  std::unique_ptr<Session> client;
  std::unique_ptr<Session> server;
  std::optional<libchiaking::platform::SecureDatagramError> client_failure;
  std::optional<libchiaking::platform::SecureDatagramError> server_failure;

 private:
  auto open(libchiaking::platform::SecureDatagramRole role, std::string alpn,
            std::uint64_t record_limit) -> std::unique_ptr<Session> {
    auto opened = provider.open(profile(role, std::move(alpn), record_limit));
    if (!opened) return nullptr;
    return std::move(opened).value();
  }
};

}  // namespace fake_secure_datagram_test
