#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/home_link_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace play_test {
namespace {

using namespace home_links;

class HomeLinkSessionRequest : public HomeLinkTest {
 protected:
  void answer(std::string_view response) {
    listener_ = Loopback::listener(providers_.network().sockets());
    ASSERT_NE(listener_, nullptr);
    const auto endpoint = listener_->local_endpoint();
    ASSERT_TRUE(endpoint);
    play::HomeLinkConfiguration config;
    config.console.target = Target::ps5_1;
    config.host = "127.0.0.1";
    config.ctrl_endpoint = endpoint.value();
    auto session =
        play::HomeLinkConfiguration::link(std::move(config), providers_);
    ASSERT_TRUE(session);
    session_ = std::move(session.value());
    session_->set_event_sink(sink_);
    ASSERT_TRUE(session_->start());

    std::unique_ptr<platform::TcpSocket> peer;
    while (!peer && test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(session_->pump_once());
      auto accepted = listener_->accept();
      if (accepted)
        peer = std::move(accepted.value());
      else
        ASSERT_EQ(accepted.error(), NetError::would_block);
      test_support::LiveProviders::spin_pause();
    }
    ASSERT_NE(peer, nullptr);
    std::string request;
    while (request.find("\r\n\r\n") == std::string::npos &&
           test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(session_->pump_once());
      std::array<std::byte, 1024> buffer{};
      const auto received = peer->receive(buffer);
      if (received)
        request.append(reinterpret_cast<const char*>(buffer.data()),
                       received.value());
      else
        ASSERT_EQ(received.error(), NetError::would_block);
      test_support::LiveProviders::spin_pause();
    }
    EXPECT_NE(request.find("/sie/ps5/rp/sess/init"), std::string::npos);
    auto remaining = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(response.data()), response.size());
    while (!remaining.empty() && test_support::LiveProviders::now() < deadline_) {
      const auto sent = peer->send(remaining);
      if (sent)
        remaining = remaining.subspan(sent.value());
      else
        ASSERT_EQ(sent.error(), NetError::would_block);
      test_support::LiveProviders::spin_pause();
    }
    ASSERT_TRUE(remaining.empty());
    EXPECT_TRUE(peer->close());
  }

  void expect_failure(StreamError expected) {
    bool failed = false;
    while (test_support::LiveProviders::now() < deadline_) {
      const auto result = session_->pump_once();
      if (!result) {
        EXPECT_EQ(result.error(), expected);
        failed = true;
        break;
      }
      test_support::LiveProviders::spin_pause();
    }
    ASSERT_TRUE(failed);
    ASSERT_TRUE(session_->failure());
    const auto retained = *session_->failure();
    EXPECT_EQ(retained.route(), play::Venue::home());
    EXPECT_EQ(retained.stage(), remote_play::FailureStage::session_init);
    EXPECT_EQ(retained.result(), expected);
    const auto description = retained.sentence();
    EXPECT_TRUE(description.starts_with("Local console: Session setup"))
        << description;
    EXPECT_EQ(description.find("video session stopped"), std::string::npos)
        << description;
    EXPECT_TRUE(diagnostics_.contains("operation=shutdown outcome=succeeded"));
    EXPECT_EQ(sink_.failed_events(), 1U);
    EXPECT_EQ(session_->state(), StreamState::failed);
    EXPECT_TRUE(listener_->close());
  }

  auto diagnostics() const -> const CapturingDiagnostics& {
    return diagnostics_;
  }

 private:
  ZeroCrypto crypto_;
  CapturingDiagnostics diagnostics_;
  platform::DefaultPlatformFactory providers_{platform::PlatformProviders{
      .crypto = &crypto_, .diagnostics = &diagnostics_}};
  std::unique_ptr<platform::TcpListener> listener_;
  std::unique_ptr<play::Link> session_;
  EventCounter sink_;
  Deadline deadline_{test_support::LiveProviders::now() + std::chrono::seconds{3}};
};

