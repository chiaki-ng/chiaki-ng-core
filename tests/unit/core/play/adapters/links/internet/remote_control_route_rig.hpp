#pragma once

#include "libchiaking/platform/default_factory.hpp"
#include "src/core/net/nat/holepunch/holepunch_probe.hpp"
#include "src/core/play/adapters/links/internet/internet_start.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/random.hpp"
#include "tests/support/websocket.hpp"
#include "tests/support/live_providers.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test::remote_route_rig {

namespace holepunch = libchiaking::net::nat::holepunch;
namespace platform = libchiaking::platform;
namespace psn_remote_play = libchiaking::psn::remote_play;

class Messages {
 public:
  static constexpr std::string_view offer =
      R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"OFFER\",\"reqId\":2,\"error\":0,\"connRequest\":{\"sid\":3,\"peerSid\":4,\"skey\":\"AAAAAAAAAAAAAAAAAAAAAA==\",\"natType\":2,\"candidate\":[],\"defaultRouteMacAddr\":\"\",\"localPeerAddr\":,\"localHashedId\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAA=\"}}"}}}})";
  static constexpr std::string_view result =
      R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"RESULT\",\"reqId\":1,\"error\":0,\"connRequest\":{}}"}}}})";
  static constexpr std::string_view accept =
      R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"ACCEPT\",\"reqId\":2,\"error\":0,\"connRequest\":{}}"}}}})";

  static auto offer_with_candidate() -> std::string {
    return replaced(
        offer, R"(\"candidate\":[])",
        R"(\"candidate\":[{\"type\":\"LOCAL\",\"addr\":\"127.0.0.1\",\"mappedAddr\":\"0.0.0.0\",\"port\":9999,\"mappedPort\":0}])");
  }

  static auto replaced(std::string_view message, std::string_view from,
                       std::string_view to) -> std::string {
    std::string text{message};
    const auto at = text.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
  }
};

class CreateHttp final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    if (request.method == platform::HttpMethod::post &&
        request.url.view().find("/sessionMessage") != std::string::npos) {
      psn_bodies.emplace_back(request.body.view());
      platform::HttpResponse accepted{};
      accepted.status_code = 204;
      return accepted;
    }
    constexpr std::string_view created =
        R"({"remotePlaySessions":[{"sessionId":"01234567-89ab-cdef-0123-456789abcdef","members":[{"accountId":"123456789"}]}]})";
    constexpr std::string_view hosts = "preferred.example:9999\n";
    platform::HttpResponse response{};
    response.status_code = 200;
    response.body.assign(request.method == platform::HttpMethod::get ? hosts
                                                                     : created);
    return response;
  }

  std::vector<std::string> psn_bodies;
};

class QueuedConnection final : public platform::WebSocketConnection {
 public:
  explicit QueuedConnection(std::initializer_list<std::string_view> texts) {
    for (const auto text : texts) {
      platform::WebSocketMessage message{};
      message.payload.assign(text);
      messages_.push_back(std::move(message));
    }
  }

  auto receive(const platform::StopToken&)
      -> libchiaking::Expected<platform::WebSocketMessage,
                               platform::WebSocketError> override {
    ++receive_calls_;
    if (next_ >= messages_.size())
      return libchiaking::unexpected(platform::WebSocketError::backend_failure);
    return std::move(messages_[next_++]);
  }

  auto close()
      -> libchiaking::Expected<void, platform::WebSocketError> override {
    return {};
  }

  auto receive_calls() const -> unsigned { return receive_calls_; }

 private:
  std::vector<platform::WebSocketMessage> messages_;
  std::size_t next_{};
  unsigned receive_calls_{};
};

class RouteSocket final : public platform::UdpSocket {
 public:
  RouteSocket(platform::NetworkProvider& owner, unsigned& closes,
              unsigned& probe_sends)
      : owner_(owner),
        closes_(closes),
        probe_sends_(probe_sends),
        response_(stun_response_.begin(), stun_response_.end()) {}
  RouteSocket(const RouteSocket&) = delete;
  auto operator=(const RouteSocket&) -> RouteSocket& = delete;
  ~RouteSocket() override {
    if (!closed_) ++closes_;
  }

