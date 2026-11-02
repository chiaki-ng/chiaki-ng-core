#include "libchiaking/platform/net/default.hpp"
#include "src/core/net/nat/holepunch/holepunch_codec.hpp"
#include "src/core/net/nat/holepunch/holepunch_probe.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_accept.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/holepunch_accept_rig.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/holepunch_psn_rig.hpp"
#include "tests/support/live_providers.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace internet = libchiaking::play::internet;
namespace pf = libchiaking::platform;
using holepunch::PsnRig;
using holepunch::SelectedAccept;
using ExchangeErrorKind = internet::holepunch::SelectedAcceptExchangeErrorKind;

class HolepunchAcceptExchange : public ::testing::Test {
 protected:
  auto exchange(nat::SelectedCandidate selected, std::uint64_t local_req_id = 9,
                const nat::ConnRequest& console = SelectedAccept::console()) {
    return internet::holepunch::Accept::run_selected_exchange(
        network_, SelectedAccept::offer(), console, std::move(selected), 7,
        local_req_id, psn_.session, psn_.push, psn_.connection, PsnRig::token,
        PsnRig::session_id, PsnRig::account_id, PsnRig::uid, PsnRig::family, {},
        psn_.clock, PsnRig::deadline);
  }

  pf::DefaultNetworkProvider network_{test_support::LiveProviders::threading(), test_support::LiveProviders::steady()};
  PsnRig psn_;
};

TEST_F(HolepunchAcceptExchange,
       AnswersTheConsolesProbeAfterTheAcknowledgement) {
  psn_.connection.push(SelectedAccept::accept_json);
  auto peer =
      network_.sockets().create_udp_socket(pf::IpAddress::loopback_ipv4());
  ASSERT_TRUE(peer);
  ASSERT_TRUE(peer.value()->bind({pf::IpAddress::loopback_ipv4(), 0}));
  auto selected = SelectedAccept::selected(network_);
  ASSERT_TRUE(selected.socket);
  selected.peer = peer.value()->local_endpoint().value();
  const auto client = selected.socket->local_endpoint().value();
  nat::CandidateProbe request{};
  request.request_id = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                        std::byte{5}};
  auto packet = nat::ProbeCodec::encode(request);
  psn_.http.after_second_post(
      [&] { EXPECT_TRUE(peer.value()->send_to(packet, client)); });
  auto result = exchange(std::move(selected));
  EXPECT_TRUE(result);
  std::array<std::byte, 89> response{};
  auto received = peer.value()->receive_from(response);
  ASSERT_TRUE(received);
  EXPECT_EQ(received.value().size, 88U);
  EXPECT_EQ(
      nat::ProbeCodec::classify(std::span{response.data(), std::size_t{88}},
                                request.request_id),
      nat::ProbeDatagramKind::matching_response);
  EXPECT_EQ(response[0], std::byte{7});
}

TEST_F(HolepunchAcceptExchange, KeepsTheSelectedSocketOnceReady) {
  psn_.connection.push(SelectedAccept::accept_json);
  auto selected = SelectedAccept::selected(network_);
  auto* original = selected.socket.get();
  auto result = exchange(std::move(selected));
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().selected.socket.get(), original);
  EXPECT_EQ(result.value().ready.state(),
            internet::holepunch::AcceptWaitState::ready);
  ASSERT_EQ(psn_.http.bodies().size(), 2U);
  EXPECT_EQ(psn_.connection.calls(), 1U);
  EXPECT_EQ(psn_.http.bodies()[1],
            PsnRig::envelope(
                nat::MessageCodec::format_short(nat::Action::result, 9, 0)));
}

TEST_F(HolepunchAcceptExchange,
       FailsToBuildForACandidateTheConsoleNeverOffered) {
  auto selected = SelectedAccept::selected(network_);
  selected.candidate_index = 2;
  auto result = exchange(std::move(selected));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), ExchangeErrorKind::build_failed);
  EXPECT_TRUE(psn_.http.bodies().empty());
  EXPECT_EQ(psn_.connection.calls(), 0U);
}

TEST_F(HolepunchAcceptExchange, FailsToSendWithAZeroRequestId) {
  auto result = exchange(SelectedAccept::selected(network_), 0);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), ExchangeErrorKind::send_failed);
  EXPECT_TRUE(psn_.http.bodies().empty());
  EXPECT_EQ(psn_.connection.calls(), 0U);
}

TEST_F(HolepunchAcceptExchange, FailsTheWaitWhenThePushFails) {
  psn_.connection.fail();
  auto result = exchange(SelectedAccept::selected(network_));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), ExchangeErrorKind::wait_failed);
  EXPECT_EQ(psn_.http.bodies().size(), 1U);
  EXPECT_EQ(psn_.connection.calls(), 1U);
}

}  // namespace
}  // namespace play_test
