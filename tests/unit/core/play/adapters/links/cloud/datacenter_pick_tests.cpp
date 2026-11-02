#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/net/default.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "tests/support/crypto/refusing.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/live_providers.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/play/cleared_launch.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace play_test {

using test_support::LiveProviders;
using test_support::play::SignedInPlayer;
namespace {

namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using StreamError = remote_play::StreamError;

class SigningCrypto final : public platform::CryptoProvider {
 public:
  auto generate_ecdh(platform::EcdhCurve)
      -> libchiaking::Expected<std::unique_ptr<platform::EcdhKeyPair>,
                               platform::CryptoError> override {
    return std::unique_ptr<platform::EcdhKeyPair>{
        new test_support::CountingKeyPair};
  }
  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }
  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<std::byte> output)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    std::ranges::fill(output, std::byte{0x5a});
    return {};
  }
  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }
  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }
};

class PickingPs final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    const platform::Lock lock(mutex);
    platform::HttpResponse response;
    response.status_code = 200;
    std::string body;
    const std::string posted{request.body.view()};
    const auto step = calls++;
    switch (step) {
      case 0:
        EXPECT_NE(request.url.view().find("authorizeCheck"), std::string::npos);
        response.status_code = 204;
        break;
      case 1:
        body =
            R"({"gkClientId":"gk-id","ps3GkClientId":"ps3-id","streamServerClientId":"server-id"})";
        break;
      case 2:
        body = R"({"configKey":"config-key"})";
        break;
      case 3:
        body = R"({"sessionId":"synthetic-session"})";
        response.headers.push_back(
            {"X-Gaikai-Session", libchiaking::SecretString{"start-key"}});
        break;
      case 4:
      case 5:
        response.status_code = 302;
        response.headers.push_back(
            {"Location",
             libchiaking::SecretString{
                 step == 4 ? (request.url.view().find("grc-response") !=
                                      std::string_view::npos
                                  ? std::string{"https://psnow.playstation.com/"
                                                "app/2.2.0/133/5cdcc037d/"
                                                "grc-response.html"}
                                  : std::string{"gaikai://local"}) +
                                 "?code=CLIENT"
                           : (request.url.view().find("grc-response") !=
                                      std::string_view::npos
                                  ? std::string{"https://psnow.playstation.com/"
                                                "app/2.2.0/133/5cdcc037d/"
                                                "grc-response.html"}
                                  : std::string{"gaikai://local"}) +
                                 "?code=SERVER"}});
        break;
      case 6:
        response.headers.push_back(
            {"X-Gaikai-Session", libchiaking::SecretString{"authorize-key"}});
        break;
      case 7:
        body = R"({"lockAcquired":true,"pollFrequency":5})";
        break;
      case 8:
        EXPECT_TRUE(request.url.view().ends_with("/datacenters"));
        body =
            R"([{"dataCenter":"dc1","publicIp":"","port":0,"maxBandwidth":20000},)"
            R"({"dataCenter":"dc2","publicIp":"","port":0,"maxBandwidth":20000}])";
        response.headers.push_back(
            {"X-Gaikai-Session", libchiaking::SecretString{"datacenter-key"}});
        break;
      case 9:
        EXPECT_TRUE(request.url.view().ends_with("/datacenters/select"));
        select_body = posted;
        body = R"({"port":8975})";
        response.headers.push_back(
            {"X-Gaikai-Session", libchiaking::SecretString{"select-key"}});
        break;
      case 10:
        EXPECT_TRUE(request.url.view().ends_with("/allocate"));
        allocate_body = posted;
        body =
            R"({"launchSlot":{"publicIp":"127.0.0.1","port":)" +
            std::to_string(allocation_port) +
            R"(,"privateIp":"10.0.0.7"},"handshakeKey":"AAECAwQFBgcICQoLDA0ODw==","launchSpecification":"synthetic-launch","sessionId":"synthetic-session"})";
        break;
      default:
        ADD_FAILURE() << "unscripted request " << step << ": "
                      << request.url.view();
    }
    for (const char character : body)
      response.body.push_back(static_cast<std::byte>(character));
    return response;
  }

  platform::Mutex mutex{LiveProviders::threading()};
  std::uint16_t allocation_port{};
  unsigned calls{};
  std::string select_body;
  std::string allocate_body;
};

