#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/loopback_transport.hpp"
#include "tests/unit/core/kakusan/adapters/shared_session/shared_session_core_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

namespace kakusan_test::session_core {
namespace {

class SharedSessionCoreCongestion : public SharedSessionCoreTest {};

TEST_F(SharedSessionCoreCongestion, FailsAStalledGuestAndSparesTheOthers) {
  CoreRig rig;
  ASSERT_TRUE(rig.session);
  ASSERT_NO_FATAL_FAILURE(rig.admit(0xc1, 1));
  const auto stalled = rig.party;
  ASSERT_NO_FATAL_FAILURE(rig.admit(0xc2, 2));
  const auto healthy = rig.party;
  stalled.guest->wire_link->drop = [](TransportRole role,
                                      test_support::kakusan::Lane lane,
                                      const std::vector<std::byte>& bytes) {
    return role == TransportRole::guest &&
           lane == test_support::kakusan::Lane::record && is_media_ack(bytes);
  };
  const auto healthy_before = healthy.guest->packets.size();
  for (std::size_t index = 0;
       index < 10000 && rig.host_state(stalled.id) != Progress::gone; ++index) {
    if (index % 8 == 0) rig.send_video(1, 600, 0);
    rig.step();
  }
  const auto state = rig.last_state(stalled.id);
  ASSERT_TRUE(state);
  EXPECT_EQ(state->state, Progress::gone);
  EXPECT_EQ(state->error, Error::congestion);
  rig.run(50);
  EXPECT_EQ(stalled.guest->error, Error::congestion);
  EXPECT_TRUE(stalled.guest->by_peer);
  EXPECT_EQ(rig.host_state(healthy.id), Progress::playing);
  EXPECT_EQ(healthy.guest->state(), Progress::playing);
  EXPECT_GT(healthy.guest->packets.size(), healthy_before + 100);
  const auto healthy_mid = healthy.guest->packets.size();
  rig.send_video(10, 600, 0);
  rig.run(200);
  EXPECT_EQ(healthy.guest->packets.size(), healthy_mid + 10);
  rig.check_packets(*healthy.guest);
  EXPECT_TRUE(rig.removals(healthy.id).empty());
}

}  // namespace
}  // namespace kakusan_test::session_core
