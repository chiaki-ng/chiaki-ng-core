#include "libchiaking/platform/net/default.hpp"
#include "src/core/net/transport/datagram/udp_transport.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_accept.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/holepunch_accept_rig.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/holepunch_psn_rig.hpp"
#include "tests/support/live_providers.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <utility>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace internet = libchiaking::play::internet;
namespace pf = libchiaking::platform;
namespace datagram = libchiaking::net::transport::datagram;
using holepunch::PsnRig;
using holepunch::SelectedAccept;
using TransportErrorKind = internet::holepunch::CtrlAcceptTransportErrorKind;

class HolepunchAcceptTransport : public ::testing::Test {
 protected:
  void SetUp() override {
    auto receiver = datagram::ProviderUdpTransport::create(
        network_,
        datagram::UdpTransportConfig{{pf::IpAddress::loopback_ipv4(), 0}});
    ASSERT_TRUE(receiver);
    receiver_ = std::move(receiver.value());
    const auto peer = receiver_->local_endpoint();
    ASSERT_TRUE(peer);
    peer_ = peer.value();
  }

  auto transport(nat::SelectedCandidate selected) {
    return internet::holepunch::Accept::transport_ctrl(
        network_, SelectedAccept::offer(), SelectedAccept::console(),
        std::move(selected), 7, 9, psn_.session, psn_.push, psn_.connection,
        PsnRig::token, PsnRig::session_id, PsnRig::account_id, PsnRig::uid,
        PsnRig::family, {}, psn_.clock, PsnRig::deadline);
  }

  pf::DefaultNetworkProvider network_{test_support::LiveProviders::threading(), test_support::LiveProviders::steady()};
  std::unique_ptr<datagram::ProviderUdpTransport> receiver_;
  pf::SocketEndpoint peer_{};
  PsnRig psn_;
};

TEST_F(HolepunchAcceptTransport, HandsTheAcceptedSocketToTheControlRoute) {
  psn_.connection.push(SelectedAccept::accept_json);
  auto selected = SelectedAccept::selected(network_);
  selected.peer = peer_;
  auto route = transport(std::move(selected));
  ASSERT_TRUE(route);
  ASSERT_TRUE(route.value().transport);
  auto adopted = route.value().transport->local_endpoint();
  ASSERT_TRUE(adopted);
  EXPECT_EQ(adopted.value(), route.value().local_endpoint);
  EXPECT_EQ(route.value().registration_local_address,
            SelectedAccept::offer().local.addr);
  EXPECT_EQ(psn_.http.bodies().size(), 2U);
  EXPECT_EQ(psn_.connection.calls(), 1U);
}

TEST_F(HolepunchAcceptTransport, ReportsTheExchangeWhenNoAcceptArrives) {
  auto route = transport(SelectedAccept::selected(network_));
  ASSERT_FALSE(route);
  EXPECT_EQ(route.error().kind(), TransportErrorKind::exchange);
  EXPECT_EQ(route.error().exchange()->kind(),
            internet::holepunch::SelectedAcceptExchangeErrorKind::wait_failed);
  EXPECT_EQ(psn_.http.bodies().size(), 1U);
}

TEST_F(HolepunchAcceptTransport, ReportsTheHandoffForAPeerWithoutAnAddress) {
  psn_.connection.push(SelectedAccept::accept_json);
  auto selected = SelectedAccept::selected(network_);
  selected.peer = {pf::IpAddress::any_ipv4(), 9295};
  auto route = transport(std::move(selected));
  ASSERT_FALSE(route);
  EXPECT_EQ(route.error().kind(), TransportErrorKind::handoff);
  EXPECT_EQ(route.error().handoff(),
            internet::holepunch::CtrlRudpHandoffError::invalid_peer);
}

}  // namespace
}  // namespace play_test