TEST_F(HomeLinkSessionRequest, ReportsATruncatedReplyAsTruncated) {
  ASSERT_NO_FATAL_FAILURE(answer("HTTP/1.1 200 OK\r\nRP-Nonce: AAECAw"));
  ASSERT_NO_FATAL_FAILURE(expect_failure(StreamError::session_init_truncated));
  EXPECT_TRUE(diagnostics().contains(
      "operation=session outcome=failed stage=session_init"));
}

TEST_F(HomeLinkSessionRequest, ReportsAnUnavailableConsoleAsRefused) {
  ASSERT_NO_FATAL_FAILURE(answer("HTTP/1.1 503 Service Unavailable\r\n\r\n"));
  ASSERT_NO_FATAL_FAILURE(expect_failure(StreamError::session_init_refused));
  EXPECT_TRUE(
      diagnostics().contains("local session-init rejected: http=503 reason=0"));
}

TEST_F(HomeLinkSessionRequest, ReportsAConsoleAlreadyInUse) {
  ASSERT_NO_FATAL_FAILURE(
      answer("HTTP/1.1 403 Forbidden\r\n"
             "RP-Application-Reason: 80108b10\r\n\r\n"));
  ASSERT_NO_FATAL_FAILURE(expect_failure(StreamError::session_init_in_use));
  EXPECT_TRUE(diagnostics().contains(
      "local session-init rejected: http=403 reason=2148567824"));
}

TEST_F(HomeLinkSessionRequest, ReportsAVersionMismatch) {
  ASSERT_NO_FATAL_FAILURE(
      answer("HTTP/1.1 400 Bad Request\r\n"
             "RP-Application-Reason: 80108b11\r\nRP-Version: 2.0\r\n\r\n"));
  ASSERT_NO_FATAL_FAILURE(
      expect_failure(StreamError::session_init_version_mismatch));
  EXPECT_TRUE(diagnostics().contains(
      "operation=session outcome=failed stage=session_init"));
}

class HomeLinkSessionRetry : public HomeLinkTest {
 protected:
  void refuse_with_version_five(std::string_view reason) {
    listener_ = Loopback::listener(providers_.network().sockets(), 4);
    ASSERT_NE(listener_, nullptr);
    const auto endpoint = listener_->local_endpoint();
    ASSERT_TRUE(endpoint);
    play::HomeLinkConfiguration config;
    config.console.target = Target::ps4_9;
    config.host = "127.0.0.1";
    config.ctrl_endpoint = endpoint.value();
    auto session =
        play::HomeLinkConfiguration::link(std::move(config), providers_);
    ASSERT_TRUE(session);
    session_ = std::move(session.value());
    session_->set_event_sink(sink_);
    ASSERT_TRUE(session_->start());
    const std::string response =
        "HTTP/1.1 403 Forbidden\r\n"
        "RP-Application-Reason: " +
        std::string(reason) + "\r\nRP-Version: 5.0\r\n\r\n";
    for (int attempt = 0; attempt < 3; ++attempt) {
      SCOPED_TRACE(attempt);
      std::unique_ptr<platform::TcpSocket> peer;
      while (!peer && test_support::LiveProviders::now() < deadline_) {
        ASSERT_TRUE(session_->pump_once());
        auto accepted = listener_->accept();
        if (accepted) peer = std::move(accepted.value());
        test_support::LiveProviders::spin_pause();
      }
      ASSERT_NE(peer, nullptr);
      std::string request;
      while (request.find("\r\n\r\n") == std::string::npos &&
             test_support::LiveProviders::now() < deadline_) {
        ASSERT_TRUE(session_->pump_once());
        std::array<std::byte, 1024> buffer{};
        const auto received = peer->receive(buffer);
        if (received)
          request.append(reinterpret_cast<const char*>(buffer.data()),
                         received.value());
        test_support::LiveProviders::spin_pause();
      }
      EXPECT_NE(request.find("/sce/rp/session"), std::string::npos);
      EXPECT_NE(request.find("Rp-Version: 9.0\r\n"), std::string::npos);
      auto remaining = std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(response.data()), response.size());
      while (!remaining.empty() && test_support::LiveProviders::now() < deadline_) {
        const auto sent = peer->send(remaining);
        if (sent) remaining = remaining.subspan(sent.value());
        test_support::LiveProviders::spin_pause();
      }
      ASSERT_TRUE(remaining.empty());
      EXPECT_TRUE(peer->close());
    }
  }

  void expect_given_up(StreamError expected_error,
                       remote_play::QuitReason expected_quit) {
    bool failed = false;
    while (!failed && test_support::LiveProviders::now() < deadline_) {
      const auto result = session_->pump_once();
      if (!result) {
        EXPECT_EQ(result.error(), expected_error);
        failed = true;
      }
      test_support::LiveProviders::spin_pause();
    }
    ASSERT_TRUE(failed);
    EXPECT_EQ(session_->quit().reason, expected_quit);
    EXPECT_TRUE(listener_->close());
  }

 private:
  ZeroCrypto crypto_;
  platform::DefaultPlatformFactory providers_{&crypto_};
  std::unique_ptr<platform::TcpListener> listener_;
  std::unique_ptr<play::Link> session_;
  EventCounter sink_;
  Deadline deadline_{test_support::LiveProviders::now() + std::chrono::seconds{5}};
};

