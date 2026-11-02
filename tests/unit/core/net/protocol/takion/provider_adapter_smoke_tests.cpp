#include <libchiaking/platform/platform.hpp>
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/protocol/takion/takion.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace takion_test {
namespace {

namespace platform = libchiaking::platform;
namespace takion = libchiaking::net::protocol::takion;
using libchiaking::net::protocol::takion::Expected;
using libchiaking::net::transport::datagram::Datagram;
using libchiaking::net::transport::datagram::TransportError;

class SmokeRecordingTransport final
    : public libchiaking::net::transport::datagram::DatagramTransport {
 public:
  auto send(std::span<const std::byte> datagram)
      -> Expected<std::size_t, TransportError> override {
    sent_bytes += datagram.size();
    ++sent_datagrams;
    return Expected<std::size_t, TransportError>{datagram.size()};
  }

  auto receive() -> Expected<std::optional<Datagram>, TransportError> override {
    return Expected<std::optional<Datagram>, TransportError>{std::nullopt};
  }

  auto close() -> Expected<void, TransportError> override {
    closed = true;
    return Expected<void, TransportError>{};
  }

  std::size_t sent_bytes{};
  std::size_t sent_datagrams{};
  bool closed{};
};

class SmokeNullSink final : public takion::EventSink {
 public:
  void on_event(takion::Event) override {}
};

class PlatformProviderSmoke : public ::testing::Test {
 protected:
  platform::SteadyClock platform_clock;
  platform::DefaultThreadingProvider threading;
  platform::SystemSecureRandom platform_random;
};

TEST_F(PlatformProviderSmoke, SteadyClockNeverRunsBackwards) {
  const auto first = platform_clock.now_microseconds();
  const auto second = platform_clock.now_microseconds();
  EXPECT_GE(second, first);
}

TEST_F(PlatformProviderSmoke, SystemRandomFillsBuffersOfAnySize) {
  EXPECT_TRUE(platform_random.fill_bytes({}));

  std::array<std::byte, 16> small{};
  EXPECT_TRUE(platform_random.fill_bytes(small));

  std::array<std::byte, 1024> large{};
  std::array<std::byte, 1024> second_large{};
  ASSERT_TRUE(platform_random.fill_bytes(large));
  ASSERT_TRUE(platform_random.fill_bytes(second_large));

  std::size_t differing{};
  for (std::size_t i = 0; i < large.size(); ++i) {
    if (large[i] != second_large[i]) ++differing;
  }
  EXPECT_GE(differing, large.size() / 2);

  std::array<std::byte, 8> over_chunk{};
  EXPECT_TRUE(platform_random.fill_bytes(over_chunk));

  std::vector<std::byte> big(4096);
  EXPECT_TRUE(platform_random.fill_bytes(big));
}

TEST_F(PlatformProviderSmoke, DriveATakionClientThroughStartAndStop) {
  platform::ClockProvider& clock{platform_clock};
  platform::SecureRandomProvider& random{platform_random};

  auto transport = std::make_unique<SmokeRecordingTransport>();
  SmokeRecordingTransport& transport_ref = *transport;
  SmokeNullSink sink;
  takion::ClientConfig config;
  takion::Client client(std::move(transport), clock, threading, random, sink,
                        std::move(config));

  const auto start_result = client.start();
  if (start_result.has_value())
    EXPECT_NE(transport_ref.sent_datagrams, 0U);
  else
    EXPECT_NE(start_result.error(), takion::ClientError::random_failure);
  const auto snapshot = client.snapshot();
  EXPECT_TRUE(snapshot.state == takion::ClientState::handshaking ||
              snapshot.state == takion::ClientState::connected)
      << "state " << static_cast<int>(snapshot.state);

  EXPECT_TRUE(client.request_stop());
}

}  // namespace
}  // namespace takion_test
