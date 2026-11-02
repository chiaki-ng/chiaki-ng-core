#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/loopback_transport.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/kakusan/adapters/shared_session/shared_session_core_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <memory>

namespace kakusan_test::session_core {
namespace {

class SharedSessionCoreSource : public SharedSessionCoreTest {};
class SharedSessionCoreRules : public SharedSessionCoreTest {};

TEST_F(SharedSessionCoreSource, EndingClosesActiveGuests) {
  CoreRig rig;
  ASSERT_TRUE(rig.session);
  ASSERT_NO_FATAL_FAILURE(rig.admit(0x81, 1));
  const auto party = rig.party;
  const auto ended_at = rig.outputs.size();
  rig.session->source_ended(rig.now);
  rig.drain();
  rig.run(1500);
  const auto state = rig.last_state(party.id);
  ASSERT_TRUE(state);
  EXPECT_EQ(state->state, Progress::gone);
  EXPECT_EQ(state->reason, control::Reason::source);
  EXPECT_EQ(party.guest->reason, control::Reason::source);
  ASSERT_EQ(rig.removals(party.id, ended_at).size(), 1U);
  EXPECT_EQ(rig.removals(party.id, ended_at).front().console_seat, pad_seat(1));
  const auto invited = rig.session->invite(rig.clock());
  ASSERT_FALSE(invited);
  EXPECT_EQ(invited.error(), Error::source_ended);
  EXPECT_FALSE(rig.session->source_started(stream_info(), 60, rig.now));
  EXPECT_FALSE(rig.session->next_deadline_us());
}

TEST_F(SharedSessionCoreRules, NeedTheRequiredButtonsAndSensibleSeats) {
  test_support::SeededRandom entropy{2};
  test_support::kakusan::LoopbackFactory factory{
      std::make_shared<test_support::kakusan::LoopbackExchange>(crypto())};
  const CoreRig::Core::Dependencies dependencies{
      crypto(), entropy, factory, test_support::process_threading()};
  EXPECT_FALSE(SharingRules::create(0, 1, button_mask));
  EXPECT_FALSE(SharingRules::create(2, 3, button_mask));
  EXPECT_FALSE(
      SharingRules::create(4, 1, button_mask & ~button(Button::south)));
  const auto narrowed =
      SharingRules::create(4, 1, button_mask & ~optional_buttons);
  ASSERT_TRUE(narrowed);
  EXPECT_TRUE(
      CoreRig::Core::create(dependencies, identity(host_tag), *narrowed));
}

TEST_F(SharedSessionCoreSource, IsNeededBeforeAnInvitation) {
  CoreRig rig{*SharingRules::create(4, 1), false};
  ASSERT_TRUE(rig.session);
  const auto early = rig.session->invite(rig.clock());
  ASSERT_FALSE(early);
  EXPECT_EQ(early.error(), Error::source_ended);
  const auto keyless = rig.session->source_started(stream_info(), 60, rig.now);
  ASSERT_FALSE(keyless);
  EXPECT_EQ(keyless.error(), Error::source_format);
  rig.session->source_keys(12, source_keys());
  EXPECT_FALSE(rig.session->source_started(stream_info(), 0, rig.now));
  auto no_profiles = stream_info();
  no_profiles.profiles.clear();
  EXPECT_FALSE(rig.session->source_started(no_profiles, 60, rig.now));
  EXPECT_TRUE(rig.session->source_started(stream_info(), 60, rig.now));
  rig.factory.fail_next = true;
  const auto unroutable = rig.session->invite(rig.clock());
  ASSERT_FALSE(unroutable);
  EXPECT_EQ(unroutable.error(), Error::no_path);
  EXPECT_TRUE(rig.session->invite(rig.clock()));
}

TEST_F(SharedSessionCoreSource, ChangesEndTheSetupsAlreadyIssued) {
  CoreRig rig;
  ASSERT_TRUE(rig.session);
  ASSERT_NO_FATAL_FAILURE(rig.admit(0xe1, 1));
  const auto party = rig.party;
  ASSERT_NO_FATAL_FAILURE(rig.invite_guest(0xe2));
  const auto pending = rig.party;
  ASSERT_TRUE(party.guest->source_session);
  const auto first_session = *party.guest->source_session;
  EXPECT_TRUE(rig.session->source_started(stream_info(), 60, rig.now));
  rig.drain();
  rig.run(50);
  EXPECT_EQ(rig.host_state(party.id), Progress::playing);

  const auto rekeyed_at = rig.outputs.size();
  auto rekeyed = source_keys(std::byte{0xff});
  rig.session->source_keys(12, rekeyed.clone());
  rig.send_video(5);
  rig.run(1500);
  const auto ended = rig.last_state(party.id);
  ASSERT_TRUE(ended);
  EXPECT_EQ(ended->state, Progress::gone);
  EXPECT_EQ(ended->reason, control::Reason::source);
  EXPECT_EQ(party.guest->reason, control::Reason::source);
  EXPECT_TRUE(party.guest->packets.empty());
  EXPECT_EQ(rig.removals(party.id, rekeyed_at).size(), 1U);
  EXPECT_EQ(rig.host_state(pending.id), Progress::waiting);
  const auto stale = rig.session->invite(rig.clock());
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error(), Error::source_ended);

  EXPECT_TRUE(rig.session->source_started(stream_info(), 60, rig.now));
  rig.pads[pending.id] = 2;
  ASSERT_NO_FATAL_FAILURE(rig.reply(pending));
  ASSERT_NO_FATAL_FAILURE(rig.verify(pending));
  ASSERT_NO_FATAL_FAILURE(rig.exchange_codes(pending));
  ASSERT_TRUE(rig.run_until([&] {
    return rig.host_state(pending.id) == Progress::waiting_for_seat;
  }));
  EXPECT_TRUE(rig.session->approve(pending.id, rig.now));
  rig.drain();
  ASSERT_TRUE(rig.run_until(
      [&] { return pending.guest->state() == Progress::playing; }));
  ASSERT_TRUE(pending.guest->receive_key);
  ASSERT_TRUE(pending.guest->source_session);
  EXPECT_TRUE(std::ranges::equal(*pending.guest->receive_key, rekeyed.key()));
  EXPECT_NE(*pending.guest->source_session, first_session);

  ASSERT_NO_FATAL_FAILURE(rig.invite_guest(0xe3));
  const auto unanswered = rig.party;
  auto changed = stream_info();
  changed.profiles.front().width = 1920;
  changed.profiles.front().height = 1080;
  EXPECT_TRUE(rig.session->source_started(changed, 60, rig.now));
  rig.drain();
  rig.run(1500);
  ASSERT_TRUE(rig.last_state(pending.id));
  EXPECT_EQ(rig.last_state(pending.id)->reason, control::Reason::source);
  EXPECT_EQ(rig.host_state(unanswered.id), Progress::waiting);

  rig.session->source_keys(9, source_keys());
  rig.step();
  EXPECT_EQ(rig.host_state(unanswered.id), Progress::gone);
  ASSERT_TRUE(rig.last_state(unanswered.id));
  EXPECT_EQ(rig.last_state(unanswered.id)->error, Error::source_ended);
  EXPECT_FALSE(rig.session->next_deadline_us());
}

}  // namespace
}  // namespace kakusan_test::session_core