TEST_F(HomeLinkSessionRetry, GivesUpOnAVersionMismatchAfterRetrying) {
  ASSERT_NO_FATAL_FAILURE(refuse_with_version_five("80108b11"));
  expect_given_up(StreamError::session_init_version_mismatch,
                  remote_play::QuitReason::session_request_rp_version_mismatch);
}

TEST_F(HomeLinkSessionRetry, GivesUpOnAnUnknownReasonAfterRetrying) {
  ASSERT_NO_FATAL_FAILURE(refuse_with_version_five("80108bff"));
  expect_given_up(StreamError::session_init_refused,
                  remote_play::QuitReason::session_request_unknown);
}

class HomeLinkSessionAddress : public HomeLinkTest {
 protected:
  auto providers() -> platform::DefaultPlatformFactory& { return providers_; }

 private:
  ZeroCrypto crypto_;
  platform::DefaultPlatformFactory providers_{&crypto_};
};

TEST_F(HomeLinkSessionAddress, ReportsARefusedConnectionAsItsQuitReason) {
  auto listener = Loopback::listener(providers().network().sockets());
  ASSERT_NE(listener, nullptr);
  const auto endpoint = listener->local_endpoint();
  ASSERT_TRUE(endpoint);
  EXPECT_TRUE(listener->close());
  play::HomeLinkConfiguration config;
  config.console.target = Target::ps5_1;
  config.host = "127.0.0.1";
  config.ctrl_endpoint = endpoint.value();
  auto session =
      play::HomeLinkConfiguration::link(std::move(config), providers());
  ASSERT_TRUE(session);
  EventCounter sink;
  session.value()->set_event_sink(sink);
  auto progress = session.value()->start();
  const auto deadline = test_support::LiveProviders::now() + std::chrono::seconds{3};
  while (progress && test_support::LiveProviders::now() < deadline) {
    progress = session.value()->pump_once();
    test_support::LiveProviders::spin_pause();
  }
  ASSERT_FALSE(progress);
  EXPECT_EQ(progress.error(), StreamError::session_init_connection_refused);
  EXPECT_EQ(session.value()->state(), StreamState::failed);
  ASSERT_TRUE(session.value()->failure());
  EXPECT_EQ(session.value()->failure()->result(),
            StreamError::session_init_connection_refused);
  EXPECT_EQ(session.value()->quit().reason,
            remote_play::QuitReason::session_request_connection_refused);
}

