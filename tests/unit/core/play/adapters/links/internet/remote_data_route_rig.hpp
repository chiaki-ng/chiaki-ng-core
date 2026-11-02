#pragma once

#include "libchiaking/platform/net/default.hpp"
#include "src/core/net/transport/datagram/udp_transport.hpp"
#include "src/core/play/adapters/links/internet/internet_start.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/live_providers.hpp"
#include "tests/support/random.hpp"
#include "tests/support/websocket.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test::data_route_rig {

namespace holepunch = libchiaking::net::nat::holepunch;
namespace internet = libchiaking::play::internet;
namespace platform = libchiaking::platform;
namespace psn_remote_play = libchiaking::psn::remote_play;

class DataRouteRig : public ::testing::Test {
 protected:
  using InternetStart = libchiaking::play::InternetStart;

  static auto bound_socket(platform::NetworkProvider& network)
      -> std::unique_ptr<platform::UdpSocket> {
    auto socket = network.sockets().create_udp_socket(
        platform::IpAddress::loopback_ipv4());
    EXPECT_TRUE(socket.has_value());
    if (!socket) return nullptr;
    EXPECT_TRUE(socket.value()
                    ->bind({platform::IpAddress::loopback_ipv4(), 0})
                    .has_value());
    return std::move(socket.value());
  }

  static auto selected(platform::NetworkProvider& network,
                       platform::SocketEndpoint peer)
      -> holepunch::SelectedCandidate {
    return {bound_socket(network), peer, 0};
  }

  auto receiver_endpoint() -> std::optional<platform::SocketEndpoint> {
    auto receiver =
        libchiaking::net::transport::datagram::ProviderUdpTransport::create(
            network, libchiaking::net::transport::datagram::UdpTransportConfig{
                         {platform::IpAddress::loopback_ipv4(), 0}});
    EXPECT_TRUE(receiver.has_value());
    if (!receiver) return std::nullopt;
    const auto endpoint = receiver.value()->local_endpoint();
    EXPECT_TRUE(endpoint.has_value());
    receivers_.push_back(std::move(receiver.value()));
    if (!endpoint) return std::nullopt;
    return endpoint.value();
  }

  platform::DefaultNetworkProvider network{
      test_support::LiveProviders::threading(),
      test_support::LiveProviders::steady()};

 private:
  std::vector<std::unique_ptr<
      libchiaking::net::transport::datagram::ProviderUdpTransport>>
      receivers_;
};

class QueuedConnection final : public platform::WebSocketConnection {
 public:
  void add(std::string_view text) {
    platform::WebSocketMessage message{};
    message.payload.assign(text);
    messages_.push_back(std::move(message));
  }

  auto receive(const platform::StopToken& stop)
      -> libchiaking::Expected<platform::WebSocketMessage,
                               platform::WebSocketError> override {
    EXPECT_FALSE(stop.stop_requested());
    if (calls_ >= messages_.size())
      return libchiaking::unexpected(platform::WebSocketError::backend_failure);
    return std::move(messages_[calls_++]);
  }

  auto close()
      -> libchiaking::Expected<void, platform::WebSocketError> override {
    return {};
  }

  auto calls() const -> std::size_t { return calls_; }

 private:
  std::vector<platform::WebSocketMessage> messages_;
  std::size_t calls_{};
};

class RecordingHttp final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request,
               const platform::StopToken& stop)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    EXPECT_FALSE(stop.stop_requested());
    EXPECT_EQ(request.method, platform::HttpMethod::post);
    EXPECT_NE(request.url.view().find("/sessionMessage"), std::string::npos);
    EXPECT_FALSE(request.headers.empty());
    if (!request.headers.empty()) {
      EXPECT_EQ(request.headers.front().value.view(),
                "Bearer SYNTHETIC_TEST_TOKEN");
    }
    bodies_.emplace_back(request.body.view());
    platform::HttpResponse response{};
    response.status_code = 204;
    return response;
  }

  auto bodies() const -> const std::vector<std::string>& { return bodies_; }

 private:
  std::vector<std::string> bodies_;
};

class AcceptRig : public DataRouteRig {
 protected:
  static inline const libchiaking::psn::auth::BearerToken token{
      std::string_view{"SYNTHETIC_TEST_TOKEN"}};
  static constexpr std::string_view session_id =
      "01234567-89ab-cdef-0123-456789abcdef";
  static constexpr std::array<std::byte, 32> uid{};
  static constexpr std::string_view accept_json =
      R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"ACCEPT\",\"reqId\":9,\"error\":0,\"connRequest\":{}}"}}}})";

  static auto offer() -> internet::holepunch::PreparedOffer {
    internet::holepunch::PreparedOffer ours{};
    ours.local = {holepunch::CandidateType::local, "192.168.1.42", "0.0.0.0",
                  54321, 0};
    ours.request.sid = 4660;
    ours.request.candidates = {
        {holepunch::CandidateType::static_, "203.0.113.7", "0.0.0.0", 54321, 0},
        ours.local};
    return ours;
  }

  static auto console() -> holepunch::ConnRequest {
    holepunch::ConnRequest console{};
    console.sid = 22136;
    console.candidates = {
        {holepunch::CandidateType::local, "10.0.0.5", "0.0.0.0", 33333, 0}};
    return console;
  }

  auto accept(holepunch::SelectedCandidate chosen) {
    return InternetStart::accept_data_route(
        network, offer(), console(), std::move(chosen), 7, 9, session, push,
        connection, token, session_id, 12, uid,
        libchiaking::psn::rest::ConsoleFamily::ps5, {}, clock, 10000);
  }

  test_support::FixedRandom random{std::byte{}};
  RecordingHttp http;
  test_support::RefusingWebSocket websocket{
      platform::WebSocketError::backend_failure};
  QueuedConnection connection;
  test_support::FakeClock clock{100};
  psn_remote_play::SessionClient session{
      http, test_support::LiveProviders::steady(), random};
  psn_remote_play::PushClient push{websocket};
};

}  // namespace play_test::data_route_rig