class OfferSink final : public play::LinkEventSink {
 public:
  void on_event(remote_play::StreamEvent event) override {
    const platform::Lock lock(mutex);
    if (auto* progress =
            std::get_if<remote_play::event::SetupProgress>(&event)) {
      progress_texts.push_back(progress->text);
      return;
    }
    if (auto* rows =
            std::get_if<remote_play::event::CloudDatacenters>(&event)) {
      datacenters = rows->datacenters;
      offered.store(true, std::memory_order_release);
      offer_made.notify_all();
      return;
    }
    if (auto* chosen =
            std::get_if<remote_play::event::CloudDatacenterSelected>(&event)) {
      selections.push_back(*chosen);
      return;
    }
    if (std::holds_alternative<remote_play::event::Failed>(event))
      ++failed_events;
  }

  void wait_for_offer() const {
    platform::Lock lock(mutex);
    EXPECT_TRUE(offer_made.wait_for(lock, std::chrono::seconds{20}, [this] {
      return offered.load(std::memory_order_acquire);
    }));
  }

  auto progressed(std::string_view text) const -> bool {
    return std::ranges::find(progress_texts, text) != progress_texts.end();
  }

  mutable platform::Mutex mutex{LiveProviders::threading()};
  mutable platform::ConditionVariable offer_made{LiveProviders::threading(),
                                                 LiveProviders::steady()};
  std::atomic<bool> offered{};
  std::vector<std::string> progress_texts;
  std::vector<libchiaking::cloud::DatacenterPing> datacenters;
  std::vector<remote_play::event::CloudDatacenterSelected> selections;
  unsigned failed_events{};
};

class CloudDatacenterPick : public ::testing::Test {
 protected:
  void SetUp() override {
    auto peer = network_.sockets().create_udp_socket(
        platform::IpAddress::loopback_ipv4());
    ASSERT_TRUE(peer);
    ASSERT_TRUE(peer.value()->bind({platform::IpAddress::loopback_ipv4(), 0}));
    http.allocation_port = peer.value()->local_endpoint().value().port;
    peer_socket_ = std::move(peer.value());
  }

  auto make(std::chrono::milliseconds timeout) -> std::unique_ptr<play::Link> {
    play::CloudLinkConfiguration config;
    config.stop = platform::StopToken{stop};
    config.user = SignedInPlayer::fresh();
    cleared_.arm(config);
    config.time_zone = "UTC+00:00";
    config.datacenter =
        play::DatacenterPreference::ask_at_launch(timeout).value();
    auto session = play::CloudLinkConfiguration::link(
        std::move(config), factory_, cleared_.seams());
    EXPECT_TRUE(session);
    if (!session) return nullptr;
    session.value()->set_event_sink(sink);
    return std::move(session.value());
  }

  static constexpr std::string_view unreachable =
      "no datacenter passed the latency and reachability checks";

  PickingPs http;
  OfferSink sink;
  std::shared_ptr<test_support::AtomicStop> stop{
      std::make_shared<test_support::AtomicStop>()};

 private:
  ClearedLaunch cleared_;
  platform::DefaultNetworkProvider network_{LiveProviders::threading(),
                                            LiveProviders::steady()};
  SigningCrypto crypto_;
  platform::DefaultPlatformFactory factory_{platform::PlatformProviders{
      .network = &network_, .crypto = &crypto_, .http = &http}};
  std::unique_ptr<platform::UdpSocket> peer_socket_;
};

