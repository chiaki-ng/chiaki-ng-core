#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/identities.hpp"
#include "tests/unit/core/kakusan/pair_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace kakusan_test {
namespace {

class SharedVisit : public PairStageTest {
 protected:
  static constexpr std::size_t rejections_before_a_keyframe = 2;

  static auto felt(const libchiaking::controllers::ControllerFeedback& feedback,
                   libchiaking::controllers::Rumble wanted) -> bool {
    const auto* rumble = feedback.get<libchiaking::controllers::Rumble>();
    return rumble != nullptr && *rumble == wanted;
  }

  auto host_key() const -> libchiaking::kakusan::IdentityKey {
    return stage.pair->host().this_device().key();
  }

  auto guest_key() const -> libchiaking::kakusan::IdentityKey {
    return stage.pair->guest().this_device().key();
  }

  void seated() {
    ASSERT_NO_FATAL_FAILURE(stage.verified());
    ASSERT_NO_FATAL_FAILURE(stage.sit(seat(2)));
  }

  void media_shared() {
    stage.link.send_shared_media();
    ASSERT_NO_FATAL_FAILURE(stage.until([&] {
      return stage.guest->media.video.size() == shared.video_frames.size() &&
             stage.guest->media.audio.size() == shared.audio_frames.size();
    }));
  }

  void remembered() {
    ASSERT_TRUE(stage.share->remember(stage.guest_id, "  Guest pad "));
    ASSERT_TRUE(stage.visit->remember_host("Host den"));
  }

  void seat_taken_back() {
    ASSERT_TRUE(stage.share->take_back_seat(stage.guest_id));
    ASSERT_NO_FATAL_FAILURE(stage.until([&] {
      return stage.host.reached(Progress::gone) &&
             stage.host.seen.count<SharedSession::PlayerUnseated>() == 1;
    }));
    ASSERT_NO_FATAL_FAILURE(stage.finish_visit());
  }

  void revisited() {
    stage.quiet(3000);
    ASSERT_NO_FATAL_FAILURE(stage.invite());
    ASSERT_NO_FATAL_FAILURE(stage.visit_with(stage.invitation));
    ASSERT_NO_FATAL_FAILURE(stage.answer());
    ASSERT_NO_FATAL_FAILURE(stage.verify());
    ASSERT_NO_FATAL_FAILURE(stage.sit(seat(1)));
  }

