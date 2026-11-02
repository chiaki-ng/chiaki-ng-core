#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_case.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

namespace play_test {

using test_support::play::SessionRig;
namespace {

namespace play = libchiaking::play;

using PlaySessionMicrophone = SessionCase;

TEST_F(PlaySessionMicrophone, ConnectsAndSendsFrames) {
  SessionRig rig;
  rig.start_playing();
  auto& microphone = rig.session->microphone();
  EXPECT_FALSE(microphone.latest());
  EXPECT_TRUE(microphone.open());
  const auto ticket = microphone.connect(77);
  ASSERT_TRUE(ticket);
  EXPECT_EQ(ticket.value().tag(), 77U);
  EXPECT_EQ(microphone.latest(), ticket.value());
  SessionRig::eventually([&] {
    return microphone.state(ticket.value()) ==
           play::Microphone::ConnectState::accepted;
  });
  EXPECT_EQ(rig.link.count("microphone_connect"), 1U);
  EXPECT_EQ(microphone.state(
                play::Microphone::ConnectTicket{ticket.value().id() + 9, 77}),
            play::Microphone::ConnectState::rejected);
  EXPECT_TRUE(microphone.set_muted(true));
  SessionRig::eventually(
      [&] { return rig.link.count("microphone_mute") == 1; });
  std::array<std::byte, play::Microphone::frame_size> frame{};
  EXPECT_TRUE(microphone.set_muted(false));
  SessionRig::eventually(
      [&] { return rig.link.count("microphone_unmute") == 1; });
  EXPECT_TRUE(microphone.push(frame));
  SessionRig::eventually(
      [&] { return microphone.statistics().frames_sent == 1; });
  EXPECT_EQ(rig.link.count("microphone_frame"), 1U);
  const auto statistics = microphone.statistics();
  EXPECT_EQ(statistics.frames_queued, 1U);
  EXPECT_EQ(statistics.frames_sent, 1U);
  EXPECT_FALSE(statistics.last_error);
  EXPECT_EQ(rig.session->statistics().microphone, statistics);
}

}  // namespace
}  // namespace play_test
