#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/remote_control_route_rig.hpp"

#include <gtest/gtest.h>

#include <string>

namespace play_test {
namespace {

namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
using remote_route_rig::Messages;

class RemoteDataRouteHolepunch : public remote_route_rig::RouteRig {
 protected:
  void SetUp() override { secrets.local_sid = 1; }
};

TEST_F(RemoteDataRouteHolepunch, AdoptsTheProbedSocketOnceTheConsoleAccepts) {
  const auto offer = Messages::replaced(Messages::offer_with_candidate(),
                                        R"(\"reqId\":2)", R"(\"reqId\":4)");
  const auto result =
      Messages::replaced(Messages::result, R"(\"reqId\":1)", R"(\"reqId\":3)");
  const auto accept =
      Messages::replaced(Messages::accept, R"(\"reqId\":2)", R"(\"reqId\":4)");
  ASSERT_NO_FATAL_FAILURE(open({offer, result, accept}));
  auto established = InternetStart::establish_data_route(
      routed, session, push, *started, secrets, 3, 4, {}, 20000, false,
      libchiaking::psn::rest::ConsoleFamily::ps5, false, 75U, 250U, nullptr,
      nullptr, 9);
  ASSERT_TRUE(established);
  ASSERT_TRUE(established.value().transport);
  EXPECT_EQ(established.value().kind, remote_play::RouteKind::remote_rudp);
  EXPECT_EQ(
      established.value().local_endpoint,
      (platform::SocketEndpoint{platform::IpAddress::loopback_ipv4(), 40000}));
  EXPECT_EQ(
      established.value().remote_endpoint,
      (platform::SocketEndpoint{platform::IpAddress::loopback_ipv4(), 9999}));
  EXPECT_EQ(connection().receive_calls(), 3U);
  EXPECT_GE(sockets().probe_sends(), 1U);
  EXPECT_EQ(sockets().creates(), 1U);
  EXPECT_EQ(sockets().closes(), 0U);
  EXPECT_EQ(http.psn_bodies.size(), 4U);
  EXPECT_TRUE(body_has(0, "RESULT"));
  EXPECT_TRUE(body_has(1, "OFFER"));
  EXPECT_TRUE(body_has(1, R"(\"peerSid\":9,)"));
  EXPECT_TRUE(body_has(2, "ACCEPT"));
  EXPECT_TRUE(body_has(3, "RESULT"));
  established.value().transport.reset();
  EXPECT_EQ(sockets().closes(), 1U);
  EXPECT_FALSE(started->opened.lease().release_attempted());
  EXPECT_TRUE(started->opened.lease().release({}));
}

}  // namespace
}  // namespace play_test