  auto owner() const noexcept -> const platform::NetworkProvider& override {
    return owner_;
  }
  auto bind(const platform::SocketEndpoint&)
      -> libchiaking::Expected<void, platform::NetError> override {
    bound_ = true;
    return {};
  }
  auto send_to(std::span<const std::byte> bytes,
               const platform::SocketEndpoint&)
      -> libchiaking::Expected<std::size_t, platform::NetError> override {
    if (bytes.size() == holepunch::CandidateProbe::kEncodedSize) {
      auto probe = holepunch::ProbeCodec::decode(bytes);
      if (probe) {
        auto reply =
            holepunch::ProbeCodec::encode_response(probe.value(), {{}, 9999});
        response_.assign(reply.begin(), reply.end());
        followup_.assign(bytes.begin(), bytes.end());
        pending_ = true;
        ++probe_sends_;
      }
    }
    return bytes.size();
  }
  auto receive_from(std::span<std::byte> buffer)
      -> libchiaking::Expected<platform::UdpReceiveResult,
                               platform::NetError> override {
    if (!pending_ || buffer.size() < response_.size())
      return libchiaking::unexpected(platform::NetError::system_failure);
    std::ranges::copy(response_, buffer.begin());
    const auto size = response_.size();
    response_ = std::move(followup_);
    followup_.clear();
    pending_ = !response_.empty();
    return platform::UdpReceiveResult{
        size, {platform::IpAddress::loopback_ipv4(), 9999}, std::nullopt};
  }
  auto set_boolean_option(platform::SocketOption, bool)
      -> libchiaking::Expected<void, platform::NetError> override {
    return {};
  }
  auto set_receive_buffer_size(std::size_t)
      -> libchiaking::Expected<void, platform::NetError> override {
    return {};
  }
  auto local_endpoint() -> libchiaking::Expected<platform::SocketEndpoint,
                                                 platform::NetError> override {
    if (!bound_)
      return libchiaking::unexpected(platform::NetError::system_failure);
    return platform::SocketEndpoint{platform::IpAddress::loopback_ipv4(),
                                    40000};
  }
  auto close() -> libchiaking::Expected<void, platform::NetError> override {
    if (!closed_) {
      closed_ = true;
      ++closes_;
    }
    return {};
  }

  auto pending() const -> bool { return pending_; }

 private:
  static constexpr std::array<std::byte, 32> stun_response_{
      std::byte{1},    std::byte{1},    std::byte{0},    std::byte{12},
      std::byte{0x21}, std::byte{0x12}, std::byte{0xa4}, std::byte{0x42},
      std::byte{},     std::byte{},     std::byte{},     std::byte{},
      std::byte{},     std::byte{},     std::byte{},     std::byte{},
      std::byte{},     std::byte{},     std::byte{},     std::byte{},
      std::byte{0},    std::byte{1},    std::byte{0},    std::byte{8},
      std::byte{0},    std::byte{1},    std::byte{0x9c}, std::byte{0x41},
      std::byte{203},  std::byte{0},    std::byte{113},  std::byte{7}};

  platform::NetworkProvider& owner_;
  unsigned& closes_;
  unsigned& probe_sends_;
  bool bound_{};
  bool closed_{};
  bool pending_{true};
  std::vector<std::byte> response_;
  std::vector<std::byte> followup_;
};

class RouteSockets final : public platform::SocketProvider {
 public:
  explicit RouteSockets(platform::NetworkProvider& owner) : owner_(owner) {}

  auto create_udp_socket(const platform::IpAddress&)
      -> libchiaking::Expected<std::unique_ptr<platform::UdpSocket>,
                               platform::NetError> override {
    auto socket = std::make_unique<RouteSocket>(owner_, closes_, probe_sends_);
    last_ = socket.get();
    ++creates_;
    return std::unique_ptr<platform::UdpSocket>{std::move(socket)};
  }
  auto create_tcp_socket(const platform::IpAddress&)
      -> libchiaking::Expected<std::unique_ptr<platform::TcpSocket>,
                               platform::NetError> override {
    return libchiaking::unexpected(platform::NetError::not_supported);
  }
  auto create_tcp_listener(const platform::IpAddress&)
      -> libchiaking::Expected<std::unique_ptr<platform::TcpListener>,
                               platform::NetError> override {
    return libchiaking::unexpected(platform::NetError::not_supported);
  }

  auto last() const -> const RouteSocket* { return last_; }
  auto creates() const -> unsigned { return creates_; }
  auto closes() const -> unsigned { return closes_; }
  auto probe_sends() const -> unsigned { return probe_sends_; }

 private:
  platform::NetworkProvider& owner_;
  RouteSocket* last_{};
  unsigned creates_{};
  unsigned closes_{};
  unsigned probe_sends_{};
};

class RoutePoller final : public platform::Poller {
 public:
  RoutePoller(platform::NetworkProvider& owner, const RouteSockets& sockets)
      : owner_(owner), sockets_(sockets) {}

  auto owner() const noexcept -> const platform::NetworkProvider& override {
    return owner_;
  }
  auto wake() -> libchiaking::Expected<void, platform::NetError> override {
    return {};
  }
  auto wait(std::span<const platform::PollRequest> requests,
            std::chrono::nanoseconds, const platform::StopToken&)
      -> libchiaking::Expected<platform::PollWaitResult,
                               platform::NetError> override {
    platform::PollWaitResult result{};
    const auto* last = sockets_.last();
    for (const auto& request : requests) {
      if (last && request.pollable == last && last->pending())
        result.ready.push_back({request.tag, platform::PollEvent::readable});
    }
    return result;
  }

 private:
  platform::NetworkProvider& owner_;
  const RouteSockets& sockets_;
};

