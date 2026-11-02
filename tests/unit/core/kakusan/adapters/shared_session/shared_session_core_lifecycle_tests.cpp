#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/shared_session/shared_session_core_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <variant>

namespace kakusan_test::session_core {
namespace {

class SharedSessionCoreGuest : public SharedSessionCoreTest {};
class SharedSessionCoreGuests : public SharedSessionCoreTest {};
class SharedSessionCoreRemoval : public SharedSessionCoreTest {};
class SharedSessionCoreDeadlines : public SharedSessionCoreTest {};

TEST_F(SharedSessionCoreGuest, IsInvitedSeatedFedAndLeaves) {
  CoreRig rig;
  ASSERT_TRUE(rig.session);
  const auto initial = rig.session->seats();
  EXPECT_EQ(initial.capacity, 4U);
  EXPECT_EQ(initial.local_seats, 1U);
  EXPECT_EQ(initial.remote_seats, 0U);
  EXPECT_EQ(initial.reserved_seats, 0U);
  EXPECT_FALSE(rig.session->next_deadline_us());

  ASSERT_NO_FATAL_FAILURE(rig.invite_guest(0x21));
  const auto party = rig.party;
  rig.pads[party.id] = 1;
  const auto invitation =
      ShortBlockCodec::parse_invitation(crypto(), *rig.block_for(party.id));
  ASSERT_TRUE(invitation);
  const auto& offered = invitation.value().body;
  EXPECT_EQ(offered.invitation_id, party.id);
  EXPECT_EQ(offered.identity_hint,
            ShortBlockCodec::hint_of(identity(host_tag).key().bytes()));
  EXPECT_EQ(offered.source_protocol, 12U);
  EXPECT_GE(offered.invite_expires_at, start_wall + 600);
  EXPECT_LE(offered.invite_expires_at, start_wall + 601);
  Digest256 host_digest{};
  host_digest.fill(std::byte{0xa1});
  EXPECT_EQ(offered.dtls_sha256, host_digest);
  EXPECT_EQ(offered.capacity, 4U);
  EXPECT_LT(rig.block_for(party.id)->size(), 200U);
  const auto ready = rig.all<CoreRig::Core::InvitationReady>();
  ASSERT_EQ(ready.size(), 1U);
  EXPECT_EQ(ready.front().expires_at, offered.invite_expires_at);
  const auto waiting_deadline = rig.session->next_deadline_us();
  ASSERT_TRUE(waiting_deadline);
  EXPECT_GT(*waiting_deadline, rig.now);
  EXPECT_LE(*waiting_deadline, rig.now + 600 * one_second);

  ASSERT_NO_FATAL_FAILURE(rig.reply(party));
  EXPECT_EQ(rig.count<CoreRig::Core::GuestIdentified>(), 0U);
  EXPECT_EQ(rig.host_state(party.id), Progress::connecting);

  ASSERT_NO_FATAL_FAILURE(rig.verify(party));
  EXPECT_EQ(rig.host_state(party.id), Progress::verifying);
  EXPECT_EQ(rig.count<CoreRig::Core::CodeReady>(), 1U);
  const auto identified = rig.all<CoreRig::Core::GuestIdentified>();
  ASSERT_EQ(identified.size(), 1U);
  EXPECT_EQ(identified.front().guest, party.id);
  EXPECT_EQ(identified.front().identity, party.guest->identity.key().bytes());
  EXPECT_EQ(
      identified.front().fingerprint,
      Hash::sha256(crypto(), party.guest->identity.key().bytes()).value());
  ASSERT_NO_FATAL_FAILURE(rig.exchange_codes(party));
  ASSERT_TRUE(rig.run_until(
      [&] { return rig.host_state(party.id) == Progress::waiting_for_seat; }));
  EXPECT_EQ(rig.count<CoreRig::Core::JoinRequested>(), 0U);
  EXPECT_EQ(rig.count<CoreRig::Core::SeatsChanged>(), 0U);

  const auto approved_at = rig.outputs.size();
  EXPECT_TRUE(rig.session->approve(party.id, rig.now));
  rig.drain();
  const auto joins = rig.all<CoreRig::Core::JoinRequested>(approved_at);
  ASSERT_EQ(joins.size(), 1U);
  EXPECT_EQ(joins.front().guest, party.id);
  EXPECT_EQ(joins.front().seat.seat, 2U);
  EXPECT_EQ(joins.front().seat.generation, 1U);
  const auto reserved = rig.all<CoreRig::Core::SeatsChanged>(approved_at);
  ASSERT_FALSE(reserved.empty());
  EXPECT_EQ(reserved.front().seats.reserved_seats, 1U);
  EXPECT_EQ(reserved.front().seats.remote_seats, 0U);
  ASSERT_TRUE(rig.run_until([&] {
    return party.guest->state() == Progress::playing &&
           rig.host_state(party.id) == Progress::playing &&
           party.guest->receiver;
  }));
  ASSERT_TRUE(rig.last_state(party.id));
  EXPECT_EQ(rig.last_state(party.id)->seat, joins.front().seat);
  const auto seat_history = rig.all<CoreRig::Core::SeatsChanged>();
  ASSERT_FALSE(seat_history.empty());
  EXPECT_EQ(seat_history.back().seats.remote_seats, 1U);
  EXPECT_EQ(seat_history.back().seats.reserved_seats, 0U);
  EXPECT_EQ(rig.session->seats().remote_seats, 1U);
  EXPECT_EQ(party.guest->source_protocol, 12U);
  ASSERT_TRUE(party.guest->receive_key);
  ASSERT_TRUE(party.guest->receive_iv);
  EXPECT_TRUE(
      std::ranges::equal(*party.guest->receive_key, source_keys().key()));
  EXPECT_TRUE(std::ranges::equal(*party.guest->receive_iv, source_keys().iv()));
  ASSERT_TRUE(party.guest->source_session);
  EXPECT_NE(*party.guest->source_session, control::SourceSessionId{});
  rig.run(5);
  EXPECT_EQ(rig.count<CoreRig::Core::IdrRequested>(), 1U);

  const InputState pressed{
      button(Button::south) | button(Button::l2), 1200, -800, 300, 0, 0, 0, {}};
  const auto pressed_at = rig.outputs.size();
  party.guest->sample(pressed);
  rig.run(50);
  const auto pad_inputs = rig.inputs(1, pressed_at);
  ASSERT_FALSE(pad_inputs.empty());
  EXPECT_TRUE(same_input(pad_inputs.back(), mapped(pressed)));
  EXPECT_EQ(pad_inputs.back().l2, 255U);
  EXPECT_EQ(rig.count<CoreRig::Core::ControllerInput>(pressed_at),
            pad_inputs.size());

  rig.session->source_rumble(SourceRumble{pad_seat(1), 900, 300}, rig.now);
  rig.session->source_rumble(SourceRumble{pad_seat(3), 5000, 5000}, rig.now);
  rig.run(50);
  ASSERT_FALSE(party.guest->rumbles.empty());
  EXPECT_EQ(party.guest->rumbles.back().low_frequency, 900U);
  EXPECT_EQ(party.guest->rumbles.back().high_frequency, 300U);
  for (const auto& rumble : party.guest->rumbles)
    EXPECT_NE(rumble.low_frequency, 5000U);

  rig.send_video(40);
  rig.send(libchiaking::play::SharedDatagram::Media::audio, 7'000'000,
           haptics_datagram(7'000'000));
  rig.send(libchiaking::play::SharedDatagram::Media::video, 7'004'096,
           video_datagram(7'004'096, 400, 3));
  rig.run(300);
  EXPECT_EQ(party.guest->packets.size(), 40U);
  rig.check_packets(*party.guest);

  const auto left_at = rig.outputs.size();
  party.guest->leave();
  rig.run(1500);
  const auto final_state = rig.last_state(party.id);
  ASSERT_TRUE(final_state);
  EXPECT_EQ(final_state->state, Progress::gone);
  EXPECT_TRUE(final_state->by_peer);
  EXPECT_EQ(final_state->reason, control::Reason::user);
  EXPECT_FALSE(final_state->error);
  const auto after_leave = rig.inputs(1, left_at);
  ASSERT_FALSE(after_leave.empty());
  EXPECT_TRUE(is_neutral(after_leave.back()));
  const auto removed = rig.removals(party.id, left_at);
  ASSERT_EQ(removed.size(), 1U);
  EXPECT_EQ(removed.front().console_seat, pad_seat(1));
  EXPECT_EQ(removed.front().seat, joins.front().seat);
  std::size_t neutral_index = 0;
  std::size_t removal_index = 0;
  for (std::size_t index = left_at; index < rig.outputs.size(); ++index) {
    if (const auto* input =
            std::get_if<CoreRig::Core::ControllerInput>(&rig.outputs[index]))
      neutral_index = is_neutral(input->state) ? index : neutral_index;
    if (std::holds_alternative<CoreRig::Core::RemovalRequested>(
            rig.outputs[index]))
      removal_index = index;
  }
  EXPECT_NE(neutral_index, 0U);
  EXPECT_LT(neutral_index, removal_index);
  EXPECT_EQ(rig.session->seats().remote_seats, 0U);
  EXPECT_EQ(rig.all<CoreRig::Core::SeatsChanged>().back().seats.remote_seats,
            0U);
  EXPECT_EQ(party.guest->closed, 1U);
  EXPECT_EQ(party.guest->state(), Progress::gone);
  EXPECT_FALSE(rig.session->remove(party.id, rig.now));
  EXPECT_FALSE(rig.session->next_deadline_us());
}

TEST_F(SharedSessionCoreGuests, StayIndependentOfEachOther) {
  CoreRig rig;
  ASSERT_TRUE(rig.session);
  ASSERT_NO_FATAL_FAILURE(rig.admit(0x31, 1));
  const auto first = rig.party;
  ASSERT_NO_FATAL_FAILURE(rig.admit(0x32, 2));
  const auto second = rig.party;
  ASSERT_TRUE(rig.last_state(first.id));
  ASSERT_TRUE(rig.last_state(second.id));
  const auto first_seat = rig.last_state(first.id)->seat;
  const auto second_seat = rig.last_state(second.id)->seat;
  ASSERT_TRUE(first_seat);
  ASSERT_TRUE(second_seat);
  EXPECT_NE(first_seat->seat, second_seat->seat);
  EXPECT_EQ(rig.session->seats().remote_seats, 2U);
  rig.run(1100);
  EXPECT_EQ(rig.count<CoreRig::Core::IdrRequested>(), 2U);

  const auto south = pressing(Button::south);
  const auto east = pressing(Button::east);
  const auto pressed_at = rig.outputs.size();
  first.guest->sample(south);
  second.guest->sample(east);
  rig.run(50);
  ASSERT_FALSE(rig.inputs(1, pressed_at).empty());
  ASSERT_FALSE(rig.inputs(2, pressed_at).empty());
  EXPECT_TRUE(same_input(rig.inputs(1, pressed_at).back(), mapped(south)));
  EXPECT_TRUE(same_input(rig.inputs(2, pressed_at).back(), mapped(east)));
  EXPECT_EQ(rig.inputs(1, pressed_at).size() + rig.inputs(2, pressed_at).size(),
            rig.count<CoreRig::Core::ControllerInput>(pressed_at));

  rig.session->source_rumble(SourceRumble{pad_seat(1), 1000, 0}, rig.now);
  rig.run(30);
  ASSERT_FALSE(first.guest->rumbles.empty());
  EXPECT_EQ(first.guest->rumbles.back().low_frequency, 1000U);
  for (const auto& rumble : second.guest->rumbles) {
    EXPECT_EQ(rumble.low_frequency, 0U);
    EXPECT_EQ(rumble.high_frequency, 0U);
  }
  rig.session->source_rumble(SourceRumble{pad_seat(2), 0, 2000}, rig.now);
  rig.run(30);
  ASSERT_FALSE(second.guest->rumbles.empty());
  EXPECT_EQ(second.guest->rumbles.back().high_frequency, 2000U);
  for (const auto& rumble : first.guest->rumbles)
    EXPECT_EQ(rumble.high_frequency, 0U);

  rig.send_video(20);
  rig.run(300);
  EXPECT_EQ(first.guest->packets.size(), 20U);
  EXPECT_EQ(second.guest->packets.size(), 20U);

  first.guest->leave();
  rig.run(1500);
  EXPECT_EQ(rig.host_state(first.id), Progress::gone);
  EXPECT_EQ(first.guest->state(), Progress::gone);
  EXPECT_EQ(rig.host_state(second.id), Progress::playing);
  EXPECT_EQ(second.guest->state(), Progress::playing);
  EXPECT_EQ(rig.session->seats().remote_seats, 1U);
  EXPECT_TRUE(rig.removals(second.id).empty());

  const auto north = pressing(Button::north);
  const auto later = rig.outputs.size();
  second.guest->sample(north);
  rig.send_video(10);
  rig.run(300);
  ASSERT_FALSE(rig.inputs(2, later).empty());
  EXPECT_TRUE(same_input(rig.inputs(2, later).back(), mapped(north)));
  EXPECT_TRUE(rig.inputs(1, later).empty());
  EXPECT_EQ(second.guest->packets.size(), 30U);
  EXPECT_EQ(first.guest->packets.size(), 20U);
  rig.check_packets(*first.guest);
  rig.check_packets(*second.guest);
}

TEST_F(SharedSessionCoreRemoval, OfAPendingGuestCancelsItsInvitation) {
  CoreRig rig;
  ASSERT_TRUE(rig.session);
  ASSERT_NO_FATAL_FAILURE(rig.invite_guest(0xb1));
  const auto pending = rig.party;
  EXPECT_TRUE(rig.session->remove(pending.id, rig.now));
  rig.drain();
  const auto pending_state = rig.last_state(pending.id);
  ASSERT_TRUE(pending_state);
  EXPECT_EQ(pending_state->state, Progress::gone);
  EXPECT_EQ(pending_state->reason, control::Reason::user);
  EXPECT_TRUE(rig.removals(pending.id).empty());
  const auto block = pending.guest->answer(*rig.block_for(pending.id));
  const auto stale = rig.session->import_reply(block, rig.clock());
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error(), Error::invalid_block);
}

TEST_F(SharedSessionCoreRemoval, OfAnActiveGuestFreesItsSeat) {
  CoreRig rig;
  ASSERT_TRUE(rig.session);
  ASSERT_NO_FATAL_FAILURE(rig.invite_guest(0xb1));
  EXPECT_TRUE(rig.session->remove(rig.party.id, rig.now));
  rig.drain();
  ASSERT_NO_FATAL_FAILURE(rig.admit(0xb2, 1));
  const auto active = rig.party;
  const auto removed_at = rig.outputs.size();
  EXPECT_TRUE(rig.session->remove(active.id, rig.now));
  rig.drain();
  rig.run(1500);
  EXPECT_EQ(rig.host_state(active.id), Progress::gone);
  EXPECT_EQ(active.guest->reason, control::Reason::user);
  EXPECT_TRUE(active.guest->by_peer);
  EXPECT_EQ(active.guest->state(), Progress::gone);
  const auto inputs = rig.inputs(1, removed_at);
  ASSERT_FALSE(inputs.empty());
  EXPECT_TRUE(is_neutral(inputs.front()));
  const auto removed = rig.removals(active.id, removed_at);
  ASSERT_EQ(removed.size(), 1U);
  EXPECT_EQ(removed.front().console_seat, pad_seat(1));
  EXPECT_FALSE(rig.session->remove(active.id, rig.now));
  EXPECT_FALSE(rig.session->next_deadline_us());
}

TEST_F(SharedSessionCoreDeadlines, SettleWhileAGuestPlaysAndClearAfterwards) {
  CoreRig rig;
  ASSERT_TRUE(rig.session);
  EXPECT_FALSE(rig.session->next_deadline_us());
  ASSERT_NO_FATAL_FAILURE(rig.admit(0xd1, 1));
  const auto party = rig.party;
  const auto until = rig.now + 5 * one_second;
  std::size_t pumps = 0;
  std::size_t repeats = 0;
  while (rig.now < until) {
    const auto host = rig.session->next_deadline_us();
    ASSERT_TRUE(host);
    ASSERT_GE(*host, rig.now);
    const auto next = libchiaking::timing::Deadline::earliest(
        host, party.guest->next_deadline());
    ASSERT_TRUE(next);
    const auto advance = *next > rig.now ? *next - rig.now : 0;
    repeats = advance == 0 ? repeats + 1 : 0;
    ASSERT_LT(repeats, 16U);
    rig.step(advance);
    ++pumps;
  }
  EXPECT_LT(pumps, 2000U);
  EXPECT_EQ(rig.host_state(party.id), Progress::playing);
  EXPECT_EQ(party.guest->state(), Progress::playing);

  EXPECT_TRUE(rig.session->remove(party.id, rig.now));
  rig.drain();
  for (std::size_t index = 0; index < 100 && rig.session->next_deadline_us();
       ++index) {
    const auto host = rig.session->next_deadline_us();
    rig.step(*host > rig.now ? *host - rig.now : 1000);
  }
  EXPECT_EQ(rig.host_state(party.id), Progress::gone);
  EXPECT_FALSE(rig.session->next_deadline_us());
}

}  // namespace
}  // namespace kakusan_test::session_core
