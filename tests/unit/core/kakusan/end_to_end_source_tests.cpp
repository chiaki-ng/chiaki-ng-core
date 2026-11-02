#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/end_to_end_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string_view>

namespace kakusan_test {

using test_support::play::SourceStream;
namespace {

using libchiaking::kakusan::SharingRules;
using libchiaking::net::kakusan::Error;

class SourceEndToEnd : public EndToEndTest {
 protected:
  static constexpr std::string_view h265_header =
      "0000000140010c01ffff016000000300b000000300000300781702400000000142010101"
      "60"
      "0000"
      "0300b0"
      "0000030000030078a003c0801107cb8817b916452ffcb9fc4feb016a0404040100000001"
      "44"
      "01c0"
      "72f05324";

  static auto h265_description() -> libchiaking::media::StreamDescription {
    auto info = SourceStream::description();
    info.profiles.front().width = 1920;
    info.profiles.front().height = 1080;
    info.profiles.front().header = test_support::Bytes::unhex(h265_header);
    return info;
  }
};

TEST_F(SourceEndToEnd, CarriesAnH265SourceToTheGuest) {
  World world;
  const auto info = h265_description();
  world.start_source(info);
  auto& player = world.join(0x40);
  ASSERT_TRUE(player.info);
  EXPECT_EQ(player.info->profiles, info.profiles);
  world.forward_source();
  const auto audio = player.media.audio;
  EXPECT_EQ(audio.size(), SourceStream::audio_frames.size());
  ASSERT_EQ(player.media.headers.size(), 1U);
  EXPECT_EQ(player.media.headers.front(), test_support::Bytes::unhex(h265_header));
  const auto video = player.media.video;
  EXPECT_EQ(video.size(), SourceStream::video_frames.size());
}

TEST_F(SourceEndToEnd, RefusesAnHdrSourceAndStopsInviting) {
  World hdr;
  auto info = h265_description();
  info.picture = libchiaking::media::GrantedPicture{
      libchiaking::media::VideoCodec::h265_hdr, 1920, 1080, 60, 15000};
  hdr.host->source_keys(SourceStream::takion_version,
                        SourceStream::receive_keys());
  const auto refused =
      hdr.host->source_started(info, SourceStream::video_fps, hdr.now);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), Error::codec);
  EXPECT_FALSE(hdr.host->invite(hdr.clock()));
}

class NewerTakionEndToEnd
    : public EndToEndTest,
      public ::testing::WithParamInterface<std::uint32_t> {};

TEST_P(NewerTakionEndToEnd, CarriesTheSourceToTheGuest) {
  World world;
  world.takion_version = GetParam();
  world.start_source();
  auto& player = world.join(0x40);
  ASSERT_TRUE(player.info);
  EXPECT_EQ(player.fps, SourceStream::video_fps);
  world.forward_source();
  expect_decoded(player);
}

INSTANTIATE_TEST_SUITE_P(TakionVersions, NewerTakionEndToEnd,
                         ::testing::Values(15U, 20U));

class DeadEntropyEndToEnd : public EndToEndTest {
 protected:
  class DeadEntropy final : public libchiaking::platform::SecureRandomProvider {
   public:
    auto fill_bytes(std::span<std::byte> output) -> libchiaking::Expected<
        void, libchiaking::platform::PlatformError> override {
      std::ranges::fill(output, std::byte{});
      return libchiaking::unexpected(
          libchiaking::platform::PlatformError::system_failure);
    }
  };

  DeadEntropy dead;
};

TEST_F(DeadEntropyEndToEnd, StopsTheHostFromStarting) {
  auto exchange =
      std::make_shared<test_support::kakusan::LoopbackExchange>(crypto());
  test_support::kakusan::LoopbackFactory factory{exchange};
  const auto host = SharedSession::Core::create(
      SharedSession::Core::Dependencies{crypto(), dead, factory,
                                        test_support::process_threading()},
      identity(0x10),
      *SharingRules::create(4, 1, libchiaking::net::kakusan::button_mask));
  ASSERT_FALSE(host);
  EXPECT_EQ(host.error(), Error::crypto_unavailable);
}

TEST_F(DeadEntropyEndToEnd, StopsTheGuestFromAcceptingAnInvitation) {
  auto exchange =
      std::make_shared<test_support::kakusan::LoopbackExchange>(crypto());
  test_support::kakusan::LoopbackFactory factory{exchange};
  World world;
  world.start_source();
  const auto guest = world.host->invite(world.clock());
  ASSERT_TRUE(guest);
  world.until([&] { return !world.invitations.empty(); });
  ASSERT_FALSE(world.invitations.empty());
  ForwardedMedia media;
  TrackedClock clock{world.now};
  libchiaking::platform::DefaultPlatformFactory platform{
      libchiaking::platform::PlatformProviders{.clock = &clock,
                                               .crypto = &crypto()}};
  auto session = GuestVisit::Core::create(
      GuestVisit::Core::Dependencies{platform, dead, factory, media},
      identity(0x40));
  EXPECT_TRUE(session);
  test_support::kakusan::LoopbackFactory paired{world.exchange};
  auto joining =
      GuestVisit::Core::create(
          GuestVisit::Core::Dependencies{platform, dead, paired, media},
          identity(0x40))
          .value();
  const auto refused = joining->accept_invitation(
      world.invitations.back().block.view(), world.clock());
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), Error::crypto_unavailable);
  EXPECT_EQ(joining->state(), Progress::waiting);
  EXPECT_FALSE(joining->take_output());
}

}  // namespace
}  // namespace kakusan_test