class RoutePollers final : public platform::PollerProvider {
 public:
  RoutePollers(platform::NetworkProvider& owner, const RouteSockets& sockets)
      : owner_(owner), sockets_(sockets) {}

  auto create_poller()
      -> libchiaking::Expected<std::unique_ptr<platform::Poller>,
                               platform::NetError> override {
    return std::unique_ptr<platform::Poller>{
        std::make_unique<RoutePoller>(owner_, sockets_)};
  }

 private:
  platform::NetworkProvider& owner_;
  const RouteSockets& sockets_;
};

class RouteDns final : public platform::DnsProvider {
 public:
  auto resolve(std::string_view host, std::string_view)
      -> libchiaking::Expected<std::vector<platform::IpAddress>,
                               platform::NetError> override {
    if (host == "preferred.example" || host == "127.0.0.1")
      return std::vector{platform::IpAddress::loopback_ipv4()};
    return libchiaking::unexpected(platform::NetError::host_unreachable);
  }
};

class RouteInterfaces final : public platform::InterfacesProvider {
 public:
  auto enumerate()
      -> libchiaking::Expected<std::vector<platform::InterfaceInfo>,
                               platform::NetError> override {
    platform::InterfaceInfo info{};
    info.address = platform::IpAddress::ipv4({192, 168, 1, 42});
    info.operational = true;
    return std::vector{info};
  }
};

class RouteNetwork final : public platform::NetworkProvider {
 public:
  RouteNetwork() : sockets_(*this), pollers_(*this, sockets_) {}
  RouteNetwork(const RouteNetwork&) = delete;
  auto operator=(const RouteNetwork&) -> RouteNetwork& = delete;

  auto sockets() noexcept -> platform::SocketProvider& override {
    return sockets_;
  }
  auto pollers() noexcept -> platform::PollerProvider& override {
    return pollers_;
  }
  auto dns() noexcept -> platform::DnsProvider& override { return dns_; }
  auto interfaces() noexcept -> platform::InterfacesProvider& override {
    return interfaces_;
  }
  auto capabilities() const noexcept -> platform::NetworkCapabilities override {
    return {};
  }

  auto route_sockets() const -> const RouteSockets& { return sockets_; }

 private:
  RouteSockets sockets_;
  RoutePollers pollers_;
  RouteDns dns_;
  RouteInterfaces interfaces_;
};

class HeardRoutes final
    : public libchiaking::play::InternetStart::RouteWatcher {
 public:
  using Route = libchiaking::progress::InternetRoute;

  void on_route(const Route& route) override { heard.push_back(route); }

  std::vector<Route> heard;
};

class RouteRig : public ::testing::Test {
 protected:
  using InternetStart = libchiaking::play::InternetStart;

  void open(std::initializer_list<std::string_view> messages = {}) {
    auto lease = psn_remote_play::SessionLease::create(
        session, libchiaking::psn::auth::BearerToken{token}, {});
    ASSERT_TRUE(lease);
    auto connection = std::make_unique<QueuedConnection>(messages);
    connection_ = connection.get();
    started.emplace(InternetStart::AccountSession{
        psn_remote_play::StartedSession{
            std::move(lease.value()),
            psn_remote_play::PushConnection{push, std::move(connection)},
            "userA", 7},
        {}});
  }

  auto connection() const -> const QueuedConnection& { return *connection_; }
  auto body_has(std::size_t index, std::string_view text) const
      -> ::testing::AssertionResult {
    if (index >= http.psn_bodies.size())
      return ::testing::AssertionFailure()
             << "only " << http.psn_bodies.size() << " PSN bodies";
    if (http.psn_bodies[index].find(text) == std::string::npos)
      return ::testing::AssertionFailure()
             << "body " << index << " lacks " << text << ": "
             << http.psn_bodies[index];
    return ::testing::AssertionSuccess();
  }
  auto sockets() const -> const RouteSockets& {
    return network.route_sockets();
  }

  static constexpr std::string_view token = "SYNTHETIC_TEST_TOKEN";

  test_support::FakeClock clock{100};
  test_support::FixedRandom random{std::byte{}};
  CreateHttp http;
  test_support::RefusingWebSocket websocket{
      platform::WebSocketError::backend_failure};
  psn_remote_play::SessionClient session{http, test_support::LiveProviders::steady(), random};
  psn_remote_play::PushClient push{websocket};
  RouteNetwork network;
  platform::DefaultPlatformFactory without_http{
      platform::PlatformProviders{.clock = &clock, .random = &random}};
  platform::DefaultPlatformFactory with_http{platform::PlatformProviders{
      .clock = &clock, .random = &random, .http = &http}};
  platform::DefaultPlatformFactory routed{platform::PlatformProviders{
      .clock = &clock, .random = &random, .network = &network, .http = &http}};
  InternetStart::Secrets secrets{};
  std::optional<InternetStart::AccountSession> started;

 private:
  QueuedConnection* connection_{};
};

}  // namespace play_test::remote_route_rig