TEST_F(HomeLinkSessionAddress, SendsTheNumericAddressAsTheHostHeader) {
  auto listener = Loopback::listener(providers().network().sockets());
  ASSERT_NE(listener, nullptr);
  const auto endpoint = listener->local_endpoint();
  ASSERT_TRUE(endpoint);
  play::HomeLinkConfiguration config;
  config.console.target = Target::ps5_1;
  config.host = "console.test.invalid";
  config.ctrl_endpoint = endpoint.value();
  config.route.local = {platform::IpAddress::loopback_ipv4(), 0};
  auto session =
      play::HomeLinkConfiguration::link(std::move(config), providers());
  ASSERT_TRUE(session);
  EventCounter sink;
  session.value()->set_event_sink(sink);
  ASSERT_TRUE(session.value()->start());
  const auto host =
      "Host: 127.0.0.1:" + std::to_string(endpoint.value().port) + "\r\n";
  const auto deadline = test_support::LiveProviders::now() + std::chrono::seconds{3};
  SessionInitConsole console{*session.value(), *listener, deadline};
  ASSERT_NO_FATAL_FAILURE(console.open_ctrl(host));
  auto* ctrl = console.ctrl();
  ASSERT_NE(ctrl, nullptr);
  std::string request;
  while (request.find("\r\n\r\n") == std::string::npos &&
         test_support::LiveProviders::now() < deadline) {
    ASSERT_TRUE(session.value()->pump_once());
    std::array<std::byte, 1024> buffer{};
    const auto received = ctrl->receive(buffer);
    if (received)
      request.append(reinterpret_cast<const char*>(buffer.data()),
                     received.value());
    test_support::LiveProviders::spin_pause();
  }
  EXPECT_NE(request.find(host), std::string::npos) << request;
  EXPECT_EQ(request.find("console.test.invalid"), std::string::npos) << request;
  EXPECT_TRUE(session.value()->stop());
  EXPECT_TRUE(ctrl->close());
  EXPECT_TRUE(listener->close());
}

TEST_F(HomeLinkSessionAddress, ReachesALinkLocalConsoleThroughItsZone) {
  platform::DefaultNetworkProvider probe{test_support::LiveProviders::threading(),
                                         test_support::LiveProviders::steady()};
  const auto interfaces = probe.interfaces().enumerate();
  ASSERT_TRUE(interfaces);
  std::string zone;
  for (const auto& entry : interfaces.value()) {
    if (entry.loopback && entry.address.is_link_local() &&
        entry.address.ipv6_bytes()[15] == 1)
      zone = entry.name;
  }
  if (zone.empty())
    GTEST_SKIP() << "no loopback interface carries a link-local address";
  const std::string host = "fe80::1%" + zone;
  const auto address = platform::parse_ip_address(host);
  ASSERT_TRUE(address);
  EXPECT_NE(address.value().scope_id(), 0U);
  auto listener =
      Loopback::listener(providers().network().sockets(), 1, address.value());
  ASSERT_NE(listener, nullptr);
  const auto endpoint = listener->local_endpoint();
  ASSERT_TRUE(endpoint);
  play::HomeLinkConfiguration config;
  config.console.target = Target::ps5_1;
  config.host = host;
  config.ctrl_endpoint = {platform::IpAddress::any_ipv6(),
                          endpoint.value().port};
  auto session =
      play::HomeLinkConfiguration::link(std::move(config), providers());
  ASSERT_TRUE(session);
  EventCounter sink;
  session.value()->set_event_sink(sink);
  ASSERT_TRUE(session.value()->start());
  const auto deadline = test_support::LiveProviders::now() + std::chrono::seconds{3};
  std::unique_ptr<platform::TcpSocket> peer;
  while (!peer && test_support::LiveProviders::now() < deadline) {
    ASSERT_TRUE(session.value()->pump_once());
    auto accepted = listener->accept();
    if (accepted) peer = std::move(accepted.value());
    test_support::LiveProviders::spin_pause();
  }
  ASSERT_NE(peer, nullptr);
  std::string request;
  while (request.find("\r\n\r\n") == std::string::npos &&
         test_support::LiveProviders::now() < deadline) {
    ASSERT_TRUE(session.value()->pump_once());
    std::array<std::byte, 1024> buffer{};
    const auto received = peer->receive(buffer);
    if (received)
      request.append(reinterpret_cast<const char*>(buffer.data()),
                     received.value());
    test_support::LiveProviders::spin_pause();
  }
  const auto expected_host = "\r\nHost: " + host + ":" +
                             std::to_string(endpoint.value().port) + "\r\n";
  EXPECT_NE(request.find(expected_host), std::string::npos) << request;
  EXPECT_TRUE(session.value()->stop());
  EXPECT_TRUE(peer->close());
  EXPECT_TRUE(listener->close());
}