TEST_F(CloudDatacenterPick, TakesTheUsersPickAndRejectsUnknownNames) {
  auto session = make(std::chrono::seconds{30});
  ASSERT_NE(session, nullptr);
  const auto early = session->select_datacenter("dc2");
  ASSERT_FALSE(early);
  EXPECT_EQ(early.error(), StreamError::invalid_state);

  std::atomic<bool> unknown_rejected{};
  auto picker = LiveProviders::spawn("picker", [&] {
    sink.wait_for_offer();
    const auto unknown = session->select_datacenter("nowhere");
    unknown_rejected =
        !unknown && unknown.error() == StreamError::invalid_configuration;
    LiveProviders::nap(std::chrono::milliseconds{30});
    EXPECT_TRUE(session->select_datacenter("dc2"));
  });
  const auto started_at = LiveProviders::steady_us();
  const auto started = session->start();
  static_cast<void>(picker.join());
  EXPECT_TRUE(started);
  EXPECT_LT(LiveProviders::steady_us() - started_at, 25'000'000U);
  EXPECT_TRUE(unknown_rejected);
  EXPECT_EQ(session->state(), remote_play::StreamState::negotiating_stream);

  {
    const platform::Lock lock(sink.mutex);
    ASSERT_EQ(sink.datacenters.size(), 2U);
    EXPECT_EQ(sink.datacenters[0].name, "dc1");
    EXPECT_EQ(sink.datacenters[1].name, "dc2");
    EXPECT_TRUE(
        sink.progressed("Waiting for Datacenter Selection - Step 9 of 10"));
    EXPECT_TRUE(sink.progressed("Selecting Datacenter (dc2) - Step 9 of 10"));
    ASSERT_EQ(sink.selections.size(), 1U);
    EXPECT_EQ(sink.selections[0].name, "dc2");
    EXPECT_TRUE(sink.selections[0].forced);
    EXPECT_NE(http.select_body.find("\"dc2\""), std::string::npos);
    EXPECT_EQ(http.select_body.find("\"dc1\""), std::string::npos);
    EXPECT_NE(http.allocate_body.find("\"dataCenter\":\"dc2\""),
              std::string::npos);
  }
  const auto late = session->select_datacenter("dc1");
  ASSERT_FALSE(late);
  EXPECT_EQ(late.error(), StreamError::invalid_state);
  EXPECT_TRUE(session->stop());
}

TEST_F(CloudDatacenterPick, MakesNoForcedSelectionWhenAutomaticIsPicked) {
  auto session = make(std::chrono::seconds{30});
  ASSERT_NE(session, nullptr);
  auto picker = LiveProviders::spawn("picker", [&] {
    sink.wait_for_offer();
    EXPECT_TRUE(session->select_datacenter("Auto"));
  });
  const auto started = session->start();
  static_cast<void>(picker.join());
  EXPECT_FALSE(started);
  const platform::Lock lock(sink.mutex);
  EXPECT_TRUE(sink.selections.empty());
  EXPECT_EQ(sink.failed_events, 1U);
  ASSERT_TRUE(session->failure());
  EXPECT_EQ(session->failure()->describe(), unreachable);
  EXPECT_TRUE(http.select_body.empty());
}

TEST_F(CloudDatacenterPick, FallsBackToTheAutomaticChoiceAfterTheTimeout) {
  auto session = make(std::chrono::milliseconds{150});
  ASSERT_NE(session, nullptr);
  const auto started_at = LiveProviders::steady_us();
  const auto started = session->start();
  const auto elapsed = LiveProviders::steady_us() - started_at;
  EXPECT_FALSE(started);
  EXPECT_GE(elapsed, 150'000U);
  const platform::Lock lock(sink.mutex);
  EXPECT_TRUE(sink.offered);
  EXPECT_TRUE(sink.selections.empty());
  EXPECT_EQ(sink.failed_events, 1U);
  ASSERT_TRUE(session->failure());
  EXPECT_EQ(session->failure()->describe(), unreachable);
  EXPECT_TRUE(http.select_body.empty());
  EXPECT_TRUE(http.allocate_body.empty());
}

TEST_F(CloudDatacenterPick, AbortsCleanlyWhenCancelledWhileWaiting) {
  auto session = make(std::chrono::seconds{30});
  ASSERT_NE(session, nullptr);
  auto canceller = LiveProviders::spawn("canceller", [&] {
    sink.wait_for_offer();
    LiveProviders::nap(std::chrono::milliseconds{30});
    stop->stopped.store(true, std::memory_order_release);
    session->wake();
  });
  const auto started_at = LiveProviders::steady_us();
  const auto started = session->start();
  static_cast<void>(canceller.join());
  EXPECT_LT(LiveProviders::steady_us() - started_at, 25'000'000U);
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error(), StreamError::route_failed);
  EXPECT_EQ(session->state(), remote_play::StreamState::failed);
  {
    const platform::Lock lock(sink.mutex);
    EXPECT_TRUE(sink.selections.empty());
    EXPECT_EQ(sink.failed_events, 1U);
    ASSERT_TRUE(session->failure());
    EXPECT_TRUE(session->failure()->cancelled());
    EXPECT_TRUE(http.select_body.empty());
    EXPECT_TRUE(http.allocate_body.empty());
  }
  const auto late = session->select_datacenter("dc1");
  ASSERT_FALSE(late);
  EXPECT_EQ(late.error(), StreamError::invalid_state);
}

}  // namespace
}  // namespace play_test
