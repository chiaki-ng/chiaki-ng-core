#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_share_case.hpp"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

namespace play_test {

using test_support::play::SessionRig;
namespace {

namespace play = libchiaking::play;
using play::GuestTag;
using play::NotShareable;
using play::SeatRefusal;
using play::SessionShare;
using play::Where;

using PlaySessionShare = ShareCase;

TEST_F(PlaySessionShare, SharesOnlyKnownProtocolVersions) {
  EXPECT_TRUE(SessionShare::shares_protocol_version(shareable_version));
  EXPECT_TRUE(SessionShare::shares_protocol_version(9));
  EXPECT_TRUE(SessionShare::shares_protocol_version(19));
  EXPECT_FALSE(SessionShare::shares_protocol_version(unshareable_version));
  EXPECT_FALSE(SessionShare::shares_protocol_version(4096));
}

TEST_F(PlaySessionShare, SharesAConsoleSessionUntilItEnds) {
  for (const auto where : {Where::home, Where::internet}) {
    SCOPED_TRACE(testing::PrintToString(where));
    SessionRig rig{where};
    RecordedAudience audience;
    EXPECT_TRUE(rig.session->share().can_be_shared());
    rig.link.report_protocol_version(shareable_version);
    rig.start_playing();
    EXPECT_TRUE(rig.session->share().can_be_shared());
    auto sharing = rig.session->share().begin(audience);
    EXPECT_TRUE(sharing);
    EXPECT_TRUE(rig.session->leave());
    rig.session->wait_until_ended();
    EXPECT_EQ(rig.session->share().can_be_shared().error(),
              NotShareable::session_ended);
    RecordedAudience late;
    EXPECT_EQ(rig.session->share().begin(late).error(),
              NotShareable::session_ended);
  }
}

TEST_F(PlaySessionShare, NeverSharesACloudSession) {
  SessionRig cloud{Where::cloud,
                   libchiaking::catalog::ServiceGeneration::ps5_cloud};
  RecordedAudience audience;
  EXPECT_EQ(cloud.session->share().can_be_shared().error(),
            NotShareable::cloud_session);
  EXPECT_EQ(cloud.session->share().begin(audience).error(),
            NotShareable::cloud_session);
  cloud.start_playing();
  EXPECT_EQ(cloud.session->share().begin(audience).error(),
            NotShareable::cloud_session);
  EXPECT_EQ(cloud.session->share().seat_guest({GuestTag{1}}).error().kind(),
            SeatRefusal::Kind::not_sent);
}

TEST_F(PlaySessionShare, RefusesAReportedOldProtocol) {
  SessionRig reported;
  RecordedAudience audience;
  reported.link.report_protocol_version(unshareable_version);
  reported.start_playing();
  SessionRig::eventually([&] {
    const auto verdict = reported.session->share().can_be_shared();
    return !verdict && verdict.error() == NotShareable::protocol_version;
  });
  EXPECT_EQ(reported.session->share().begin(audience).error(),
            NotShareable::protocol_version);
}

TEST_F(PlaySessionShare, RefusesKeysAnnouncedForAnOldProtocol) {
  SessionRig keyed;
  RecordedAudience audience;
  keyed.start_playing();
  announce(keyed, unshareable_version);
  SessionRig::eventually(
      [&] { return !keyed.session->share().can_be_shared(); });
  EXPECT_EQ(keyed.session->share().can_be_shared().error(),
            NotShareable::protocol_version);
  EXPECT_EQ(keyed.session->share().begin(audience).error(),
            NotShareable::protocol_version);
  EXPECT_EQ(audience.count("stream_started"), 0U);
  EXPECT_EQ(audience.count("session_ended"), 0U);
}

TEST_F(PlaySessionShare, TakesOneAudienceAtATime) {
  SessionRig rig;
  rig.start_playing();
  auto& share = rig.session->share();
  RecordedAudience first;
  RecordedAudience second;
  auto sharing = share.begin(first);
  ASSERT_TRUE(sharing);
  EXPECT_EQ(share.begin(second).error(), NotShareable::already_shared);
  EXPECT_EQ(share.begin(first).error(), NotShareable::already_shared);

  sharing.value().stop();
  sharing.value().stop();
  {
    auto again = share.begin(second);
    ASSERT_TRUE(again);
    EXPECT_EQ(share.begin(first).error(), NotShareable::already_shared);
    play::Sharing moved{std::move(again.value())};
    EXPECT_EQ(share.begin(first).error(), NotShareable::already_shared);
  }
  auto third = share.begin(first);
  ASSERT_TRUE(third);
  auto fourth = share.begin(second);
  EXPECT_FALSE(fourth);
  third.value() = play::Sharing{nullptr};
  EXPECT_TRUE(share.begin(second));
}

TEST_F(PlaySessionShare, TellsItsAudienceWhenItTurnsOutUnshareable) {
  SessionRig rig;
  auto& share = rig.session->share();
  RecordedAudience audience;
  auto sharing = share.begin(audience);
  ASSERT_TRUE(sharing);
  rig.start_playing();
  audience.until("service", 3);
  EXPECT_EQ(audience.count("became_unshareable"), 0U);

  announce(rig, unshareable_version);
  audience.until("became_unshareable");
  rig.link.describe(shared_description());
  SessionRig::eventually([&] { return rig.media.descriptions().size() == 1; });
  audience.until("service", audience.count("service") + 5);
  EXPECT_EQ(audience.unshareable(),
            std::vector{NotShareable::protocol_version});
  EXPECT_EQ(audience.count("stream_started"), 0U);
  EXPECT_EQ(audience.count("session_ended"), 0U);
  EXPECT_EQ(share.can_be_shared().error(), NotShareable::protocol_version);
  EXPECT_EQ(audience.threads(), rig.events.threads());

  announce(rig, unshareable_version, 50);
  audience.until("service", audience.count("service") + 5);
  EXPECT_EQ(audience.count("became_unshareable"), 1U);

  RecordedAudience refused;
  EXPECT_EQ(share.begin(refused).error(), NotShareable::protocol_version);
  sharing.value().stop();
  EXPECT_EQ(share.begin(refused).error(), NotShareable::protocol_version);
  EXPECT_EQ(refused.count("became_unshareable"), 0U);

  announce(rig, shareable_version, 100);
  SessionRig::eventually([&] { return share.can_be_shared().has_value(); });
  RecordedAudience welcome;
  auto again = share.begin(welcome);
  ASSERT_TRUE(again);
  welcome.until("stream_started");
  welcome.until("service", welcome.count("service") + 5);
  EXPECT_EQ(welcome.count("became_unshareable"), 0U);
  ASSERT_FALSE(welcome.started().empty());
  EXPECT_TRUE(same_keys(welcome.started().front(), receive_keys(100)));

  announce(rig, unshareable_version, 150);
  welcome.until("became_unshareable");
  welcome.until("service", welcome.count("service") + 5);
  EXPECT_EQ(welcome.count("became_unshareable"), 1U);
  EXPECT_EQ(welcome.count("stream_started"), 1U);
  EXPECT_EQ(audience.count("became_unshareable"), 1U);
}

}  // namespace
}  // namespace play_test
