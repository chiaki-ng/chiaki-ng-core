#include "tests/support/enum_printing.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/play/adapters/links/home_link_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <utility>
#include <vector>

namespace play_test {
namespace {

using namespace home_links;

class HomeLinkResolution : public HomeLinkTest {
 protected:
  auto link(play::HomeLinkConfiguration config) -> std::unique_ptr<play::Link> {
    auto session =
        play::HomeLinkConfiguration::link(std::move(config), providers_);
    EXPECT_TRUE(session);
    if (!session) return nullptr;
    return std::move(session.value());
  }

  static auto named(platform::SocketEndpoint endpoint)
      -> play::HomeLinkConfiguration {
    play::HomeLinkConfiguration config;
    config.host = "ctrl.test.invalid";
    config.ctrl_endpoint = endpoint;
    return config;
  }

  auto network() -> ScriptedNetwork& { return network_; }
  auto crypto() -> ZeroCrypto& { return crypto_; }

 private:
  ZeroCrypto crypto_;
  ScriptedNetwork network_;
  platform::DefaultPlatformFactory providers_{
      platform::PlatformProviders{.network = &network_, .crypto = &crypto_}};
};

TEST_F(HomeLinkResolution, StopsResolvingWhenCancelled) {
  ScriptedNetwork network;
  network.scripted_dns().hang_until_stopped();
  auto state = std::make_shared<test_support::AtomicStop>();
  auto config = named({platform::IpAddress::any_ipv4(), 9295});
  config.stop = platform::StopToken{state};
  platform::DefaultPlatformFactory providers(
      platform::PlatformProviders{.network = &network, .crypto = &crypto()});
  auto session =
      play::HomeLinkConfiguration::link(std::move(config), providers);
  ASSERT_TRUE(session);
  EventCounter sink;
  session.value()->set_event_sink(sink);
  auto canceller =
      platform::Thread::start(test_support::LiveProviders::threading(), "canceller",
                              platform::ThreadRole::unspecified,
                              [state] {
                                test_support::LiveProviders::nap(std::chrono::milliseconds{100});
                                state->request_stop();
                              })
          .value();
  const auto started_at = test_support::LiveProviders::now();
  const auto started = session.value()->start();
  const auto elapsed = test_support::LiveProviders::now() - started_at;
  static_cast<void>(canceller.join());
  EXPECT_EQ(network.scripted_dns().stop_aware_calls(), 1U);
  EXPECT_LT(elapsed, std::chrono::seconds{2});
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), StreamError::route_failed);
  EXPECT_EQ(session.value()->state(), StreamState::failed);
  const auto* failed = sink.last_as<remote_play::event::Failed>();
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->reason, remote_play::StreamFailure::cancelled);
  EXPECT_EQ(session.value()->quit().reason, remote_play::QuitReason::stopped);
}

TEST_F(HomeLinkResolution, FailsWhenNoAnswerHasTheRequestedFamily) {
  network().scripted_dns().answer({platform::IpAddress::loopback_ipv4()});
  auto unresolved = link(named({platform::IpAddress::any_ipv6(), 9295}));
  ASSERT_NE(unresolved, nullptr);
  EventCounter sink;
  unresolved->set_event_sink(sink);
  const auto resolve_result = unresolved->start();
  ASSERT_FALSE(resolve_result);
  EXPECT_EQ(resolve_result.error(), StreamError::ctrl_failed);
  EXPECT_EQ(unresolved->state(), StreamState::failed);
  EXPECT_EQ(sink.failed_events(), 1U);
  const auto* failed = sink.last_as<remote_play::event::Failed>();
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->reason,
            remote_play::StreamFailure::route_establishment_failed);
  const auto& mismatch = unresolved->failure();
  ASSERT_TRUE(mismatch);
  EXPECT_EQ(mismatch->route(), play::Venue::home());
  EXPECT_EQ(mismatch->stage(), remote_play::FailureStage::resolve);
  EXPECT_EQ(mismatch->result(), StreamError::ctrl_failed);
  EXPECT_EQ(mismatch->as<play::LinkFailure::ConsoleUnresolved>(), nullptr);
  EXPECT_EQ(network().scripted_dns().calls(), 1U);
  EXPECT_FALSE(network().scripted_sockets().tcp_family());
}

TEST_F(HomeLinkResolution, ReportsTheLookupErrorWhenResolutionFails) {
  network().scripted_dns().fail_with(NetError::not_supported);
  auto lookup_failed = link(named({platform::IpAddress::any_ipv4(), 9295}));
  ASSERT_NE(lookup_failed, nullptr);
  EventCounter sink;
  lookup_failed->set_event_sink(sink);
  const auto lookup_result = lookup_failed->start();
  ASSERT_FALSE(lookup_result);
  EXPECT_EQ(lookup_result.error(), StreamError::ctrl_failed);
  EXPECT_EQ(lookup_failed->state(), StreamState::failed);
  EXPECT_EQ(sink.failed_events(), 1U);
  const auto& dns = lookup_failed->failure();
  ASSERT_TRUE(dns);
  EXPECT_EQ(dns->stage(), remote_play::FailureStage::resolve);
  const auto* unresolved = dns->as<play::LinkFailure::ConsoleUnresolved>();
  ASSERT_NE(unresolved, nullptr);
  EXPECT_EQ(unresolved->error, NetError::not_supported);
  EXPECT_FALSE(network().scripted_sockets().tcp_family());
}

