#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/shared_session_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace kakusan_test {
namespace {

using libchiaking::kakusan::SharedSession;
using libchiaking::kakusan::SharingRules;
using test_support::play::SessionRig;
using Where = libchiaking::play::Where;

class SharedSessionStart : public SharedSessionTest {
 protected:
  static constexpr std::uint32_t unshareable_version = 7;
};
class SharedSessionStream : public SharedSessionTest {};

TEST_F(SharedSessionStart, SharesASessionOnlyOnce) {
  ShareRig home;
  home.session.start_playing();
  ASSERT_TRUE(home.begin());
  EXPECT_EQ(home.recorder.size(), 0U);
  EXPECT_EQ(home.log.logged("Kakusan is open for this session"), 1U);
  const auto again =
      home.root.share(home.session.session->share(),
                      *SharingRules::create(4, 1), home.recorder);
  EXPECT_TRUE(refused(again, Refusal::Kind::busy));
  EXPECT_EQ(home.recorder.size(), 0U);
}

TEST_F(SharedSessionStart, SharesAnInternetSession) {
  ShareRig away{Where::internet};
  away.session.start_playing();
  EXPECT_TRUE(away.begin());
}

TEST_F(SharedSessionStart, RefusesACloudSessionWithoutTellingTheObserver) {
  ShareRig cloud{Where::cloud,
                 libchiaking::catalog::ServiceGeneration::ps5_cloud};
  cloud.session.start_playing();
  EXPECT_TRUE(refused(cloud.begin(), Refusal::Kind::cannot_be_shared));
  EXPECT_EQ(cloud.share, nullptr);
  EXPECT_EQ(cloud.recorder.size(), 0U);
  EXPECT_EQ(cloud.log.logged(
                "Kakusan is unavailable for this session: CANNOT_BE_SHARED"),
            1U);
  EXPECT_TRUE(refused(cloud.begin(), Refusal::Kind::cannot_be_shared));
  EXPECT_EQ(cloud.recorder.size(), 0U);
}

TEST_F(SharedSessionStart, RefusesASessionThatHasEnded) {
  ShareRig over;
  over.session.start_playing();
  EXPECT_TRUE(over.session.session->leave());
  over.session.session->wait_until_ended();
  EXPECT_TRUE(refused(over.begin(), Refusal::Kind::not_sharing));
  EXPECT_EQ(over.share, nullptr);
}

TEST_F(SharedSessionStart, IsNotSharingUntilTheStreamIsDescribed) {
  ShareRig rig;
  ASSERT_NO_FATAL_FAILURE(rig.play());
  EXPECT_TRUE(refused(rig.share->invite_player(), Refusal::Kind::not_sharing));
  rig.announce_keys();
  EXPECT_EQ(rig.recorder.count<SharedSession::StreamShared>(), 0U);
  EXPECT_TRUE(refused(rig.share->invite_player(), Refusal::Kind::not_sharing));
  rig.session.link.describe(ShareRig::description());
  ASSERT_TRUE(rig.until(
      [&] { return rig.recorder.count<SharedSession::StreamShared>() == 1; }));
  EXPECT_EQ(rig.recorder.find<SharedSession::StreamShared>()->frames_per_second,
            60U);
  EXPECT_TRUE(rig.share->invite_player());
  EXPECT_EQ(rig.recorder.count<SharedSession::SharingFailed>(), 0U);
}

TEST_F(SharedSessionStart, CanBeginAfterTheStreamStarted) {
  ShareRig rig;
  rig.session.start_playing();
  rig.announce_keys();
  rig.session.link.describe(ShareRig::description());
  SessionRig::eventually(
      [&] { return rig.session.media.descriptions().size() == 1; });
  ASSERT_TRUE(rig.begin());
  ASSERT_TRUE(rig.until(
      [&] { return rig.recorder.count<SharedSession::StreamShared>() == 1; }));
  ASSERT_NO_FATAL_FAILURE(rig.seated(0x46));
  EXPECT_EQ(rig.share->players().size(), 1U);
}

TEST_F(SharedSessionStart, LeavesTheSessionAloneWhenRefused) {
  ShareRig rig{Where::cloud,
               libchiaking::catalog::ServiceGeneration::ps5_cloud};
  rig.session.start_playing();
  EXPECT_FALSE(rig.begin());
  rig.session.link.describe(ShareRig::description());
  SessionRig::eventually(
      [&] { return rig.session.media.descriptions().size() == 1; });
  EXPECT_TRUE(
      rig.session.session->controllers().send(SeatNumber::first(), pressed()));
  rig.session.link.feed_back(rumble(SeatNumber::first(), 200, 50));
  SessionRig::eventually(
      [&] { return rig.session.media.feedback().size() == 1; });
  SessionRig::eventually([&] {
    return rig.session.link.controller_samples(SeatNumber::first()).size() == 1;
  });
  EXPECT_EQ(rig.share, nullptr);
  EXPECT_EQ(rig.recorder.size(), 0U);
}

TEST_F(SharedSessionStart, ReportsASessionThatTurnsOutUnshareable) {
  ShareRig rig;
  ASSERT_NO_FATAL_FAILURE(rig.play());
  rig.announce_keys(unshareable_version);
  rig.session.link.describe(ShareRig::description());
  ASSERT_TRUE(rig.until(
      [&] { return rig.recorder.count<SharedSession::SharingFailed>() == 1; }));
  EXPECT_EQ(rig.recorder.find<SharedSession::SharingFailed>()->refusal,
            Refusal::Kind::unsupported_version);
  EXPECT_EQ(rig.recorder.count<SharedSession::StreamShared>(), 0U);
  EXPECT_TRUE(refused(rig.share->invite_player(), Refusal::Kind::not_sharing));
  EXPECT_EQ(rig.log.logged("Kakusan cannot share this session"), 1U);
}

TEST_F(SharedSessionStart, EndsWithTheSession) {
  ShareRig rig;
  ASSERT_NO_FATAL_FAILURE(rig.play());
  ASSERT_NO_FATAL_FAILURE(rig.stream());
  ASSERT_NO_FATAL_FAILURE(rig.seated(0x41));

  EXPECT_TRUE(rig.session.session->leave());
  rig.session.session->wait_until_ended();
  EXPECT_EQ(rig.recorder.count<SharedSession::SharingEnded>(), 1U);
  const auto* last = rig.recorder.state();
  ASSERT_NE(last, nullptr);
  EXPECT_GE(last->progress, manual::Progress::leaving);
  EXPECT_EQ(last->ended, libchiaking::kakusan::EndReason::Kind::session_ended);
  const auto events = rig.recorder.size();
  for (std::size_t index = 0; index < 50; ++index) {
    rig.pump();
    nap(std::chrono::milliseconds{1});
  }
  EXPECT_EQ(rig.recorder.size(), events);
}

TEST_F(SharedSessionStream, DescriptionDecidesTheSharedFrameRate) {
  ShareRig rig;
  ASSERT_NO_FATAL_FAILURE(rig.play());
  rig.announce_keys();
  rig.session.link.describe(ShareRig::description(30));
  ASSERT_TRUE(rig.until(
      [&] { return rig.recorder.count<SharedSession::StreamShared>() == 1; }));
  rig.session.link.describe(ShareRig::description(60));
  ASSERT_TRUE(rig.until([&] {
    return rig.log.logged("Kakusan is sharing the stream at 60 fps") == 1;
  }));
  auto unusable = ShareRig::description(60);
  unusable.profiles.front().header.resize(
      manual::Host::max_profile_header_size + 1);
  rig.session.link.describe(std::move(unusable));
  ASSERT_TRUE(rig.until(
      [&] { return rig.recorder.count<SharedSession::SharingFailed>() == 1; }));
  EXPECT_EQ(rig.log.logged("Kakusan is sharing the stream at 30 fps"), 1U);
  EXPECT_EQ(rig.log.logged("Kakusan is sharing the stream at 60 fps"), 1U);
  EXPECT_EQ(rig.log.logged("Kakusan could not share the stream: SOURCE_FORMAT"),
            1U);
  EXPECT_EQ(rig.session.media.descriptions().size(), 3U);
  EXPECT_EQ(rig.recorder.find<SharedSession::SharingFailed>()->refusal,
            Refusal::Kind::stream_not_shareable);
}

}  // namespace
}  // namespace kakusan_test
