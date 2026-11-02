#include "libchiaking/platform/net/default.hpp"
#include "src/core/net/protocol/takion/detail/envelope_codec.hpp"
#include "src/core/net/transport/datagram/udp_transport.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/cloud/links/ping_rig.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace cloud_test {
namespace {

namespace gk = libchiaking::psn::gk;
namespace play = libchiaking::play;
namespace platform = libchiaking::platform;
namespace datagram = libchiaking::net::transport::datagram;
namespace takion = libchiaking::net::protocol::takion;
using Error = play::DatacenterSurvey::Error;

class SurveyOverUdp : public ::testing::Test {
 protected:
  void SetUp() override {
    datagram::UdpTransportConfig peer_config;
    peer_config.local = {platform::IpAddress::loopback_ipv4(), 0};
    auto created = datagram::ProviderUdpTransport::create(network, peer_config);
    ASSERT_TRUE(created);
    peer = std::move(created.value());
    const auto endpoint = peer->local_endpoint();
    ASSERT_TRUE(endpoint);
    peer_port = endpoint.value().port;
  }

  auto start(std::vector<gk::Datacenter> datacenters,
             play::PingService service = play::PingService::cloud_stream,
             const libchiaking::SecretString& key = PingRig::test_key())
      -> libchiaking::Expected<play::DatacenterSurvey, Error> {
    return play::DatacenterSurvey::start(providers, std::move(datacenters),
                                         service, key, running);
  }

  auto first_datagram() -> std::optional<datagram::OwnedDatagram> {
    const auto ready = peer->wait_readable(std::chrono::seconds{1}, running);
    EXPECT_TRUE(ready);
    if (!ready) return std::nullopt;
    EXPECT_TRUE(ready.value());
    const auto received = peer->receive();
    EXPECT_TRUE(received);
    if (!received || !received.value()) return std::nullopt;
    EXPECT_FALSE(received.value()->empty());
    return datagram::OwnedDatagram(received.value()->begin(),
                                   received.value()->end());
  }

  auto survey_until_sent(play::DatacenterSurvey& survey)
      -> std::optional<datagram::OwnedDatagram> {
    const auto limit = PingRig::deadline_in(std::chrono::seconds{2});
    while (PingRig::before(limit)) {
      EXPECT_FALSE(survey.step());
      const auto ready =
          peer->wait_readable(std::chrono::milliseconds{5}, running);
      EXPECT_TRUE(ready);
      if (!ready || ready.value()) break;
    }
    return first_datagram();
  }

