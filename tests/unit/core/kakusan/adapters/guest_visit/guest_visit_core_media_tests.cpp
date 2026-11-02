#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/guest_visit/guest_visit_core_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace kakusan_test {

using test_support::play::SourceStream;
namespace {

namespace play = libchiaking::play;
namespace wire = libchiaking::net::kakusan;

class GuestVisitCoreMedia : public GuestVisitCoreTest {
 protected:
  Rig rig;
};

TEST_F(GuestVisitCoreMedia, SourceVectorsDecodeThroughAForwardedStream) {
  ForwardedMedia shown;
  std::uint64_t now = 1000;
  TrackedClock clock{now};
  libchiaking::platform::DefaultPlatformFactory platform{
      libchiaking::platform::PlatformProviders{.clock = &clock,
                                               .crypto = &crypto()}};
  auto receiver =
      play::ForwardedStream::open(platform, SourceVectors::shared_stream(),
                                  SourceStream::receive_keys(), shown);
  ASSERT_TRUE(receiver);
  for (std::size_t index = 0; index < SourceStream::vectors.size();
       ++index) {
    const auto& vector = SourceStream::vectors[index];
    const auto bytes = SourceStream::datagram(index);
    play::SharedDatagram datagram;
    datagram.media = vector.media;
    datagram.key_position = vector.key_position;
    datagram.bytes = bytes;
    EXPECT_TRUE(receiver.value()->receive(datagram));
  }
  EXPECT_FALSE(receiver.value()->wants_keyframe());
  expect_source_media(shown);
}

TEST_F(GuestVisitCoreMedia, AsksForAnIdrWhenAFrameIsLost) {
  rig.activate();
  const auto lost = SourceStream::vectors[3].key_position;
  rig.wire_link().drop = [lost](TransportRole, Lane lane,
                                const std::vector<std::byte>& bytes) {
    return lane == Lane::datagram && media_key_position(bytes) == lost;
  };
  for (std::size_t index = 0; index < SourceStream::vectors.size(); ++index)
    rig.forward(index);
  rig.run(100);
  EXPECT_TRUE(rig.host_got_idr());
  EXPECT_GE(rig.idr_requests, 1U);
  EXPECT_EQ(rig.guest->state(), Progress::playing);
  EXPECT_EQ(rig.media.video.size(), SourceStream::video_frames.size() - 1);
}

TEST_F(GuestVisitCoreMedia, RefusesAnUnsupportedAudioSetup) {
  rig.setup.audio_header[0] = std::byte{3};
  rig.connect();
  rig.pair();
  rig.run(300);
  EXPECT_EQ(rig.count<GuestVisit::Core::StreamReady>(), 0U);
  EXPECT_EQ(rig.count<GuestVisit::Core::StreamStopped>(), 0U);
  EXPECT_EQ(rig.count<GuestVisit::Core::ControllerRequested>(), 0U);
  EXPECT_TRUE(rig.host_got_failure(Error::codec));
  EXPECT_EQ(rig.guest->state(), Progress::gone);
  EXPECT_EQ(rig.last_state().error, Error::codec);
  EXPECT_FALSE(rig.guest->next_deadline_us());
  rig.run(1100);
  EXPECT_EQ(rig.host.state(), Progress::gone);
  EXPECT_EQ(rig.host_shown, Error::codec);
}

TEST_F(GuestVisitCoreMedia, RefusesAVideoProfileWithoutParameterSets) {
  rig.setup.video_profiles.front().header =
      test_support::Bytes::unhex("0000000168ebe3cb22c0");
  rig.connect();
  rig.pair();
  rig.run(300);
  EXPECT_EQ(rig.count<GuestVisit::Core::StreamReady>(), 0U);
  EXPECT_TRUE(rig.host_got_failure(Error::codec));
  EXPECT_EQ(rig.last_state().error, Error::codec);
}

TEST_F(GuestVisitCoreMedia, EndsOnASourceThatFailsAuthentication) {
  rig.activate();
  auto tampered = SourceStream::datagram(1);
  tampered[11] ^= std::byte{0x01};
  rig.forward_raw(wire::MediaKind::audio,
                  SourceStream::vectors[1].key_position, tampered);
  rig.run(30);
  EXPECT_TRUE(rig.host_got_failure(Error::source_format));
  EXPECT_EQ(rig.guest->state(), Progress::gone);
  EXPECT_EQ(rig.last_state().error, Error::source_format);
  EXPECT_EQ(rig.count<GuestVisit::Core::StreamStopped>(), 1U);
  EXPECT_TRUE(rig.media.empty());
  EXPECT_FALSE(rig.guest->next_deadline_us());
}

TEST_F(GuestVisitCoreMedia, EndsOnASourceFramedAsTheWrongKind) {
  rig.activate();
  rig.forward_raw(wire::MediaKind::audio,
                  SourceStream::vectors[0].key_position,
                  SourceStream::datagram(0));
  rig.run(30);
  EXPECT_TRUE(rig.host_got_failure(Error::source_format));
  EXPECT_EQ(rig.last_state().error, Error::source_format);
  EXPECT_EQ(rig.guest->state(), Progress::gone);
}

TEST_F(GuestVisitCoreMedia, EndsOnASourceAtAShiftedKeyPosition) {
  rig.activate();
  rig.forward_raw(
      wire::MediaKind::audio,
      SourceStream::vectors[1].key_position + (std::uint64_t{1} << 32U),
      SourceStream::datagram(1));
  rig.run(30);
  EXPECT_TRUE(rig.host_got_failure(Error::source_format));
  EXPECT_EQ(rig.guest->state(), Progress::gone);
}

TEST_F(GuestVisitCoreMedia, IgnoresForeignMedia) {
  rig.activate();
  auto raw = std::make_unique<test_support::kakusan::LoopbackTransport>(
      rig.exchange->links.front(), TransportRole::host);
  std::vector<std::byte> forged(120, std::byte{0x5a});
  forged[0] = std::byte{'R'};
  forged[1] = std::byte{'C'};
  forged[2] = std::byte{'P'};
  forged[3] = std::byte{'M'};
  EXPECT_TRUE(raw->send_datagram(forged));
  rig.run(30);
  EXPECT_EQ(rig.guest->state(), Progress::playing);
  EXPECT_TRUE(rig.media.empty());
  EXPECT_EQ(rig.acks, 0U);
}

}  // namespace
}  // namespace kakusan_test
