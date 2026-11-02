#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/end_to_end_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace kakusan_test {

using test_support::play::SourceStream;
namespace {

using libchiaking::controllers::ControllerButton;
using libchiaking::controllers::SeatNumber;
using libchiaking::kakusan::ControllerMap;
using libchiaking::kakusan::SourceRumble;
using libchiaking::net::kakusan::Error;
namespace control = libchiaking::net::kakusan::control;

class OneGuestEndToEnd : public EndToEndTest {
 protected:
  void SetUp() override { world.start_source(); }

  auto press(Player& player) -> World::Input {
    EXPECT_TRUE(player.session->set_controller(pressed(), world.now));
    world.until([&] {
      return !world.inputs.empty() && world.inputs.back().buttons != 0;
    });
    return world.inputs.empty() ? World::Input{} : world.inputs.back();
  }

  World world;
};

TEST_F(OneGuestEndToEnd, JoinsWithASeatAndTheStream) {
  auto& player = world.join(0x40);
  ASSERT_EQ(world.bound.size(), 1U);
  EXPECT_EQ(world.bound.front().guest, player.id);
  ASSERT_EQ(world.joins.size(), 1U);
  ASSERT_TRUE(player.controller);
  EXPECT_EQ(player.controller->seat, world.joins.front().seat);
  EXPECT_EQ(player.controller->capacity, 4U);
  EXPECT_EQ(player.controller->local_seats, 1U);
  ASSERT_TRUE(player.info);
  EXPECT_EQ(player.fps, SourceStream::video_fps);
  EXPECT_EQ(player.info->profiles, SourceStream::description().profiles);
  EXPECT_EQ(player.info->audio_header,
            SourceStream::description().audio_header);
  EXPECT_EQ(world.host->seats().remote_seats, 1U);
  EXPECT_EQ(world.idr_requests, 1U);
}

TEST_F(OneGuestEndToEnd, DecodesTheForwardedSource) {
  auto& player = world.join(0x40);
  world.forward_source();
  expect_decoded(player);
}

TEST_F(OneGuestEndToEnd, MapsItsInputOntoItsConsoleSeat) {
  auto& player = world.join(0x40);
  world.forward_source();
  const auto applied = press(player);
  const auto expected = ControllerMap::to_source_state(
      pressed(), libchiaking::net::kakusan::button_mask);
  ASSERT_TRUE(expected);
  EXPECT_EQ(applied.controller_id, 1U);
  EXPECT_EQ(applied.buttons, expected.value().buttons);
  EXPECT_EQ(applied.buttons,
            static_cast<std::uint32_t>(ControllerButton::cross));
  EXPECT_EQ(applied.left_x, 12000);
  EXPECT_EQ(applied.right_y, -32767);
  EXPECT_EQ(applied.l2, 255U);
}

TEST_F(OneGuestEndToEnd, FeelsRumbleForItsOwnSeat) {
  auto& player = world.join(0x40);
  world.forward_source();
  press(player);
  world.host->source_rumble(
      SourceRumble{SeatNumber::create(1).value(), 40000, 900}, world.now);
  world.host->source_rumble(SourceRumble{SeatNumber::create(3).value(), 1, 1},
                            world.now);
  world.until([&] { return !player.rumbles.empty(); });
  ASSERT_FALSE(player.rumbles.empty());
  EXPECT_EQ(player.rumbles.front().low_frequency, 40000U);
  EXPECT_EQ(player.rumbles.front().high_frequency, 900U);
}

TEST_F(OneGuestEndToEnd, ReleasesItsSeatWhenItLeaves) {
  auto& player = world.join(0x40);
  world.forward_source();
  press(player);
  world.host->source_rumble(
      SourceRumble{SeatNumber::create(1).value(), 40000, 900}, world.now);
  world.until([&] { return !player.rumbles.empty(); });

  player.session->leave(world.now);
  world.until([&] { return world.host_state(player.id) == Progress::gone; });
  EXPECT_EQ(player.session->state(), Progress::gone);
  EXPECT_EQ(player.stopped, 1U);
  ASSERT_EQ(player.diagnostics.size(), 1U);
  EXPECT_TRUE(player.diagnostics.front().starts_with("media receiver closed: "))
      << player.diagnostics.front();
  EXPECT_EQ(player.diagnostics.front().find(" 0 datagrams"), std::string::npos);
  ASSERT_EQ(world.diagnostics.size(), 1U);
  EXPECT_TRUE(
      world.diagnostics.front().starts_with("media sender closed: round trip "))
      << world.diagnostics.front();
  EXPECT_FALSE(player.session->next_deadline_us());
  ASSERT_EQ(world.removals.size(), 1U);
  EXPECT_EQ(world.removals.front().console_seat, SeatNumber::create(1));
  ASSERT_FALSE(world.inputs.empty());
  EXPECT_EQ(world.inputs.back().controller_id, 1U);
  EXPECT_EQ(world.inputs.back().buttons, 0U);
  EXPECT_EQ(world.host->seats().remote_seats, 0U);
  const auto closed =
      std::find_if(world.states.rbegin(), world.states.rend(),
                   [&](const auto& state) { return state.guest == player.id; });
  ASSERT_NE(closed, world.states.rend());
  EXPECT_TRUE(closed->by_peer);
  EXPECT_EQ(closed->reason, control::Reason::user);
}

class TwoGuestsEndToEnd : public EndToEndTest {
 protected:
  void SetUp() override {
    world.start_source();
    first = &world.join(0x40);
    second = &world.join(0x70);
  }

