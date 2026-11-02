#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/pair_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace kakusan_test {
namespace {

class SharedVisitEnding : public PairStageTest {
 protected:
  static auto wrong(const libchiaking::kakusan::VerificationCode& right)
      -> libchiaking::kakusan::VerificationCode {
    std::string digits{right.digits()};
    digits.front() =
        digits.front() == '9' ? '0' : static_cast<char>(digits.front() + 1);
    return *libchiaking::kakusan::VerificationCode::parse(digits);
  }

  void seated(SeatNumber wanted) {
    ASSERT_NO_FATAL_FAILURE(stage.verified());
    ASSERT_NO_FATAL_FAILURE(stage.sit(wanted));
  }
};

TEST_F(SharedVisitEnding, ReportsTheFaultOfAPictureTheGuestCannotShow) {
  ASSERT_NO_FATAL_FAILURE(seated(seat(1)));
  stage.link.send_shared_media();
  ASSERT_NO_FATAL_FAILURE(
      stage.until([&] { return !stage.guest->media.video.empty(); }));

  stage.visit->video_output_failed(-11);
  ASSERT_NO_FATAL_FAILURE(stage.finish_visit());
  EXPECT_EQ(stage.outcome->status(), VisitOutcome::Status::media_failed);
  ASSERT_TRUE(stage.outcome->media_fault());
  EXPECT_EQ(stage.outcome->media_fault()->cause, -11);
  EXPECT_EQ(stage.outcome->code(),
            VisitOutcome::media_base +
                static_cast<int>(stage.outcome->media_fault()->kind));
  ASSERT_NO_FATAL_FAILURE(
      stage.until([&] { return stage.host.reached(Progress::gone); }));
  const auto* gone = stage.host.seen.last<SharedSession::PlayerChanged>();
  ASSERT_NE(gone, nullptr);
  EXPECT_TRUE(gone->player.by_them);
  EXPECT_EQ(gone->player.ended, EndReason::Kind::left);
  EXPECT_EQ(stage.host.seen.count<SharedSession::PlayerUnseated>(), 1U);
  EXPECT_TRUE(stage.share->invite_player());
}

TEST_F(SharedVisitEnding, FreesTheSeatOfAGuestWhoLeaves) {
  ASSERT_NO_FATAL_FAILURE(seated(seat(1)));
  EXPECT_TRUE(stage.visit->send(PairStage::pressed()));
  ASSERT_NO_FATAL_FAILURE(stage.until([&] {
    return std::ranges::any_of(
        stage.link.controller_samples(seat(1)),
        [](const auto& state) { return state.buttons != 0; });
  }));

  EXPECT_TRUE(stage.visit->leave());
  ASSERT_NO_FATAL_FAILURE(stage.finish_visit());
  EXPECT_EQ(stage.outcome->status(), VisitOutcome::Status::completed);
  EXPECT_FALSE(stage.outcome->refusal());
  const auto* left = stage.guest->seen.last<GuestVisit::VisitChanged>();
  ASSERT_NE(left, nullptr);
  EXPECT_EQ(left->progress, Progress::gone);
  EXPECT_FALSE(left->by_them);
  EXPECT_FALSE(left->refusal);
  EXPECT_EQ(left->ended, EndReason::Kind::left);
  EXPECT_EQ(stage.guest->seen.count<GuestVisit::StreamStopped>(), 1U);
  EXPECT_TRUE(PairStage::refused(stage.visit->send(PairStage::pressed()),
                                 Refusal::Kind::not_visiting));
  EXPECT_TRUE(
      PairStage::refused(stage.visit->leave(), Refusal::Kind::not_visiting));

  ASSERT_NO_FATAL_FAILURE(stage.until([&] {
    return stage.host.reached(Progress::gone) &&
           stage.host.seen.count<SharedSession::PlayerUnseated>() == 1;
  }));
  const auto* gone = stage.host.seen.last<SharedSession::PlayerChanged>();
  ASSERT_NE(gone, nullptr);
  EXPECT_TRUE(gone->player.by_them);
  EXPECT_EQ(gone->player.ended, EndReason::Kind::left);
  EXPECT_FALSE(gone->player.refusal);
  EXPECT_EQ(stage.host.seen.last<SharedSession::PlayerUnseated>()->seat,
            seat(1));
  EXPECT_FALSE(stage.session->seats().seat(seat(1)).is_held_by_guest());
  EXPECT_EQ(stage.share->seats(),
            (libchiaking::kakusan::SeatSummary{4, 1, 0, 0}));
  ASSERT_FALSE(stage.link.seat_requests().empty());
  EXPECT_EQ(stage.link.seat_requests().back().kind,
            ScriptedLink::SeatRequest::Kind::remove);
  EXPECT_EQ(stage.link.seat_requests().back().seat, seat(1));
  ASSERT_FALSE(stage.link.controller_samples(seat(1)).empty());
  const auto neutral = stage.link.controller_samples(seat(1)).back();
  EXPECT_EQ(neutral.buttons, 0U);
  EXPECT_EQ(neutral.l2, 0);
  EXPECT_EQ(neutral.left_x, 0);
  EXPECT_EQ(stage.host.seen.count<SharedSession::SharingEnded>(), 0U);
  EXPECT_TRUE(stage.share->invite_player());
}

TEST_F(SharedVisitEnding, EndsOnBothSidesAfterAWrongCode) {
  ASSERT_NO_FATAL_FAILURE(stage.open());
  ASSERT_NO_FATAL_FAILURE(stage.stream());
  ASSERT_NO_FATAL_FAILURE(stage.invite());
  ASSERT_NO_FATAL_FAILURE(stage.visit_with(stage.invitation));
  ASSERT_NO_FATAL_FAILURE(stage.answer());
  static_cast<void>(stage.share->verify(
      stage.guest_id,
      wrong(stage.guest->seen.last<GuestVisit::CodeToReadOut>()->code)));
  static_cast<void>(stage.visit->verify(
      wrong(stage.host.seen.last<SharedSession::CodeToReadOut>()->code)));
  ASSERT_NO_FATAL_FAILURE(stage.finish_visit());
  ASSERT_NO_FATAL_FAILURE(
      stage.until([&] { return stage.host.reached(Progress::gone); }));
  EXPECT_EQ(stage.guest->seen.last<GuestVisit::VisitChanged>()->refusal,
            Refusal::Kind::verification_failed);
  EXPECT_EQ(
      stage.host.seen.last<SharedSession::PlayerChanged>()->player.refusal,
      Refusal::Kind::verification_failed);
  EXPECT_EQ(stage.outcome->status(), VisitOutcome::Status::visit_failed);
  EXPECT_EQ(stage.outcome->refusal(), Refusal::Kind::verification_failed);
  EXPECT_EQ(stage.outcome->code(),
            VisitOutcome::refusal_base +
                Refusal{Refusal::Kind::verification_failed}.code());
  EXPECT_EQ(stage.link.count("seat_join"), 0U);
  EXPECT_EQ(stage.link.keyframe_requests(), 0U);
  EXPECT_TRUE(stage.guest->media.descriptions.empty());
  EXPECT_TRUE(stage.guest->media.video.empty());
  EXPECT_EQ(stage.host.seen.count<SharedSession::PlayerSeated>(), 0U);
  EXPECT_FALSE(stage.share->accept_reply(KakusanPair::copied(
      stage.guest->seen.last<GuestVisit::ReplyReady>()->reply)));
  EXPECT_FALSE(stage.share->give_seat(libchiaking::kakusan::GuestId{}));
  EXPECT_FALSE(stage.share->verify(
      libchiaking::kakusan::GuestId{},
      *libchiaking::kakusan::VerificationCode::parse("1234")));
  EXPECT_FALSE(stage.share->remember(stage.guest_id, "Stranger"));
  EXPECT_TRUE(host_store.devices().empty());
  EXPECT_TRUE(guest_store.devices().empty());
}

TEST_F(SharedVisitEnding,
       TellsAGuestTheConsoleDroppedThatTheSessionTookTheSeat) {
  ASSERT_NO_FATAL_FAILURE(seated(seat(1)));

  stage.link.drop(seat(3));
  stage.quiet(200);
  EXPECT_TRUE(stage.host.reached(Progress::playing));
  EXPECT_TRUE(stage.guest->reached(Progress::playing));
  EXPECT_EQ(stage.host.seen.count<SharedSession::PlayerUnseated>(), 0U);

  stage.link.drop(seat(1));
  ASSERT_NO_FATAL_FAILURE(stage.finish_visit());
  ASSERT_NO_FATAL_FAILURE(
      stage.until([&] { return stage.host.reached(Progress::gone); }));
  const auto* told = stage.guest->seen.last<GuestVisit::VisitChanged>();
  ASSERT_NE(told, nullptr);
  EXPECT_TRUE(told->by_them);
  EXPECT_EQ(told->ended, EndReason::Kind::session_ended);
  EXPECT_FALSE(told->refusal);
  const auto* gone = stage.host.seen.last<SharedSession::PlayerChanged>();
  ASSERT_NE(gone, nullptr);
  EXPECT_FALSE(gone->player.by_them);
  EXPECT_EQ(gone->player.ended, EndReason::Kind::session_ended);
  EXPECT_FALSE(gone->player.refusal);
  EXPECT_EQ(stage.host.seen.count<SharedSession::PlayerUnseated>(), 1U);
  EXPECT_EQ(stage.host.seen.last<SharedSession::PlayerUnseated>()->seat,
            seat(1));
  EXPECT_FALSE(stage.session->seats().seat(seat(1)).is_held_by_guest());
  EXPECT_EQ(stage.host.seen.count<SharedSession::SharingEnded>(), 0U);
  EXPECT_EQ(stage.share->seats(),
            (libchiaking::kakusan::SeatSummary{4, 1, 0, 0}));
  EXPECT_TRUE(stage.share->invite_player());
}

}  // namespace
}  // namespace kakusan_test