class HomeLinkCtrlOpening : public HomeLinkTest {
 protected:
  void link() {
    listener_ = Loopback::listener(providers_.network().sockets());
    ASSERT_NE(listener_, nullptr);
    const auto endpoint = listener_->local_endpoint();
    ASSERT_TRUE(endpoint);
    play::HomeLinkConfiguration config;
    config.console.target = Target::ps5_1;
    config.host = "127.0.0.1";
    config.ctrl_endpoint = endpoint.value();
    auto session =
        play::HomeLinkConfiguration::link(std::move(config), providers_);
    ASSERT_TRUE(session);
    session_ = std::move(session.value());
    session_->set_event_sink(sink_);
    ASSERT_TRUE(session_->start());
  }

  auto session() -> play::Link& { return *session_; }
  auto listener() -> platform::TcpListener& { return *listener_; }
  auto clock() -> FixedClock& { return clock_; }

 private:
  ZeroCrypto crypto_;
  FixedClock clock_;
  platform::DefaultPlatformFactory providers_{
      platform::PlatformProviders{.clock = &clock_, .crypto = &crypto_}};
  std::unique_ptr<platform::TcpListener> listener_;
  std::unique_ptr<play::Link> session_;
  EventCounter sink_;
};

TEST_F(HomeLinkCtrlOpening, OpensCtrlOnlyAfterGivingTheConsoleTime) {
  ASSERT_NO_FATAL_FAILURE(link());
  SessionInitConsole console{session(), listener(),
                             test_support::LiveProviders::now() + std::chrono::seconds{3}};
  ASSERT_NO_FATAL_FAILURE(console.open_ctrl("/sie/ps5/rp/sess/init", &clock()));
  auto* ctrl = console.ctrl();
  ASSERT_NE(ctrl, nullptr);
  EXPECT_TRUE(session().stop());
  EXPECT_TRUE(ctrl->close());
  EXPECT_TRUE(listener().close());
}

TEST_F(HomeLinkCtrlOpening, NeverOpensCtrlWhenStoppedWhileWaiting) {
  ASSERT_NO_FATAL_FAILURE(link());
  SessionInitConsole console{session(), listener(),
                             test_support::LiveProviders::now() + std::chrono::seconds{3}};
  ASSERT_NO_FATAL_FAILURE(
      console.stop_before_ctrl("/sie/ps5/rp/sess/init", clock()));
  EXPECT_EQ(console.ctrl(), nullptr);
  EXPECT_TRUE(listener().close());
}

class HomeLinkWaiting : public HomeLinkTest {
 protected:
  auto link() -> std::unique_ptr<play::Link> {
    play::HomeLinkConfiguration config;
    config.host = "127.0.0.1";
    config.ctrl_endpoint = {platform::IpAddress::loopback_ipv4(), 9295};
    auto session =
        play::HomeLinkConfiguration::link(std::move(config), providers_);
    EXPECT_TRUE(session);
    if (!session) return nullptr;
    return std::move(session.value());
  }

 private:
  ZeroCrypto crypto_;
  platform::DefaultPlatformFactory providers_{&crypto_};
};

TEST_F(HomeLinkWaiting, BlocksUntilTheTimeout) {
  auto session = link();
  ASSERT_NE(session, nullptr);
  const auto started = test_support::LiveProviders::now();
  EXPECT_TRUE(session->wait(std::chrono::milliseconds{150}));
  EXPECT_GE(test_support::LiveProviders::now() - started, std::chrono::milliseconds{100});
}

TEST_F(HomeLinkWaiting, ReturnsEarlyWhenWoken) {
  auto session = link();
  ASSERT_NE(session, nullptr);
  auto waker =
      platform::Thread::start(test_support::LiveProviders::threading(), "waker",
                              platform::ThreadRole::unspecified,
                              [&] {
                                test_support::LiveProviders::nap(std::chrono::milliseconds{50});
                                session->wake();
                              })
          .value();
  const auto started = test_support::LiveProviders::now();
  EXPECT_TRUE(session->wait(std::chrono::seconds{5}));
  const auto elapsed = test_support::LiveProviders::now() - started;
  static_cast<void>(waker.join());
  EXPECT_LT(elapsed, std::chrono::seconds{2});
}

}  // namespace
}  // namespace play_test