  World world;
  Player* first{};
  Player* second{};
};

TEST_F(TwoGuestsEndToEnd, SitOnDifferentSeats) {
  ASSERT_TRUE(first->controller);
  ASSERT_TRUE(second->controller);
  EXPECT_NE(first->controller->seat.seat, second->controller->seat.seat);
  EXPECT_EQ(world.host->seats().remote_seats, 2U);
}

TEST_F(TwoGuestsEndToEnd, BothDecodeOneSource) {
  world.forward_source();
  expect_decoded(*first);
  expect_decoded(*second);
}

TEST_F(TwoGuestsEndToEnd, EachDriveOnlyTheirOwnSeat) {
  world.forward_source();
  libchiaking::net::kakusan::InputState east;
  east.buttons =
      static_cast<std::uint32_t>(libchiaking::net::kakusan::Button::east);
  EXPECT_TRUE(second->session->set_controller(east, world.now));
  world.until([&] {
    return std::ranges::any_of(world.inputs, [](const auto& state) {
      return state.controller_id == 2 &&
             state.buttons ==
                 static_cast<std::uint32_t>(ControllerButton::moon);
    });
  });
  EXPECT_TRUE(std::ranges::none_of(world.inputs, [](const auto& state) {
    return state.controller_id == 1 && state.buttons != 0;
  }));
}

TEST_F(TwoGuestsEndToEnd, FeelOnlyTheRumbleOfTheirOwnSeat) {
  world.forward_source();
  world.host->source_rumble(SourceRumble{SeatNumber::create(2).value(), 7, 9},
                            world.now);
  world.until([&] { return !second->rumbles.empty(); });
  EXPECT_TRUE(first->rumbles.empty());
}

TEST_F(TwoGuestsEndToEnd, KeepOnePlayingWhenTheOtherIsRemoved) {
  world.forward_source();
  EXPECT_TRUE(world.host->remove(first->id, world.now));
  world.until([&] { return first->session->state() == Progress::gone; });
  ASSERT_FALSE(first->states.empty());
  EXPECT_TRUE(first->states.back().by_peer);
  EXPECT_EQ(first->states.back().reason, control::Reason::user);
  EXPECT_EQ(first->stopped, 1U);
  EXPECT_EQ(second->session->state(), Progress::playing);
  EXPECT_EQ(world.host->seats().remote_seats, 1U);
}

TEST_F(TwoGuestsEndToEnd, EndWhenTheSourceEnds) {
  world.forward_source();
  EXPECT_TRUE(world.host->remove(first->id, world.now));
  world.until([&] { return first->session->state() == Progress::gone; });

  world.host->source_ended(world.now);
  world.until([&] { return second->session->state() == Progress::gone; });
  ASSERT_FALSE(second->states.empty());
  EXPECT_TRUE(second->states.back().by_peer);
  EXPECT_EQ(second->states.back().reason, control::Reason::source);
  EXPECT_EQ(second->stopped, 1U);
  world.until([&] { return !world.host->next_deadline_us(); });
  EXPECT_EQ(world.host->seats().remote_seats, 0U);
  EXPECT_FALSE(world.host->invite(world.clock()));
}

class WrongCodeEndToEnd : public EndToEndTest {};

TEST_F(WrongCodeEndToEnd, NeverReachesTheSource) {
  World world;
  world.start_source();
  const auto guest = world.host->invite(world.clock());
  ASSERT_TRUE(guest);
  world.until([&] { return !world.invitations.empty(); });
  ASSERT_FALSE(world.invitations.empty());
  world.players.push_back(
      std::make_unique<Player>(world.exchange, 0x40, world.now));
  auto& player = *world.players.back();
  player.id = guest.value();
  EXPECT_TRUE(player.session->accept_invitation(
      world.invitations.back().block.view(), world.clock()));
  world.until([&] { return !player.reply.empty(); });
  EXPECT_TRUE(world.host->import_reply(player.reply, world.clock()));
  EXPECT_TRUE(world.host->approve(player.id, world.now));
  world.until([&] {
    return !player.code.empty() && !world.host_code(player.id).empty();
  });

  auto wrong = world.host_code(player.id);
  ASSERT_GE(wrong.size(), 4U);
  wrong[3] = wrong[3] == '9' ? '0' : static_cast<char>(wrong[3] + 1);
  EXPECT_TRUE(world.host->enter_code(player.id, player.code, world.now));
  EXPECT_TRUE(player.session->enter_code(wrong, world.now));
  world.until([&] {
    return player.session->state() == Progress::gone &&
           world.host_state(player.id) == Progress::gone;
  });
  ASSERT_FALSE(player.states.empty());
  EXPECT_EQ(player.states.back().error, Error::pairing_failed);
  EXPECT_TRUE(world.joins.empty());
  EXPECT_EQ(world.idr_requests, 0U);
  EXPECT_FALSE(player.info);
  EXPECT_TRUE(player.media.empty());
  EXPECT_EQ(world.host->seats().remote_seats, 0U);
  EXPECT_EQ(world.host->seats().reserved_seats, 0U);
  EXPECT_FALSE(world.host->import_reply(player.reply, world.clock()));
}

}  // namespace
}  // namespace kakusan_test