  const ScriptedLink::SharedMedia shared = ScriptedLink::shared_media();
};

TEST_F(SharedVisit, StartsWithOneSavedIdentityForEachSide) {
  EXPECT_NE(host_key(), guest_key());
  EXPECT_EQ(host_store.identity_saves(), 1U);
  EXPECT_EQ(guest_store.identity_saves(), 1U);
  ASSERT_TRUE(host_store.identity());
  EXPECT_EQ(host_store.identity()->key, host_key());
  EXPECT_EQ(host_store.identity()->fingerprint,
            stage.pair->host().this_device().fingerprint());
  ASSERT_TRUE(guest_store.identity());
  EXPECT_EQ(guest_store.identity()->key, guest_key());
  EXPECT_EQ(guest_store.identity()->fingerprint,
            stage.pair->guest().this_device().fingerprint());
}

TEST_F(SharedVisit, InvitesNobodyUntilTheStreamIsShared) {
  EXPECT_TRUE(stage.session->share().can_be_shared());
  ASSERT_NO_FATAL_FAILURE(stage.open());
  EXPECT_TRUE(PairStage::refused(stage.share->invite_player(),
                                 Refusal::Kind::not_sharing));
  ASSERT_NO_FATAL_FAILURE(stage.stream());
  const auto* streamed = stage.host.seen.last<SharedSession::StreamShared>();
  ASSERT_NE(streamed, nullptr);
  EXPECT_EQ(streamed->frames_per_second, shared.frames_per_second);
  ASSERT_EQ(stage.media.descriptions.size(), 1U);
  EXPECT_EQ(stage.media.descriptions.front(), shared.description);
}

TEST_F(SharedVisit, TellsTheGuestWhoIsHosting) {
  ASSERT_NO_FATAL_FAILURE(stage.open());
  ASSERT_NO_FATAL_FAILURE(stage.stream());
  ASSERT_NO_FATAL_FAILURE(stage.invite());
  const auto read = stage.pair->guest().read(stage.invitation);
  ASSERT_TRUE(read);
  EXPECT_EQ(read.value().summary().host_hint, Identities::hint_of(host_key()));
  EXPECT_EQ(read.value().summary().recognition.kind,
            Recognition::Kind::unknown);
  EXPECT_EQ(read.value().summary().seats.capacity, 4U);
  EXPECT_TRUE(PairStage::refused(stage.pair->guest().read("not an invitation"),
                                 Refusal::Kind::invalid_text));
}

TEST_F(SharedVisit, ShowsEachSideTheOthersDevice) {
  ASSERT_NO_FATAL_FAILURE(stage.open());
  ASSERT_NO_FATAL_FAILURE(stage.stream());
  ASSERT_NO_FATAL_FAILURE(stage.invite());
  ASSERT_NO_FATAL_FAILURE(stage.visit_with(stage.invitation));
  EXPECT_EQ(stage.visit->host().host_hint, Identities::hint_of(host_key()));
  EXPECT_FALSE(stage.visit->host_key());
  ASSERT_NO_FATAL_FAILURE(stage.answer());
  const auto* identified =
      stage.host.seen.last<SharedSession::PlayerIdentified>();
  ASSERT_NE(identified, nullptr);
  EXPECT_EQ(identified->guest, stage.guest_id);
  EXPECT_EQ(identified->device, guest_key());
  EXPECT_EQ(identified->fingerprint,
            stage.pair->guest().this_device().fingerprint());
  EXPECT_EQ(identified->recognition.kind, Recognition::Kind::unknown);
  const auto* proven = stage.guest->seen.last<GuestVisit::HostIdentified>();
  ASSERT_NE(proven, nullptr);
  EXPECT_EQ(proven->key, host_key());
  EXPECT_EQ(proven->fingerprint,
            stage.pair->host().this_device().fingerprint());
  EXPECT_EQ(proven->recognition.kind, Recognition::Kind::unknown);
  EXPECT_EQ(proven->seats, (libchiaking::kakusan::SeatSummary{4, 1, 0, 0}));
  EXPECT_EQ(stage.guest->seen.count<GuestVisit::HostIdentified>(), 1U);
  EXPECT_EQ(stage.visit->host_key(), host_key());
  EXPECT_EQ(stage.host.seen.last<SharedSession::CodeToReadOut>()
                ->code.digits()
                .size(),
            libchiaking::kakusan::VerificationCode::size);
  ASSERT_NO_FATAL_FAILURE(stage.verify());
}

TEST_F(SharedVisit, SeatsTheGuestWhereTheHostChose) {
  ASSERT_NO_FATAL_FAILURE(seated());
  using Request = ScriptedLink::SeatRequest;
  const std::vector expected{
      Request{Request::Kind::choose_account, seat(2),
              libchiaking::AccountId{PairStage::guest_account}, std::nullopt},
      Request{Request::Kind::join, seat(2), std::nullopt,
              libchiaking::controllers::ControllerKind::dualsense}};
  EXPECT_EQ(stage.link.seat_requests(), expected);
  const auto* seated = stage.host.seen.last<SharedSession::PlayerSeated>();
  ASSERT_NE(seated, nullptr);
  EXPECT_EQ(seated->guest, stage.guest_id);
  EXPECT_EQ(seated->seat, seat(2));
  EXPECT_TRUE(stage.session->seats().seat(seat(2)).is_held_by_guest());
  EXPECT_EQ(stage.guest->seen.count<GuestVisit::Connected>(), 1U);
  EXPECT_EQ(
      stage.guest->seen.last<GuestVisit::StreamReady>()->frames_per_second,
      shared.frames_per_second);
  const auto players = stage.share->players();
  ASSERT_EQ(players.size(), 1U);
  EXPECT_EQ(players.front().guest, stage.guest_id);
  EXPECT_EQ(players.front().progress, Progress::playing);
  EXPECT_EQ(players.front().console_seat, seat(2));
  EXPECT_EQ(players.front().device, guest_key());
  EXPECT_EQ(stage.share->seats(),
            (libchiaking::kakusan::SeatSummary{4, 1, 1, 0}));
  ASSERT_EQ(stage.guest->media.descriptions.size(), 1U);
  EXPECT_EQ(stage.guest->media.descriptions.front().profiles,
            shared.description.profiles);
  EXPECT_EQ(stage.guest->media.descriptions.front().audio_header,
            shared.description.audio_header);
}

TEST_F(SharedVisit, ForwardsTheGuestsControllerToItsSeat) {
  ASSERT_NO_FATAL_FAILURE(seated());
  EXPECT_TRUE(stage.visit->send(PairStage::pressed()));
  ASSERT_NO_FATAL_FAILURE(stage.until([&] {
    return std::ranges::any_of(
        stage.link.controller_samples(seat(2)),
        [](const auto& state) { return state.buttons != 0; });
  }));
  const auto samples = stage.link.controller_samples(seat(2));
  const auto forwarded = std::ranges::find_if(
      samples, [](const auto& state) { return state.buttons != 0; });
  ASSERT_NE(forwarded, samples.end());
  EXPECT_EQ(forwarded->buttons, PairStage::pressed().buttons);
  EXPECT_EQ(forwarded->left_x, 12000);
  EXPECT_EQ(forwarded->l2, 255);
  EXPECT_TRUE(stage.link.controller_samples(SeatNumber::first()).empty());
}

TEST_F(SharedVisit, HandsTheGuestTheSharedMedia) {
  ASSERT_NO_FATAL_FAILURE(seated());
  ASSERT_NO_FATAL_FAILURE(media_shared());
  EXPECT_EQ(stage.link.count("shared_media"), 1U);
  EXPECT_EQ(stage.guest->media.video, shared.video_frames);
  EXPECT_EQ(stage.guest->media.audio, shared.audio_frames);
  ASSERT_EQ(stage.guest->media.headers.size(), 1U);
  EXPECT_EQ(stage.guest->media.headers.front(),
            shared.description.profiles.front().header);
  ASSERT_EQ(stage.guest->media.frame_indexes.size(),
            shared.video_frames.size() + 1);
  EXPECT_EQ(stage.guest->media.frame_indexes[0],
            stage.guest->media.frame_indexes[1]);
  EXPECT_TRUE(stage.media.video.empty());
  EXPECT_TRUE(stage.media.audio.empty());
}

TEST_F(SharedVisit, FeedsBackOnlyTheGuestsOwnRumble) {
  ASSERT_NO_FATAL_FAILURE(seated());
  stage.link.feed_back(libchiaking::controllers::ControllerFeedback{
      seat(2), libchiaking::controllers::HapticAudio{}});
  stage.link.feed_back(libchiaking::controllers::ControllerFeedback{
      seat(3), libchiaking::controllers::Rumble{255, 255}});
  stage.link.feed_back(libchiaking::controllers::ControllerFeedback{
      seat(2), libchiaking::controllers::Rumble{200, 50}});
  ASSERT_NO_FATAL_FAILURE(stage.until([&] {
    return std::ranges::any_of(
        stage.guest->media.feedback_seen, [&](const auto& feedback) {
          return felt(feedback, libchiaking::controllers::Rumble{200, 50});
        });
  }));
  stage.quiet(200);
  for (const auto& feedback : stage.guest->media.feedback_seen) {
    EXPECT_EQ(feedback.kind(),
              libchiaking::controllers::ControllerFeedback::Kind::rumble);
    EXPECT_EQ(feedback.seat, SeatNumber::first());
    EXPECT_FALSE(felt(feedback, libchiaking::controllers::Rumble{255, 255}));
  }
  ASSERT_FALSE(stage.guest->media.feedback_seen.empty());
  EXPECT_TRUE(felt(stage.guest->media.feedback_seen.back(),
                   libchiaking::controllers::Rumble{200, 50}));
  EXPECT_EQ(stage.media.feedback_seen.size(), 3U);
}

TEST_F(SharedVisit, AsksForAKeyframeOnlyAfterRepeatedDecoderRejections) {
  ASSERT_NO_FATAL_FAILURE(seated());
  ASSERT_NO_FATAL_FAILURE(media_shared());
  ASSERT_FALSE(stage.guest->media.frame_indexes.empty());
  const auto presented = stage.visit->video_presented();
  ASSERT_TRUE(presented);
  EXPECT_EQ(presented->frames_lost, 0U);
  EXPECT_FALSE(presented->recovered);
  stage.quiet(1200);
  const auto asked = stage.link.keyframe_requests();
  stage.visit->video_rejected(stage.guest->media.frame_indexes.back(), -7);
  stage.quiet(1200);
  EXPECT_EQ(stage.link.keyframe_requests(), asked);
  for (std::size_t rejection = 1; rejection < rejections_before_a_keyframe;
       ++rejection)
    stage.visit->video_rejected(stage.guest->media.frame_indexes.back(), -7);
  ASSERT_NO_FATAL_FAILURE(
      stage.until([&] { return stage.link.keyframe_requests() > asked; }));
  EXPECT_EQ(stage.visit->video_statistics().decoder_rejections,
            rejections_before_a_keyframe);
  EXPECT_EQ(stage.visit->video_statistics().last_decoder_error, -7);
  EXPECT_TRUE(stage.guest->reached(Progress::playing));
  EXPECT_FALSE(stage.turns->ended());
}

TEST_F(SharedVisit, RemembersBothDevicesByTheirNicknames) {
  ASSERT_NO_FATAL_FAILURE(seated());
  const auto guest_named =
      stage.share->remember(stage.guest_id, "  Guest pad ");
  ASSERT_TRUE(guest_named);
  EXPECT_EQ(guest_named.value().nickname, "Guest pad");
  EXPECT_EQ(guest_named.value().key, guest_key());
  const auto host_named = stage.visit->remember_host("Host den");
  ASSERT_TRUE(host_named);
  EXPECT_EQ(host_named.value().nickname, "Host den");
  EXPECT_EQ(host_named.value().key, host_key());
  ASSERT_EQ(host_store.devices().size(), 1U);
  ASSERT_EQ(guest_store.devices().size(), 1U);
  EXPECT_EQ(host_store.devices().front().key, guest_key());
  EXPECT_EQ(host_store.devices().front().nickname, "Guest pad");
  EXPECT_EQ(host_store.devices().front().fingerprint,
            stage.pair->guest().this_device().fingerprint());
  EXPECT_EQ(guest_store.devices().front().key, host_key());
  EXPECT_EQ(guest_store.devices().front().nickname, "Host den");
  EXPECT_EQ(guest_store.devices().front().fingerprint,
            stage.pair->host().this_device().fingerprint());
  EXPECT_EQ(host_store.devices().front().first_verified, stage.pair->wall());
  EXPECT_EQ(stage.pair->host().remembered().recognise(guest_key()).kind,
            Recognition::Kind::recognised);
  EXPECT_EQ(stage.pair->guest().remembered().recognise(host_key()).kind,
            Recognition::Kind::recognised);
}

TEST_F(SharedVisit, EndsWhenTheHostTakesTheSeatBack) {
  ASSERT_NO_FATAL_FAILURE(seated());
  ASSERT_NO_FATAL_FAILURE(seat_taken_back());
  const auto* unseated = stage.host.seen.last<SharedSession::PlayerUnseated>();
  ASSERT_NE(unseated, nullptr);
  EXPECT_EQ(unseated->guest, stage.guest_id);
  EXPECT_EQ(unseated->seat, seat(2));
  EXPECT_FALSE(stage.session->seats().seat(seat(2)).is_held_by_guest());
  EXPECT_TRUE(stage.share->players().empty());
  const auto* removed = stage.guest->seen.last<GuestVisit::VisitChanged>();
  ASSERT_NE(removed, nullptr);
  EXPECT_EQ(removed->progress, Progress::gone);
  EXPECT_TRUE(removed->by_them);
  EXPECT_FALSE(removed->refusal);
  EXPECT_EQ(removed->ended, EndReason::Kind::left);
  EXPECT_EQ(stage.outcome->status(), VisitOutcome::Status::completed);
  EXPECT_EQ(stage.outcome->code(), 0);
  EXPECT_EQ(stage.guest->seen.count<GuestVisit::Disconnected>(), 1U);
  EXPECT_EQ(stage.host.seen.count<SharedSession::SharingEnded>(), 0U);
}

TEST_F(SharedVisit, RecognisesBothDevicesOnTheNextVisit) {
  ASSERT_NO_FATAL_FAILURE(seated());
  ASSERT_NO_FATAL_FAILURE(remembered());
  ASSERT_NO_FATAL_FAILURE(seat_taken_back());
  stage.quiet(3000);
  ASSERT_NO_FATAL_FAILURE(stage.invite());
  const auto reread = stage.pair->guest().read(stage.invitation);
  ASSERT_TRUE(reread);
  EXPECT_EQ(reread.value().summary().recognition.kind,
            Recognition::Kind::recognised);
  ASSERT_TRUE(reread.value().summary().recognition.device);
  EXPECT_EQ(reread.value().summary().recognition.device->nickname, "Host den");
  ASSERT_NO_FATAL_FAILURE(stage.visit_with(stage.invitation));
  EXPECT_EQ(stage.visit->host().recognition.kind,
            Recognition::Kind::recognised);
  ASSERT_NO_FATAL_FAILURE(stage.answer());
  const auto* known = stage.host.seen.last<SharedSession::PlayerIdentified>();
  ASSERT_NE(known, nullptr);
  EXPECT_EQ(known->guest, stage.guest_id);
  EXPECT_EQ(known->device, guest_key());
  EXPECT_EQ(known->recognition.kind, Recognition::Kind::recognised);
  ASSERT_TRUE(known->recognition.device);
  EXPECT_EQ(known->recognition.device->nickname, "Guest pad");
  ASSERT_NO_FATAL_FAILURE(stage.verify());
  EXPECT_GT(host_store.devices().front().last_seen,
            host_store.devices().front().first_verified);
  EXPECT_GT(guest_store.devices().front().last_seen,
            guest_store.devices().front().first_verified);
  EXPECT_LE(host_store.devices().front().last_seen, stage.pair->wall());
  ASSERT_NO_FATAL_FAILURE(stage.sit(seat(1)));
  EXPECT_EQ(stage.host.seen.last<SharedSession::PlayerSeated>()->seat, seat(1));
}

TEST_F(SharedVisit, EndsWhenTheHostLeavesTheSession) {
  ASSERT_NO_FATAL_FAILURE(seated());
  ASSERT_NO_FATAL_FAILURE(remembered());
  ASSERT_NO_FATAL_FAILURE(seat_taken_back());
  ASSERT_NO_FATAL_FAILURE(revisited());
  EXPECT_TRUE(stage.session->leave());
  EXPECT_TRUE(stage.session->wait_until_ended().was_left());
  EXPECT_EQ(stage.host.seen.count<SharedSession::SharingEnded>(), 1U);
  EXPECT_TRUE(PairStage::refused(stage.share->invite_player(),
                                 Refusal::Kind::not_sharing));
  ASSERT_NO_FATAL_FAILURE(stage.finish_visit());
  const auto* told = stage.guest->seen.last<GuestVisit::VisitChanged>();
  ASSERT_NE(told, nullptr);
  EXPECT_EQ(told->progress, Progress::gone);
  EXPECT_TRUE(told->by_them);
  EXPECT_FALSE(told->refusal);
  EXPECT_EQ(told->ended, EndReason::Kind::session_ended);
  EXPECT_EQ(stage.outcome->status(), VisitOutcome::Status::completed);
  EXPECT_EQ(stage.guest->seen.count<GuestVisit::Disconnected>(), 1U);
}

}  // namespace
}  // namespace kakusan_test
