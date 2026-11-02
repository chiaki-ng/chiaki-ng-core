#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/shared_session_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>

namespace kakusan_test {
namespace {

using libchiaking::kakusan::SharedSession;
using libchiaking::kakusan::SharingRules;
using manual::GuestId;
using manual::Progress;
using test_support::play::SessionRig;
using namespace std::chrono_literals;
namespace guest_event = manual::guest_event;

class SharedSessionGuest : public SharedSessionTest {};

TEST_F(SharedSessionGuest, JoinsPlaysAndIsRemoved) {
  ShareRig rig;
  ASSERT_NO_FATAL_FAILURE(rig.play());
  rig.session.link.feed_back(rumble(seat(1), 9, 9));
  SessionRig::eventually(
      [&] { return rig.session.media.feedback().size() == 1; });
  ASSERT_NO_FATAL_FAILURE(rig.stream());
  EXPECT_EQ(rig.session.media.descriptions().size(), 1U);
  EXPECT_EQ(rig.log.logged("Kakusan is sharing the stream at 60 fps"), 1U);

  ASSERT_NO_FATAL_FAILURE(rig.join(0x40));
  auto& player = rig.newest();
  const auto guest = rig.guest;
  const std::string oversized(manual::max_block_size + 1, 'A');
  EXPECT_TRUE(
      refused(rig.share->accept_reply({}), Refusal::Kind::invalid_argument));
  EXPECT_TRUE(refused(rig.share->accept_reply(oversized),
                      Refusal::Kind::invalid_argument));
  const auto* bound = rig.recorder.find<SharedSession::PlayerIdentified>();
  const auto identity = TestPlatform::identity(0x40);
  ASSERT_NE(bound, nullptr);
  EXPECT_EQ(bound->guest, guest);
  EXPECT_EQ(bound->device, identity.key());
  EXPECT_EQ(bound->fingerprint, identity.fingerprint());
  EXPECT_EQ(bound->recognition.kind,
            libchiaking::kakusan::Recognition::Kind::unknown);

  ASSERT_TRUE(
      rig.until([&] { return rig.session.link.count("seat_join") == 1; }));
  const auto held = rig.session.session->seats().seat(seat(1));
  EXPECT_TRUE(held.is_held_by_guest());
  EXPECT_EQ(held.state(), libchiaking::play::SeatState::joining);
  EXPECT_EQ(rig.recorder.count<SharedSession::PlayerSeated>(), 0U);
  EXPECT_EQ(rig.session.session->seats().remove_player(seat(1)).error(),
            libchiaking::play::InputRefusal::not_owner);

  rig.session.link.join(seat(1), {0, 0, 255});
  ASSERT_TRUE(rig.until([&] {
    return rig.recorder.reached(Progress::playing) &&
           player.reached(Progress::playing);
  }));
  EXPECT_EQ(rig.recorder.count<SharedSession::PlayerSeated>(), 1U);
  ASSERT_NE(rig.recorder.find<SharedSession::PlayerSeated>(), nullptr);
  EXPECT_EQ(rig.recorder.find<SharedSession::PlayerSeated>()->guest, guest);
  EXPECT_EQ(rig.recorder.find<SharedSession::PlayerSeated>()->seat, seat(1));
  EXPECT_EQ(rig.recorder.count<SharedSession::PlayerUnseated>(), 0U);
  ASSERT_NE(rig.recorder.state(), nullptr);
  EXPECT_TRUE(rig.recorder.state()->grant.has_value());
  EXPECT_EQ(rig.recorder.state()->device, identity.key());
  const auto players = rig.share->players();
  ASSERT_EQ(players.size(), 1U);
  EXPECT_EQ(players.front().guest, guest);
  EXPECT_EQ(players.front().progress, Progress::playing);
  EXPECT_EQ(players.front().console_seat, seat(1));
  EXPECT_EQ(players.front().fingerprint, identity.fingerprint());
  EXPECT_EQ(rig.share->seats(),
            (libchiaking::kakusan::SeatSummary{4, 1, 1, 0}));
  ASSERT_NE(player.find<guest_event::StreamReady>(), nullptr);
  EXPECT_EQ(player.find<guest_event::StreamReady>()->video_fps, 60U);
  ASSERT_TRUE(
      rig.until([&] { return rig.session.link.keyframe_requests() >= 1; }));
  EXPECT_TRUE(rig.recorder.any<SharedSession::SeatsChanged>(
      [](const auto& changed) { return changed.seats.remote_seats == 1; }));
  EXPECT_EQ(rig.session.session->controllers().send(seat(1), pressed()).error(),
            libchiaking::play::InputRefusal::not_owner);
  EXPECT_TRUE(
      rig.session.session->controllers().send(SeatNumber::first(), pressed()));

  const auto remembered = rig.share->remember(guest, "  Sam ");
  ASSERT_TRUE(remembered);
  EXPECT_EQ(remembered.value().nickname, "Sam");
  EXPECT_EQ(remembered.value().key, identity.key());
  ASSERT_EQ(rig.store.saved().size(), 1U);
  EXPECT_EQ(rig.store.saved().front().nickname, "Sam");
  EXPECT_EQ(rig.root.remembered().recognise(identity.key()).kind,
            libchiaking::kakusan::Recognition::Kind::recognised);
  const auto stranger = rig.share->remember(GuestId{}, "Sam");
  ASSERT_FALSE(stranger);
  EXPECT_EQ(stranger.error().kind,
            libchiaking::kakusan::RememberRefusal::Kind::unknown_device);

  EXPECT_TRUE(player.guest->set_controller(pressed()));
  ASSERT_TRUE(rig.until([&] {
    return std::ranges::any_of(
        rig.session.link.controller_samples(seat(1)),
        [](const auto& state) { return state.buttons != 0; });
  }));
  const auto samples = rig.session.link.controller_samples(seat(1));
  const auto forwarded = std::ranges::find_if(
      samples, [](const auto& state) { return state.buttons != 0; });
  ASSERT_NE(forwarded, samples.end());
  EXPECT_EQ(forwarded->left_x, 12000);
  EXPECT_EQ(forwarded->l2, 255U);
  EXPECT_EQ(forwarded->buttons, pressed().buttons);

  rig.session.link.feed_back(rumble(seat(1), 255, 128));
  ASSERT_TRUE(
      rig.until([&] { return player.find<guest_event::Rumble>() != nullptr; }));
  const auto* felt = player.find<guest_event::Rumble>();
  EXPECT_EQ(felt->low_frequency, 0xffffU);
  EXPECT_EQ(felt->high_frequency, 0x8080U);
  EXPECT_EQ(rig.session.media.feedback().size(), 2U);

  const auto felt_before = player.count<guest_event::Rumble>();
  const auto still_felt = [&](const guest_event::Rumble& shake) {
    const bool stopped = shake.low_frequency == 0 && shake.high_frequency == 0;
    const bool refreshed = shake.low_frequency == felt->low_frequency &&
                           shake.high_frequency == felt->high_frequency;
    return stopped || refreshed;
  };
  rig.session.link.feed_back(libchiaking::controllers::ControllerFeedback{
      SeatNumber::first(),
      libchiaking::controllers::HapticIntensity{
          libchiaking::controllers::FeedbackIntensity::medium}});
  rig.session.link.feed_back(rumble(seat(1), 255, 128));
  ASSERT_TRUE(rig.until([&] {
    return !std::ranges::all_of(player.rumbles_after(felt_before), still_felt);
  }));
  const auto later = player.rumbles_after(felt_before);
  const auto softened = std::ranges::find_if_not(later, still_felt);
  ASSERT_NE(softened, later.end());
  EXPECT_EQ(softened->low_frequency, 32768U);
  EXPECT_EQ(softened->high_frequency, 16448U);

  const libchiaking::kakusan::Tuning tuning{100ms,  400ms, 3,     8000ms, 5,
                                            6000ms, 8192,  16384, 500ms};
  EXPECT_TRUE(rig.share->tune(tuning));
  EXPECT_EQ(rig.log.logged("Kakusan media tuning: queue wait 100 to 400 "
                           "ms at 3 round trips"),
            1U);
  EXPECT_TRUE(refused(rig.share->change(*SharingRules::create(
                          4, 1, libchiaking::kakusan::required_buttons)),
                      Refusal::Kind::invalid_argument));
  EXPECT_TRUE(rig.share->change(*SharingRules::create(3, 2)));
  EXPECT_TRUE(rig.share->take_back_seat(guest));

  ASSERT_TRUE(rig.until([&] {
    return rig.recorder.reached(Progress::gone) &&
           rig.recorder.count<SharedSession::PlayerUnseated>() == 1;
  }));
  EXPECT_EQ(rig.recorder.find<SharedSession::PlayerUnseated>()->guest, guest);
  EXPECT_EQ(rig.recorder.find<SharedSession::PlayerUnseated>()->seat, seat(1));
  EXPECT_FALSE(rig.session.session->seats().seat(seat(1)).is_held_by_guest());
  EXPECT_TRUE(
      rig.recorder.any<SharedSession::SeatsChanged>([](const auto& changed) {
        return changed.seats.capacity == 3 && changed.seats.local_seats == 2;
      }));
  EXPECT_TRUE(rig.share->players().empty());

  EXPECT_TRUE(rig.session.session->leave());
  rig.session.session->wait_until_ended();
  EXPECT_EQ(rig.recorder.count<SharedSession::SharingEnded>(), 1U);
  EXPECT_TRUE(refused(rig.share->invite_player(), Refusal::Kind::not_sharing));
  EXPECT_TRUE(refused(rig.share->tune({}), Refusal::Kind::not_sharing));
  EXPECT_GT(rig.log.logged("Kakusan guest progress"), 0U);
  rig.share->stop_sharing();
  EXPECT_TRUE(refused(rig.share->give_seat(guest), Refusal::Kind::not_sharing));
  EXPECT_EQ(rig.recorder.count<SharedSession::SharingEnded>(), 1U);
}

TEST_F(SharedSessionGuest, FeelsRumbleAtTheStrengthSetBeforeSharing) {
  ShareRig rig;
  rig.session.start_playing();
  rig.session.link.feed_back(libchiaking::controllers::ControllerFeedback{
      SeatNumber::first(),
      libchiaking::controllers::HapticIntensity{
          libchiaking::controllers::FeedbackIntensity::weak}});
  SessionRig::eventually(
      [&] { return rig.session.media.feedback().size() == 1; });
  ASSERT_TRUE(rig.begin());
  ASSERT_NO_FATAL_FAILURE(rig.stream());
  ASSERT_NO_FATAL_FAILURE(rig.seated(0x47));
  auto& player = rig.newest();
  rig.session.link.feed_back(rumble(seat(1), 255, 128));
  ASSERT_TRUE(
      rig.until([&] { return player.find<guest_event::Rumble>() != nullptr; }));
  const auto* felt = player.find<guest_event::Rumble>();
  EXPECT_EQ(felt->low_frequency, 21627U);
  EXPECT_EQ(felt->high_frequency, 10856U);
}

}  // namespace
}  // namespace kakusan_test