  platform::SteadyClock steady;
  platform::DefaultNetworkProvider network{test_support::process_threading(),
                                           steady};
  test_support::FakeClock clock{1'000};
  test_support::FixedRandom random;
  const play::DatacenterSurvey::Providers providers{
      network, clock, test_support::process_threading(), random};
  const platform::StopToken running;
  std::unique_ptr<datagram::ProviderUdpTransport> peer;
  std::uint16_t peer_port{};
};

TEST_F(SurveyOverUdp, PingsCloudStreamPlainUntilCancelled) {
  auto made = start({{"dc", "127.0.0.1", peer_port, 20'000}});
  ASSERT_TRUE(made);
  const auto received = survey_until_sent(made.value());
  ASSERT_TRUE(received);
  EXPECT_TRUE(
      takion::EnvelopeCodec::decode(*received, takion::PlainEnvelope{}));
  const auto cancelled = made.value().cancel();
  ASSERT_TRUE(cancelled);
  ASSERT_EQ(cancelled->size(), std::size_t{1});
  EXPECT_FALSE((*cancelled)[0].measured);
  EXPECT_FALSE(made.value().step());
}

TEST_F(SurveyOverUdp, WrapsTheLegacyPingInTypeOne) {
  auto legacy = start({{"dc", "127.0.0.1", peer_port, 20'000}},
                      play::PingService::legacy_cloud_stream);
  ASSERT_TRUE(legacy);
  const auto wrapped = survey_until_sent(legacy.value());
  ASSERT_TRUE(wrapped);
  const auto unwrapped =
      takion::EnvelopeCodec::decode(*wrapped, takion::PsnEnvelope{1});
  ASSERT_TRUE(unwrapped);
  EXPECT_FALSE(unwrapped.value().empty());
  EXPECT_TRUE(legacy.value().cancel());
}

TEST_F(SurveyOverUdp, RefusesAnEmptyKey) {
  const libchiaking::SecretString empty;
  const auto empty_key = start({{"dc", "127.0.0.1", peer_port, 20'000}},
                               play::PingService::cloud_stream, empty);
  ASSERT_FALSE(empty_key);
  EXPECT_EQ(empty_key.error(), Error::invalid_key);
}

TEST_F(SurveyOverUdp, FinishesAnUnresolvableDatacenterAtOnce) {
  auto invalid_endpoint = start({{"dc", "", peer_port, 20'000}});
  ASSERT_TRUE(invalid_endpoint);
  const auto unresolved = invalid_endpoint.value().step();
  ASSERT_TRUE(unresolved);
  EXPECT_FALSE(unresolved->timed_out);
  ASSERT_EQ(unresolved->rows.size(), std::size_t{1});
  EXPECT_FALSE(unresolved->rows[0].measured);
}

TEST_F(SurveyOverUdp, WaitsForAWorkingDatacenterBesideAFailedOne) {
  auto batch = start(
      {{"working", "127.0.0.1", peer_port, 20'000}, {"failed", "", 0, 20'000}});
  ASSERT_TRUE(batch);
  EXPECT_FALSE(batch.value().step());
  const auto final = batch.value().cancel();
  ASSERT_TRUE(final);
  ASSERT_EQ(final->size(), std::size_t{2});
  EXPECT_EQ((*final)[0].name, "working");
  EXPECT_EQ((*final)[1].name, "failed");
  EXPECT_FALSE((*final)[0].measured);
  EXPECT_FALSE((*final)[1].measured);
  EXPECT_EQ((*final)[0].rtt_ms, 999);
  EXPECT_EQ((*final)[1].rtt_ms, 999);
  EXPECT_FALSE(batch.value().step());
}

TEST_F(SurveyOverUdp, FinishesAFailedDatacenterAlone) {
  auto failed_batch = start({{"unresolved", "", 0, 20'000}});
  ASSERT_TRUE(failed_batch);
  const auto failed_final = failed_batch.value().step();
  ASSERT_TRUE(failed_final);
  EXPECT_FALSE(failed_final->timed_out);
  ASSERT_EQ(failed_final->rows.size(), std::size_t{1});
  EXPECT_EQ(failed_final->rows[0].name, "unresolved");
  EXPECT_FALSE(failed_final->rows[0].measured);
  EXPECT_EQ(failed_final->rows[0].rtt_ms, 999);
}

TEST_F(SurveyOverUdp, RefusesNoKeyForTheBatch) {
  const libchiaking::SecretString no_key;
  const auto invalid = start(
      {{"working", "127.0.0.1", peer_port, 20'000}, {"failed", "", 0, 20'000}},
      play::PingService::cloud_stream, no_key);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), Error::invalid_key);
}

class SlowDns final : public platform::DnsProvider {
 public:
  auto resolve(std::string_view, std::string_view)
      -> libchiaking::Expected<std::vector<platform::IpAddress>,
                               platform::NetError> override {
    PingRig::pause_for(std::chrono::seconds{3});
    return libchiaking::unexpected(platform::NetError::timed_out);
  }

  auto resolve(std::string_view host, std::string_view service,
               const platform::StopToken& stop)
      -> libchiaking::Expected<std::vector<platform::IpAddress>,
                               platform::NetError> override {
    if (host != "slow.invalid") return inner.resolve(host, service, stop);
    ++started;
    PingRig::pause_for(std::chrono::seconds{3}, stop);
    if (stop.stop_requested()) ++interrupted;
    return libchiaking::unexpected(platform::NetError::interrupted);
  }

  platform::DefaultDnsProvider inner{test_support::process_threading(),
                                     PingRig::monotonic()};
  std::atomic<int> started{};
  std::atomic<int> interrupted{};
};

class SlowDnsNetwork final : public platform::NetworkProvider {
 public:
  auto sockets() noexcept -> platform::SocketProvider& override {
    return inner.sockets();
  }
  auto pollers() noexcept -> platform::PollerProvider& override {
    return inner.pollers();
  }
  auto dns() noexcept -> platform::DnsProvider& override { return slow; }
  auto interfaces() noexcept -> platform::InterfacesProvider& override {
    return inner.interfaces();
  }
  auto capabilities() const noexcept -> platform::NetworkCapabilities override {
    return inner.capabilities();
  }

  platform::DefaultNetworkProvider inner{test_support::process_threading(),
                                         PingRig::monotonic()};
  SlowDns slow;
};

TEST(SurveyResolution, RunsInsideThePingAndHonoursCancel) {
  SlowDnsNetwork network;
  test_support::FakeClock clock{1'000};
  test_support::FixedRandom random;
  const std::vector<gk::Datacenter> datacenters{
      {"slow-a", "slow.invalid", 9297, 20'000},
      {"slow-b", "slow.invalid", 9297, 20'000},
      {"literal", "127.0.0.1", 9297, 20'000}};
  const platform::StopToken running;
  const auto started = PingRig::monotonic().now_microseconds();
  auto batch = play::DatacenterSurvey::start(
      {network, clock, test_support::process_threading(), random}, datacenters,
      play::PingService::cloud_stream, PingRig::test_key(), running);
  ASSERT_TRUE(batch);
  EXPECT_LT(PingRig::elapsed_since(started), std::chrono::seconds{1});
  const auto limit = PingRig::deadline_in(std::chrono::seconds{2});
  while (network.slow.started.load() < 2 && PingRig::before(limit)) {
    EXPECT_FALSE(batch.value().step());
    PingRig::pause_for(std::chrono::milliseconds{1});
  }
  EXPECT_EQ(network.slow.started.load(), 2);
  const auto cancelled_at = PingRig::monotonic().now_microseconds();
  const auto final = batch.value().cancel();
  ASSERT_TRUE(final);
  EXPECT_EQ(final->size(), std::size_t{3});
  const auto wait_limit = PingRig::deadline_in(std::chrono::seconds{1});
  while (network.slow.interrupted.load() < 2 && PingRig::before(wait_limit))
    PingRig::pause_for(std::chrono::milliseconds{1});
  EXPECT_EQ(network.slow.interrupted.load(), 2);
  EXPECT_LT(PingRig::elapsed_since(cancelled_at), std::chrono::seconds{1});
}

}  // namespace
}  // namespace cloud_test
