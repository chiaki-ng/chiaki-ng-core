#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_share_case.hpp"

#include <gtest/gtest.h>

#include <optional>

namespace play_test {

using test_support::LiveProviders;
namespace {

namespace play = libchiaking::play;
using play::SessionShare;
using play::Where;

using PlaySessionShareStream = ShareCase;

TEST_F(PlaySessionShareStream, StartsOnceTheDescriptionAndKeysArrive) {
  auto requested =
      SessionShare::Core::create(LiveProviders::threading(), LiveProviders::steady(), {Where::internet, 30});
  SessionRig rig{Where::internet, std::nullopt, requested};
  EXPECT_EQ(&rig.core->share_rules(), requested.get());
  RecordedAudience audience;
  auto sharing = rig.session->share().begin(audience);
  ASSERT_TRUE(sharing);
  rig.start_playing();
  audience.until("service", 3);
  EXPECT_EQ(audience.count("stream_started"), 0U);

  announce(rig);
  audience.until("service", audience.count("service") + 3);
  EXPECT_EQ(audience.count("stream_started"), 0U);
  rig.link.describe(shared_description(false));
  audience.until("stream_started");
  audience.until("service", audience.count("service") + 5);
  EXPECT_EQ(audience.count("stream_started"), 1U);
  ASSERT_FALSE(audience.started().empty());
  const auto started = audience.started().front();
  EXPECT_EQ(started.stream.protocol_version, shareable_version);
  EXPECT_EQ(started.stream.where, Where::internet);
  EXPECT_EQ(started.stream.description, shared_description(false));
  EXPECT_FALSE(started.stream.hevc);
  EXPECT_EQ(started.stream.frames_per_second, 30U);
  EXPECT_TRUE(same_keys(started, receive_keys()));
  ASSERT_EQ(rig.media.descriptions().size(), 1U);
  EXPECT_EQ(rig.media.descriptions().front(), shared_description(false));
  EXPECT_EQ(audience.threads(), rig.events.threads());
}

TEST_F(PlaySessionShareStream, StartsAgainWithNewKeys) {
  SessionRig rig{
      Where::internet, std::nullopt,
      SessionShare::Core::create(LiveProviders::threading(), LiveProviders::steady(), {Where::internet, 30})};
  RecordedAudience audience;
  auto sharing = rig.session->share().begin(audience);
  ASSERT_TRUE(sharing);
  rig.start_playing();
  announce(rig);
  rig.link.describe(shared_description(false));
  audience.until("stream_started");

  announce(rig, shareable_version, 100);
  audience.until("stream_started", 2);
  EXPECT_TRUE(same_keys(audience.started().back(), receive_keys(100)));
  audience.until("service", audience.count("service") + 5);
  EXPECT_EQ(audience.count("stream_started"), 2U);
}

TEST_F(PlaySessionShareStream, TellsALateAudienceWhatAlreadyHappened) {
  SessionRig rig;
  rig.start_playing();
  announce(rig);
  rig.link.describe(shared_description());
  SessionRig::eventually([&] { return rig.media.descriptions().size() == 1; });

  RecordedAudience late;
  auto sharing = rig.session->share().begin(late);
  ASSERT_TRUE(sharing);
  late.until("stream_started");
  late.until("service", late.count("service") + 5);
  EXPECT_EQ(late.count("stream_started"), 1U);
  ASSERT_FALSE(late.started().empty());
  const auto started = late.started().front();
  EXPECT_EQ(started.stream.description, shared_description());
  EXPECT_TRUE(started.stream.hevc);
  EXPECT_EQ(started.stream.frames_per_second, 60U);
  EXPECT_EQ(started.stream.where, Where::home);
  EXPECT_TRUE(same_keys(started, receive_keys()));
  EXPECT_EQ(late.threads(), rig.events.threads());

  sharing.value().stop();
  RecordedAudience later;
  auto again = rig.session->share().begin(later);
  ASSERT_TRUE(again);
  later.until("stream_started");
  later.until("service", later.count("service") + 5);
  EXPECT_EQ(later.count("stream_started"), 1U);
  EXPECT_EQ(late.count("stream_started"), 1U);
  ASSERT_FALSE(later.started().empty());
  EXPECT_TRUE(same_keys(later.started().front(), receive_keys()));
}

TEST_F(PlaySessionShareStream, AsksForAKeyframeAndWakesTheLoopWhileRunning) {
  SessionRig rig;
  auto& share = rig.session->share();
  share.request_keyframe();
  share.wake();
  rig.start_playing();
  EXPECT_EQ(rig.link.keyframe_requests(), 0U);
  share.request_keyframe();
  SessionRig::eventually([&] { return rig.link.keyframe_requests() == 1; });
  RecordedAudience audience;
  auto sharing = share.begin(audience);
  ASSERT_TRUE(sharing);
  const auto before = audience.count("service");
  share.wake();
  audience.until("service", before + 1);
  EXPECT_TRUE(rig.session->leave());
  rig.session->wait_until_ended();
  share.request_keyframe();
  share.wake();
  EXPECT_EQ(rig.link.keyframe_requests(), 1U);
}

}  // namespace
}  // namespace play_test
