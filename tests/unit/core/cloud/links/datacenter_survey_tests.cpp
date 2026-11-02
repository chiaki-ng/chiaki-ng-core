#include "libchiaking/platform/net/default.hpp"
#include "src/core/net/transport/datagram/udp_transport.hpp"
#include "src/core/play/adapters/links/cloud/datacenter_survey.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/random.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/support/waiter.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace cloud_test {
namespace {

namespace gk = libchiaking::psn::gk;
namespace play = libchiaking::play;
namespace platform = libchiaking::platform;
namespace datagram = libchiaking::net::transport::datagram;
using Error = play::DatacenterSurvey::Error;

class GkDatacenterMeasure : public ::testing::Test {
 protected:
  void SetUp() override {
    waiter.answer = false;
    key.assign("synthetic-lock-key");
  }

  auto measure(const std::vector<gk::Datacenter>& listed,
               const libchiaking::SecretString& lock_key,
               const platform::StopToken& stop)
      -> libchiaking::Expected<std::vector<gk::Ping>, Error> {
    auto survey = play::DatacenterSurvey::start(
        providers, listed, play::PingService::cloud_stream, lock_key, stop);
    if (!survey) return libchiaking::unexpected(survey.error());
    auto result = survey.value().run(waiter, stop);
    if (!result) return libchiaking::unexpected(result.error());
    return std::move(result.value().rows);
  }

  auto measure(const std::vector<gk::Datacenter>& listed,
               const platform::StopToken& stop = {})
      -> libchiaking::Expected<std::vector<gk::Ping>, Error> {
    return measure(listed, key, stop);
  }

  platform::SteadyClock steady;
  platform::DefaultNetworkProvider network{test_support::process_threading(),
                                           steady};
  test_support::FakeClock clock{1'000};
  test_support::FixedRandom random;
  test_support::FakeWaiter waiter;
  const play::DatacenterSurvey::Providers providers{
      network, clock, test_support::process_threading(), random};
  libchiaking::SecretString key;
  const std::vector<gk::Datacenter> unresolved{{"unresolved", "", 0, 20'000}};
};

TEST_F(GkDatacenterMeasure, LeavesAnUnresolvableRowUnmeasured) {
  const auto measured = measure(unresolved);
  ASSERT_TRUE(measured);
  ASSERT_EQ(measured.value().size(), std::size_t{1});
  EXPECT_EQ(measured.value()[0].name, "unresolved");
  EXPECT_FALSE(measured.value()[0].measured);
  EXPECT_EQ(measured.value()[0].rtt_ms, 999);
  EXPECT_EQ(waiter.calls(), 0);
  EXPECT_EQ(key.view(), "synthetic-lock-key");
}

TEST_F(GkDatacenterMeasure, RefusesAnEmptyKey) {
  const libchiaking::SecretString empty;
  const auto measured = measure(unresolved, empty, {});
  ASSERT_FALSE(measured);
  EXPECT_EQ(measured.error(), Error::invalid_key);
}

TEST_F(GkDatacenterMeasure, RefusesADeadlinePastTheClock) {
  clock.now_us = std::numeric_limits<std::uint64_t>::max();
  const auto measured = measure(unresolved);
  ASSERT_FALSE(measured);
  EXPECT_EQ(measured.error(), Error::deadline_overflow);
}

TEST_F(GkDatacenterMeasure, IsCancelledBeforeWaitingOnceStopped) {
  const auto stop_state = std::make_shared<test_support::StopState>();
  stop_state->stopped = true;
  const auto measured = measure(unresolved, platform::StopToken{stop_state});
  ASSERT_FALSE(measured);
  EXPECT_EQ(measured.error(), Error::cancelled);
  EXPECT_EQ(waiter.calls(), 0);
}

TEST_F(GkDatacenterMeasure, IsCancelledByAStopDuringTheWait) {
  const auto stop_state = std::make_shared<test_support::StopState>();
  datagram::UdpTransportConfig peer_config;
  peer_config.local = {platform::IpAddress::loopback_ipv4(), 0};
  auto peer = datagram::ProviderUdpTransport::create(network, peer_config);
  ASSERT_TRUE(peer);
  const auto endpoint = peer.value()->local_endpoint();
  ASSERT_TRUE(endpoint);
  const std::vector<gk::Datacenter> reachable{
      {"reachable", "127.0.0.1", endpoint.value().port, 20'000}};
  waiter.stop_state = stop_state.get();
  waiter.stop_on_wait = 1;
  const auto measured = measure(reachable, platform::StopToken{stop_state});
  ASSERT_FALSE(measured);
  EXPECT_EQ(measured.error(), Error::cancelled);
  EXPECT_EQ(waiter.calls(), 1);
  EXPECT_EQ(key.view(), "synthetic-lock-key");
}

}  // namespace
}  // namespace cloud_test
