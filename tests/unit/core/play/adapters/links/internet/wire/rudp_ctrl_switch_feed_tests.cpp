#include "src/core/net/protocol/remote_play/rudp_switch.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

namespace play_test {
namespace {

namespace transport_switch =
    libchiaking::net::protocol::remote_play::transport_switch_detail;

TEST(RemoteTransportSwitch, ReleasesTheTransportOnlyOnceTheAckIsSent) {
  transport_switch::RemoteTransportSwitch state;
  EXPECT_TRUE(state.begin(7));
  EXPECT_EQ(state.state(), transport_switch::State::waiting);
  EXPECT_FALSE(state.take_ready());
  EXPECT_TRUE(state.on_ack(7));
  EXPECT_TRUE(state.take_ready());
  EXPECT_FALSE(state.on_ack(7));
}

}  // namespace
}  // namespace play_test
