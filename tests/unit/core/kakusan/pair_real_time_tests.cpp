#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/pair_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>

namespace kakusan_test {
namespace {

class KakusanOnRealTime : public PairTest {
 protected:
  void SetUp() override {
    auto made = KakusanPair::create(home.factory, {host_store, guest_store},
                                    KakusanPair::Time::real);
    ASSERT_TRUE(made);
    pair = std::move(made).value();
    const auto started = home.play.start_scripted(
        home.home_user(), libchiaking::play::Where::home, link, {}, media,
        events);
    ASSERT_TRUE(started);
    session = started.value();
    link.connect();
  }

  void TearDown() override {
    if (visiting) {
      if (visit) visit->cancel();
      static_cast<void>(visiting->join());
    }
  }

  static auto host_at(Progress wanted) {
    return [wanted](const SharedSession::PlayerChanged& changed) {
      return changed.player.progress == wanted;
    };
  }

  static auto guest_at(Progress wanted) {
    return [wanted](const GuestVisit::VisitChanged& changed) {
      return changed.progress == wanted;
    };
  }

  static constexpr auto always = [](const auto&) { return true; };

  void verified(libchiaking::media::MediaSink& picture) {
    auto shared = pair->host().share(
        session->share(), *libchiaking::kakusan::SharingRules::create(4, 1),
        host);
    ASSERT_TRUE(shared);
    share = std::move(shared).value();
    link.start_shared_stream();
    ASSERT_NE(host.seen.wait_for<SharedSession::StreamShared>(always), nullptr);
    const auto invited = share->invite_player();
    ASSERT_TRUE(invited);
    guest_id = invited.value();
    const auto* invitation =
        host.seen.wait_for<SharedSession::InvitationReady>(always);
    ASSERT_NE(invitation, nullptr);
    auto read = pair->guest().read(KakusanPair::copied(invitation->invitation));
    ASSERT_TRUE(read);
    auto joined = pair->guest().join(std::move(read).value(), {guest, picture});
    ASSERT_TRUE(joined);
    visit = std::move(joined).value();
    visiting.emplace(libchiaking::platform::Thread::start(
                         PairTime::threading(), "visit",
                         libchiaking::platform::ThreadRole::session,
                         [this] { outcome.emplace(visit->run()); })
                         .value());
    const auto* reply = guest.seen.wait_for<GuestVisit::ReplyReady>(always);
    ASSERT_NE(reply, nullptr);
    EXPECT_TRUE(share->accept_reply(KakusanPair::copied(reply->reply)));
    const auto* hosts =
        host.seen.wait_for<SharedSession::CodeToReadOut>(always);
    const auto* guests = guest.seen.wait_for<GuestVisit::CodeToReadOut>(always);
    ASSERT_NE(hosts, nullptr);
    ASSERT_NE(guests, nullptr);
    EXPECT_TRUE(share->verify(guest_id, guests->code));
    EXPECT_TRUE(visit->verify(hosts->code));
    ASSERT_NE(host.seen.wait_for<SharedSession::PlayerChanged>(
                  host_at(Progress::waiting_for_seat)),
              nullptr);
    ASSERT_NE(guest.seen.wait_for<GuestVisit::VisitChanged>(
                  guest_at(Progress::waiting_for_seat)),
              nullptr);
  }

  void cancelled() {
    visit->cancel();
    static_cast<void>(visiting->join());
    visiting.reset();
    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome->status(), VisitOutcome::Status::cancelled);
  }

  test_support::play::PublicHousehold home;
  std::unique_ptr<KakusanPair> pair;
  ScriptedLink link{PairTime::threading(), PairTime::steady()};
  test_support::play::PublicMedia media;
  test_support::play::PublicEvents events;
  std::shared_ptr<libchiaking::play::Session> session;
  PairHost host;
  PairGuest guest;
  std::unique_ptr<SharedSession> share;
  libchiaking::kakusan::GuestId guest_id;
  std::unique_ptr<GuestVisit> visit;
  std::optional<VisitOutcome> outcome;
  std::optional<libchiaking::platform::Thread> visiting;
};

TEST_F(KakusanOnRealTime, VerifiesWithoutAConductor) {
  ASSERT_NO_FATAL_FAILURE(verified(guest.media));
  EXPECT_TRUE(share->remember(guest_id, "Guest pad"));
  EXPECT_EQ(host_store.devices().size(), 1U);
  ASSERT_NO_FATAL_FAILURE(cancelled());
  EXPECT_EQ(outcome->code(), VisitOutcome::cancelled_code);
  EXPECT_TRUE(session->leave());
  EXPECT_TRUE(session->wait_until_ended().was_left());
}

TEST_F(KakusanOnRealTime, HandsTheGuestTheStreamHeaderThenTheFrames) {
  const auto shared = ScriptedLink::shared_media();
  PairPicture picture;
  ASSERT_NO_FATAL_FAILURE(verified(picture));

  EXPECT_TRUE(share->give_seat(
      guest_id, {libchiaking::controllers::ControllerKind::dualsense,
                 libchiaking::AccountId{PairStage::guest_account}, seat(1)}));
  libchiaking::platform::Sleeper sleeper{PairTime::threading(),
                                         PairTime::steady()};
  while (link.count("seat_join") == 0)
    sleeper.sleep_for(std::chrono::milliseconds{1});
  link.join(seat(1), {0, 0, 255});
  ASSERT_NE(host.seen.wait_for<SharedSession::PlayerChanged>(
                host_at(Progress::playing)),
            nullptr);
  ASSERT_NE(guest.seen.wait_for<GuestVisit::VisitChanged>(
                guest_at(Progress::playing)),
            nullptr);
  while (link.keyframe_requests() == 0)
    sleeper.sleep_for(std::chrono::milliseconds{1});

  link.send_shared_media();
  const auto units = picture.wait_for_frames(shared.video_frames.size());
  ASSERT_EQ(units.size(), shared.video_frames.size() + 1);
  EXPECT_TRUE(units.front().profile_header);
  EXPECT_EQ(units.front().bytes, shared.description.profiles.front().header);
  EXPECT_EQ(units.front().frame_index, units[1].frame_index);
  for (std::size_t frame = 0; frame < shared.video_frames.size(); ++frame) {
    EXPECT_FALSE(units[frame + 1].profile_header);
    EXPECT_EQ(units[frame + 1].bytes, shared.video_frames[frame]);
  }
  EXPECT_EQ(link.count("shared_media"), 1U);

  ASSERT_NO_FATAL_FAILURE(cancelled());
  EXPECT_TRUE(session->leave());
  EXPECT_TRUE(session->wait_until_ended().was_left());
}

}  // namespace
}  // namespace kakusan_test
