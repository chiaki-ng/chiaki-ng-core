#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>

namespace play_test {
namespace {

using libchiaking::catalog::ServiceGeneration;
using libchiaking::consoles::Generation;
using libchiaking::play::SoundRequest;

static_assert(SoundRequest::supports_3d_on_console(Generation::ps5));
static_assert(!SoundRequest::supports_3d_on_console(Generation::ps4));
static_assert(SoundRequest::supports_3d_on_cloud(ServiceGeneration::ps5_cloud));
static_assert(
    !SoundRequest::supports_3d_on_cloud(ServiceGeneration::old_service));

TEST(SoundRequest, DefaultsToNoChannelsAndNoSurround) {
  EXPECT_EQ(SoundRequest{}.channels(), 0u);
  EXPECT_FALSE(SoundRequest{}.wants_surround());
}

TEST(SoundRequest, AcceptsOnlyTheListedChannelCounts) {
  for (const std::uint32_t channels : {0U, 2U, 6U, 8U, 12U}) {
    SCOPED_TRACE(channels);
    const auto request = SoundRequest::create(channels);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->channels(), channels);
  }
  for (const std::uint32_t channels :
       {1U, 3U, 4U, 5U, 7U, 10U, 13U, 256U, 258U}) {
    SCOPED_TRACE(channels);
    EXPECT_FALSE(SoundRequest::create(channels));
  }
}

TEST(SoundRequest, KeepsControllerSpeakerAudioOnlyOnThePs5Cloud) {
  const auto stereo = *SoundRequest::create(2);
  const auto speaker = stereo.with_controller_speaker(true);
  EXPECT_FALSE(stereo.wants_controller_speaker());
  EXPECT_TRUE(speaker.wants_controller_speaker());
  EXPECT_NE(speaker, stereo);
  EXPECT_EQ(speaker.channels(), 2u);
  EXPECT_EQ(speaker.on_cloud(ServiceGeneration::ps5_cloud), speaker);
  EXPECT_FALSE(speaker.on_cloud(ServiceGeneration::old_service)
                   .wants_controller_speaker());
  EXPECT_FALSE(speaker.on_console(Generation::ps5).wants_controller_speaker());
  EXPECT_EQ(speaker.on_console(Generation::ps5).channels(), 2u);
  EXPECT_FALSE(
      speaker.with_controller_speaker(false).wants_controller_speaker());
  const auto surround = SoundRequest::create(8)->with_controller_speaker(true);
  EXPECT_EQ(surround.on_cloud(ServiceGeneration::old_service), SoundRequest{});
}

TEST(SoundRequest, IsSurroundOnlyOnAPs5Console) {
  const auto stereo = *SoundRequest::create(2);
  const auto surround = *SoundRequest::create(6);
  EXPECT_FALSE(stereo.wants_surround());
  EXPECT_TRUE(surround.wants_surround());
  EXPECT_EQ(surround.on_console(Generation::ps5), surround);
  EXPECT_EQ(stereo.on_console(Generation::ps4), stereo);
  EXPECT_EQ(surround.on_console(Generation::ps4), SoundRequest{});
  EXPECT_EQ(SoundRequest::create(12)->on_console(Generation::ps5).channels(),
            12u);
}

TEST(SoundRequest, IsSurroundOnlyOnThePs5Cloud) {
  for (const auto channels : SoundRequest::channel_counts) {
    SCOPED_TRACE(+channels);
    const auto request = *SoundRequest::create(channels);
    EXPECT_EQ(request.on_cloud(ServiceGeneration::ps5_cloud), request);
    const auto fallback = request.wants_surround() ? SoundRequest{} : request;
    EXPECT_EQ(request.on_cloud(ServiceGeneration::old_service), fallback);
  }
}

}  // namespace
}  // namespace play_test
