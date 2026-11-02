#pragma once

#include "src/core/net/nat/holepunch/holepunch_conn_request.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_accept.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/holepunch_psn_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <libchiaking/platform/net/provider.hpp>
#include <string_view>
#include <utility>

namespace play_test::holepunch {

class SelectedAccept {
 public:
  static constexpr std::string_view accept_json =
      R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"ACCEPT\",\"reqId\":9,\"error\":0,\"connRequest\":{}}"}}}})";

  static auto offer() -> libchiaking::play::internet::holepunch::PreparedOffer {
    namespace nat = libchiaking::net::nat::holepunch;
    libchiaking::play::internet::holepunch::PreparedOffer ours{};
    ours.local = {nat::CandidateType::local, "192.168.1.42", "0.0.0.0", 54321,
                  0};
    ours.request.sid = 4660;
    ours.request.candidates = {
        {nat::CandidateType::static_, "203.0.113.7", "0.0.0.0", 54321, 0},
        ours.local};
    return ours;
  }

  static auto console() -> libchiaking::net::nat::holepunch::ConnRequest {
    namespace nat = libchiaking::net::nat::holepunch;
    nat::ConnRequest console{};
    console.sid = 22136;
    console.candidates = {
        {nat::CandidateType::local, "10.0.0.5", "0.0.0.0", 33333, 0}};
    return console;
  }

  static auto selected(
      pf::NetworkProvider& network,
      pf::SocketEndpoint peer = {pf::IpAddress::loopback_ipv4(), 9295})
      -> libchiaking::net::nat::holepunch::SelectedCandidate {
    auto socket =
        network.sockets().create_udp_socket(pf::IpAddress::loopback_ipv4());
    EXPECT_TRUE(socket.has_value());
    if (!socket) return {};
    EXPECT_TRUE(
        socket.value()->bind({pf::IpAddress::loopback_ipv4(), 0}).has_value());
    return {std::move(socket.value()), peer, 0};
  }
};

}  // namespace play_test::holepunch