TEST_F(HomeLinkResolution, FailsBeforeResolvingWhenTheDeviceIdCannotBeMade) {
  test_support::FailingRandom random{platform::PlatformError::system_failure};
  ScriptedNetwork network;
  platform::DefaultPlatformFactory providers(platform::PlatformProviders{
      .random = &random, .network = &network, .crypto = &crypto()});
  play::HomeLinkConfiguration config;
  config.host = "127.0.0.1";
  config.ctrl_endpoint = {platform::IpAddress::loopback_ipv4(), 9295};
  auto session =
      play::HomeLinkConfiguration::link(std::move(config), providers);
  ASSERT_TRUE(session);
  EventCounter sink;
  session.value()->set_event_sink(sink);
  const auto started = session.value()->start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), StreamError::ctrl_failed);
  EXPECT_EQ(session.value()->state(), StreamState::failed);
  EXPECT_EQ(sink.failed_events(), 1U);
  const auto* failed = sink.last_as<remote_play::event::Failed>();
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->reason, remote_play::StreamFailure::negotiation_failed);
  const auto& detail = session.value()->failure();
  ASSERT_TRUE(detail);
  EXPECT_EQ(detail->route(), play::Venue::home());
  EXPECT_EQ(detail->stage(), remote_play::FailureStage::ctrl_startup);
  EXPECT_EQ(detail->result(), StreamError::ctrl_failed);
  EXPECT_EQ(network.scripted_dns().calls(), 0U);
  EXPECT_FALSE(network.scripted_sockets().tcp_family());
}

TEST_F(HomeLinkResolution, ResolvesAnIpv6LiteralAsIpv6) {
  network().scripted_dns().answer({platform::IpAddress::loopback_ipv6()});
  play::HomeLinkConfiguration config;
  config.host = "::1";
  config.ctrl_endpoint.port = 9295;
  auto session = link(std::move(config));
  ASSERT_NE(session, nullptr);
  EventCounter sink;
  session->set_event_sink(sink);
  const auto started = session->start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), StreamError::session_init_network_failure);
  EXPECT_EQ(network().scripted_dns().calls(), 1U);
  EXPECT_EQ(network().scripted_dns().host(), "::1");
  EXPECT_EQ(network().scripted_sockets().tcp_family(),
            platform::IpAddress::loopback_ipv6());
  EXPECT_EQ(sink.failed_events(), 1U);
}

TEST_F(HomeLinkResolution, ConnectsToTheRequestedFamily) {
  network().scripted_dns().answer({platform::IpAddress::loopback_ipv4(),
                                   platform::IpAddress::loopback_ipv6()});
  auto session = link(named({platform::IpAddress::any_ipv6(), 4321}));
  ASSERT_NE(session, nullptr);
  EventCounter sink;
  session->set_event_sink(sink);
  const auto started = session->start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), StreamError::session_init_network_failure);
  EXPECT_EQ(network().scripted_dns().calls(), 1U);
  EXPECT_EQ(network().scripted_dns().host(), "ctrl.test.invalid");
  EXPECT_EQ(network().scripted_dns().service(), "4321");
  EXPECT_EQ(network().scripted_sockets().tcp_family(),
            platform::IpAddress::loopback_ipv6());
  EXPECT_EQ(session->state(), StreamState::failed);
  EXPECT_EQ(sink.failed_events(), 1U);
}

TEST_F(HomeLinkResolution, TriesEveryAnswerOfTheRequestedFamily) {
  const auto unreachable = platform::IpAddress::ipv4({192, 0, 2, 1});
  network().scripted_dns().answer({unreachable,
                                   platform::IpAddress::loopback_ipv6(),
                                   platform::IpAddress::ipv4({192, 0, 2, 2})});
  auto session = link(named({platform::IpAddress::any_ipv4(), 4321}));
  ASSERT_NE(session, nullptr);
  EventCounter sink;
  session->set_event_sink(sink);
  const auto started = session->start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), StreamError::session_init_network_failure);
  EXPECT_EQ(network().scripted_sockets().tcp_attempts(),
            (std::vector<platform::IpAddress>{
                unreachable, platform::IpAddress::ipv4({192, 0, 2, 2})}));
  EXPECT_EQ(sink.failed_events(), 1U);
}

TEST_F(HomeLinkResolution, ConnectsToTheFirstAnswerThatAccepts) {
  const auto unreachable = platform::IpAddress::ipv4({192, 0, 2, 1});
  network().scripted_sockets().delegate_to(network().default_sockets());
  network().scripted_sockets().refuse(unreachable);
  auto listener = Loopback::listener(network().default_sockets());
  ASSERT_NE(listener, nullptr);
  const auto endpoint = listener->local_endpoint();
  ASSERT_TRUE(endpoint);
  network().scripted_dns().answer(
      {unreachable, platform::IpAddress::loopback_ipv4()});
  auto retried =
      link(named({platform::IpAddress::any_ipv4(), endpoint.value().port}));
  ASSERT_NE(retried, nullptr);
  EventCounter sink;
  retried->set_event_sink(sink);
  ASSERT_TRUE(retried->start());
  const auto deadline = test_support::LiveProviders::now() + std::chrono::seconds{3};
  std::unique_ptr<platform::TcpSocket> accepted;
  while (!accepted && test_support::LiveProviders::now() < deadline) {
    ASSERT_TRUE(retried->pump_once());
    auto next = listener->accept();
    if (next) accepted = std::move(next.value());
    test_support::LiveProviders::spin_pause();
  }
  ASSERT_NE(accepted, nullptr);
  EXPECT_EQ(network().scripted_sockets().tcp_attempts(),
            (std::vector<platform::IpAddress>{
                unreachable, platform::IpAddress::loopback_ipv4()}));
  EXPECT_TRUE(retried->stop());
  EXPECT_TRUE(accepted->close());
  EXPECT_TRUE(listener->close());
}

}  // namespace
}  // namespace play_test
