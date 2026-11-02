#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/net/transport/datagram/loopback_rig.hpp"

#include "src/core/net/transport/rudp/rudp.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rudp_test {
namespace {

namespace rudp = libchiaking::net::transport::rudp;
namespace pf = libchiaking::platform;
using libchiaking::net::transport::datagram::TransportError;

class RudpProviderHandshakeChannel : public datagram_test::UdpLoopbackRig {
 protected:
  void SetUp() override { ASSERT_NO_FATAL_FAILURE(open_pair()); }
};

TEST_F(RudpProviderHandshakeChannel, FindsNothingOnAnIdleSocketWithNoTimeout) {
  rudp::ProviderRudpHandshakeChannel channel{*first};
  const auto timeout = channel.receive_for(std::chrono::microseconds{0}, stop);
  ASSERT_TRUE(timeout);
  EXPECT_FALSE(timeout.value().has_value());
}

TEST_F(RudpProviderHandshakeChannel, CarriesADatagramBetweenTwoAdapters) {
  {
    rudp::ProviderRudpHandshakeChannel sender{*second};
    const Bytes payload{std::byte{1}, std::byte{2}};
    const auto sent = sender.send(payload);
    ASSERT_TRUE(sent);
    EXPECT_EQ(sent.value(), payload.size());
  }
  rudp::ProviderRudpHandshakeChannel receiver{*first};
  const auto received =
      receiver.receive_for(std::chrono::microseconds{500000}, stop);
  ASSERT_TRUE(received);
  ASSERT_TRUE(received.value().has_value());
  EXPECT_EQ(received.value().value(), (Bytes{std::byte{1}, std::byte{2}}));
}

TEST_F(RudpProviderHandshakeChannel, LeavesAQueuedDatagramWhenStopped) {
  const pf::StopToken stopped{std::make_shared<const test_support::Stopped>()};
  const Bytes queued{std::byte{3}};
  ASSERT_TRUE(second->send(queued));
  {
    rudp::ProviderRudpHandshakeChannel channel{*first};
    const auto blocked =
        channel.receive_for(std::chrono::microseconds{5000000}, stopped);
    ASSERT_TRUE(blocked);
    EXPECT_FALSE(blocked.value().has_value());
  }
  ASSERT_TRUE(ready(*first, stop));
  const auto raw = first->receive();
  ASSERT_TRUE(raw);
  ASSERT_TRUE(raw.value().has_value());
  EXPECT_TRUE(holds(raw.value().value(), queued));
}

TEST_F(RudpProviderHandshakeChannel, LeavesTheBorrowedTransportsWorking) {
  {
    rudp::ProviderRudpHandshakeChannel borrowed_first{*first};
    rudp::ProviderRudpHandshakeChannel borrowed_second{*second};
  }
  EXPECT_TRUE(delivers(*second, *first, Bytes{std::byte{9}}));
}

TEST_F(RudpProviderHandshakeChannel, ReportsAClosedTransport) {
  ASSERT_TRUE(second->close());
  rudp::ProviderRudpHandshakeChannel sender{*second};
  const auto sent = sender.send(Bytes{std::byte{4}});
  ASSERT_FALSE(sent);
  EXPECT_EQ(sent.error(), TransportError::closed);
  ASSERT_TRUE(first->close());
  rudp::ProviderRudpHandshakeChannel receiver{*first};
  const auto received =
      receiver.receive_for(std::chrono::microseconds{0}, stop);
  ASSERT_FALSE(received);
  EXPECT_EQ(received.error(), TransportError::closed);
}

class RudpDatagramDrain : public datagram_test::UdpLoopbackRig {};

TEST_F(RudpDatagramDrain, DiscardsLateDatagramsUntilTheLineGoesQuiet) {
  ASSERT_NO_FATAL_FAILURE(open_pair());
  const Bytes late(88, std::byte{6});
  for (int index = 0; index < 3; ++index) ASSERT_TRUE(second->send(late));
  const auto started = factory.clock().now_microseconds();
  const auto drained = rudp::RemoteDatagramDrain::drain(
      *first, factory.network(), factory.clock(), {});
  const auto elapsed = std::chrono::microseconds{
      static_cast<std::int64_t>(factory.clock().now_microseconds() - started)};
  ASSERT_TRUE(drained);
  EXPECT_EQ(drained.value(), 3U);
  EXPECT_GE(elapsed, std::chrono::milliseconds{190});
  EXPECT_LE(elapsed, std::chrono::milliseconds{900});
  const auto empty = first->receive();
  ASSERT_TRUE(empty);
  EXPECT_FALSE(empty.value().has_value());
}

}  // namespace
}  // namespace rudp_test
