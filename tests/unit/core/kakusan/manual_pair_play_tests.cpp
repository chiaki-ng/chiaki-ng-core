#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/manual_pair_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <variant>

namespace kakusan_test {

using test_support::play::SourceStream;
namespace {

using std::chrono::milliseconds;
namespace host_event = manual::host_event;
namespace guest_event = manual::guest_event;

class ManualPairGuest : public ManualWorldTest {
 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(ManualWorldTest::SetUp());
    ASSERT_NO_FATAL_FAILURE(world.start_source());
    ASSERT_NO_FATAL_FAILURE(world.join(0x40));
  }

  auto player() -> ManualPlayer& { return world.latest(); }

  auto join_seat() const -> manual::Seat {
    return world.side.find<host_event::JoinRequested>(world.players.back()->id)
        ->seat;
  }

  void pressed() {
    EXPECT_TRUE(player().guest->set_controller(ManualPair::pressed_state()));
    ASSERT_NO_FATAL_FAILURE(world.until([&] {
      const auto inputs = world.side.inputs();
      return !inputs.empty() && inputs.back().buttons != 0;
    }));
  }

  void left() {
    player().guest->leave();
    ASSERT_NO_FATAL_FAILURE(world.until(
        [&] { return world.side.state(player().id) == Progress::gone; }));
    ASSERT_NO_FATAL_FAILURE(world.until(
        [&] { return player().count<guest_event::StreamStopped>() > 0; }));
  }
};

TEST_F(ManualPairGuest, IsInvitedBoundApprovedAndJoinedInOrder) {
  auto& side = world.side;
  const auto& id = player().id;
  EXPECT_EQ(side.count<host_event::InvitationReady>(), 1U);
  EXPECT_LT(side.index_of<host_event::InvitationReady>(id),
            side.index_of<host_event::ReplyBound>(id));
  EXPECT_LT(side.index_of<host_event::ReplyBound>(id),
            side.index_of<host_event::CodeReady>(id));
  EXPECT_LT(side.index_of<host_event::CodeReady>(id),
            side.index_of<host_event::JoinRequested>(id));
  EXPECT_TRUE(side.monotonic(id));
  EXPECT_EQ(side.count<host_event::JoinRequested>(), 1U);
  const auto join = join_seat();
  EXPECT_EQ(side.count<host_event::IdrRequested>(), 1U);
  const auto* active = side.last_state(id);
  ASSERT_NE(active, nullptr);
  EXPECT_EQ(active->state, Progress::playing);
  EXPECT_EQ(active->seat, join);
  EXPECT_FALSE(active->error);
  EXPECT_FALSE(active->reason);

  EXPECT_EQ(world.host->seats(), (manual::Seats{4, 1, 1, 0}));
  EXPECT_TRUE(std::ranges::any_of(side.events, [](const auto& event) {
    const auto* changed = std::get_if<host_event::SeatsChanged>(&event);
    return changed != nullptr && changed->seats.remote_seats == 1;
  }));
}

TEST_F(ManualPairGuest, HearsTheStreamDescribedBeforeItsControllerIsAskedFor) {
  const auto join = join_seat();
  const auto* requested = player().find<guest_event::ControllerRequested>();
  ASSERT_NE(requested, nullptr);
  EXPECT_EQ(requested->seat, join);
  EXPECT_EQ(requested->capacity, 4U);
  EXPECT_EQ(requested->local_seats, 1U);
  EXPECT_EQ(requested->allowed_buttons, manual::all_buttons);
  const auto* ready = player().find<guest_event::StreamReady>();
  ASSERT_NE(ready, nullptr);
  EXPECT_EQ(ready->video_fps, SourceStream::video_fps);
  const auto info = std::ranges::find_if(
      player().streams,
      [](const auto& record) { return record.info.has_value(); });
  ASSERT_NE(info, player().streams.end());
  const auto expected = SourceStream::description();
  EXPECT_EQ(info->info->profiles, expected.profiles);
  EXPECT_EQ(info->info->audio_header, expected.audio_header);
  EXPECT_EQ(info->info->audio, expected.audio);
  EXPECT_EQ(info->order + 1, player().order_of<guest_event::StreamReady>());
  EXPECT_LT(player().order_of<guest_event::StreamReady>(),
            player().order_of<guest_event::ControllerRequested>());
  EXPECT_EQ(player().guest->state(), Progress::playing);
  ASSERT_NE(player().last_state(), nullptr);
  EXPECT_EQ(player().last_state()->state, Progress::playing);
}

TEST_F(ManualPairGuest, ReceivesTheForwardedSourceAfterItsDescription) {
  const auto info = std::ranges::find_if(
      player().streams,
      [](const auto& record) { return record.info.has_value(); });
  ASSERT_NE(info, player().streams.end());
  const auto info_order = info->order;
  ASSERT_NO_FATAL_FAILURE(world.forward_source());
  const auto video = player().frames(ManualStream::Media::video);
  const auto audio = player().frames(ManualStream::Media::audio);
  ASSERT_EQ(video.size(), SourceStream::video_frames.size());
  ASSERT_EQ(audio.size(), SourceStream::audio_frames.size());
  for (std::size_t index = 0; index < video.size(); ++index)
    EXPECT_EQ(video[index],
              test_support::Bytes::unhex(SourceStream::video_frames[index]));
  for (std::size_t index = 0; index < audio.size(); ++index)
    EXPECT_EQ(audio[index],
              test_support::Bytes::unhex(SourceStream::audio_frames[index]));
  for (const auto& record : player().streams)
    EXPECT_TRUE(!record.media.has_value() || record.order > info_order);
}

TEST_F(ManualPairGuest, HasItsControllerAppliedOnItsPad) {
  ASSERT_NO_FATAL_FAILURE(world.forward_source());
  const auto cross_and_up =
      static_cast<std::uint32_t>(
          libchiaking::controllers::ControllerButton::cross) |
      static_cast<std::uint32_t>(
          libchiaking::controllers::ControllerButton::dpad_up);
  ASSERT_NO_FATAL_FAILURE(pressed());
  const auto applied = world.side.inputs().back();
  EXPECT_EQ(applied.controller_id, 1U);
  EXPECT_EQ(applied.buttons, cross_and_up);
  EXPECT_EQ(applied.left_x, 12000);
  EXPECT_EQ(applied.left_y, -32767);
  EXPECT_EQ(applied.right_x, 0);
  EXPECT_EQ(applied.right_y, -32767);
  EXPECT_EQ(applied.l2, 255);
  EXPECT_EQ(applied.r2, 100);
  EXPECT_EQ(applied.touches[0].id, -1);
  EXPECT_EQ(applied.touches[1].id, -1);
  EXPECT_EQ(applied.accel_y, 1.0F);
  EXPECT_EQ(applied.orient_w, 1.0F);
}

TEST_F(ManualPairGuest, FeelsOnlyTheRumbleOfItsOwnSeat) {
  auto& host = *world.host;
  host.source_feedback(libchiaking::controllers::ControllerFeedback{
      ManualPair::seat(3), libchiaking::controllers::HapticAudio{}});
  host.source_feedback(ManualPair::rumble(3, 255, 255));
  host.source_feedback(ManualPair::rumble(1, 200, 50));
  ASSERT_NO_FATAL_FAILURE(
      world.until([&] { return player().count<guest_event::Rumble>() > 0; }));
  const auto* felt = player().find<guest_event::Rumble>();
  ASSERT_NE(felt, nullptr);
  EXPECT_EQ(felt->low_frequency, 200 * 257);
  EXPECT_EQ(felt->high_frequency, 50 * 257);
}

TEST_F(ManualPairGuest, SeesANewCapacityOnlyOnTheNextPump) {
  auto& host = *world.host;
  auto& side = world.side;
  const auto before = side.events.size();
  EXPECT_TRUE(host.set_capacity(5));
  EXPECT_EQ(side.events.size(), before);
  EXPECT_EQ(host.next_wait(milliseconds{1000}), milliseconds{0});
  EXPECT_TRUE(ManualPair::pump(host));
  ASSERT_GT(side.events.size(), before);
  const auto* resized =
      std::get_if<host_event::SeatsChanged>(&side.events[before]);
  ASSERT_NE(resized, nullptr);
  EXPECT_EQ(resized->seats.capacity, 5U);
}

TEST_F(ManualPairGuest, FreesItsSeatAndPadWhenItLeaves) {
  auto& host = *world.host;
  auto& side = world.side;
  const auto join = join_seat();
  ASSERT_NO_FATAL_FAILURE(pressed());
  ASSERT_NO_FATAL_FAILURE(left());
  EXPECT_EQ(player().guest->state(), Progress::gone);
  EXPECT_EQ(player().count<guest_event::StreamStopped>(), 1U);
  EXPECT_EQ(side.count<host_event::RemovalRequested>(), 1U);
  const auto* removal = side.find<host_event::RemovalRequested>(player().id);
  ASSERT_NE(removal, nullptr);
  EXPECT_EQ(removal->console_seat,
            libchiaking::controllers::SeatNumber::create(1));
  EXPECT_EQ(removal->seat, join);
  const auto* closed = side.last_state(player().id);
  ASSERT_NE(closed, nullptr);
  EXPECT_EQ(closed->state, Progress::gone);
  EXPECT_TRUE(closed->by_peer);
  EXPECT_EQ(closed->reason, manual::Reason::user);
  EXPECT_FALSE(closed->error);
  const auto neutral = side.inputs().back();
  EXPECT_EQ(neutral.controller_id, 1U);
  EXPECT_EQ(neutral.buttons, 0U);
  EXPECT_EQ(neutral.l2, 0);
  EXPECT_EQ(host.seats().remote_seats, 0U);
  EXPECT_TRUE(side.monotonic(player().id));
  EXPECT_EQ(player().guest->next_wait(milliseconds{777}), milliseconds{777});
  EXPECT_TRUE(ManualPair::failed_with(
      player().guest->set_controller(ManualPair::pressed_state()),
      Error::internal));
}

TEST_F(ManualPairGuest, StopsInvitationsOnceTheSourceEnds) {
  auto& host = *world.host;
  ASSERT_NO_FATAL_FAILURE(left());
  host.source_ended();
  ASSERT_NO_FATAL_FAILURE(world.until(
      [&] { return host.next_wait(milliseconds{500}) == milliseconds{500}; }));
  EXPECT_TRUE(ManualPair::failed_with(host.invite(), Error::source_ended));
}

TEST_F(ManualPairGuest, IsEndedWhenTheConsoleDropsItsSeat) {
  auto& host = *world.host;
  auto& side = world.side;
  EXPECT_EQ(host.seats().remote_seats, 1U);

  host.source_unseated(ManualPair::seat(3));
  ASSERT_NO_FATAL_FAILURE(world.steps(50));
  EXPECT_EQ(player().guest->state(), Progress::playing);
  EXPECT_EQ(side.state(player().id), Progress::playing);
  EXPECT_EQ(side.count<host_event::RemovalRequested>(), 0U);

  host.source_unseated(ManualPair::seat(1));
  ASSERT_NO_FATAL_FAILURE(world.until([&] {
    return side.state(player().id) == Progress::gone &&
           player().guest->state() == Progress::gone;
  }));
  ASSERT_NE(player().last_state(), nullptr);
  EXPECT_TRUE(player().last_state()->by_peer);
  EXPECT_EQ(player().last_state()->reason, manual::Reason::source);
  ASSERT_NE(side.last_state(player().id), nullptr);
  EXPECT_EQ(side.last_state(player().id)->reason, manual::Reason::source);
  EXPECT_FALSE(side.last_state(player().id)->by_peer);
  EXPECT_FALSE(side.last_state(player().id)->error);
  const auto* removal = side.find<host_event::RemovalRequested>(player().id);
  ASSERT_NE(removal, nullptr);
  EXPECT_EQ(removal->console_seat,
            libchiaking::controllers::SeatNumber::create(1));
  EXPECT_EQ(host.seats().remote_seats, 0U);
  EXPECT_TRUE(host.invite());
}

class ManualPairCodes : public ManualWorldTest {};

TEST_F(ManualPairCodes, FailOnBothSidesWhenOneIsWrong) {
  ASSERT_NO_FATAL_FAILURE(world.start_source());
  world.side.approve_inside_callback = false;
  ASSERT_NO_FATAL_FAILURE(world.pair(0x40));
  auto& player = world.latest();
  auto& host = *world.host;
  EXPECT_TRUE(host.approve(player.id));
  ASSERT_NO_FATAL_FAILURE(world.enter_codes(player, false));
  ASSERT_NO_FATAL_FAILURE(world.until([&] {
    return player.guest->state() == Progress::gone &&
           world.side.state(player.id) == Progress::gone;
  }));
  ASSERT_NE(player.last_state(), nullptr);
  EXPECT_EQ(player.last_state()->error, Error::pairing_failed);
  ASSERT_NE(world.side.last_state(player.id), nullptr);
  EXPECT_EQ(world.side.last_state(player.id)->error, Error::pairing_failed);
  EXPECT_EQ(world.side.count<host_event::JoinRequested>(), 0U);
  EXPECT_EQ(world.side.count<host_event::IdrRequested>(), 0U);
  EXPECT_TRUE(player.streams.empty());
  const auto reply = player.find<guest_event::ReplyReady>()->block.view();
  EXPECT_FALSE(host.import_reply(reply));
  GuestId::Bytes unknown_bytes{};
  unknown_bytes.fill(std::byte{0xee});
  const GuestId unknown{unknown_bytes};
  EXPECT_TRUE(ManualPair::failed_with(host.approve(unknown), Error::internal));
  EXPECT_TRUE(ManualPair::failed_with(host.enter_code(unknown, "1234"),
                                      Error::internal));
}

}  // namespace
}  // namespace kakusan_test
