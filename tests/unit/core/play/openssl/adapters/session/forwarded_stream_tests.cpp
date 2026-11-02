#include "src/core/net/protocol/takion/source_tap.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/play/session_rig.hpp"
#include "tests/unit/core/play/shared_audience.hpp"
#include "tests/support/play/source_stream.hpp"

#include <libchiaking/platform/crypto/openssl/provider.hpp>
#include <libchiaking/platform/default_factory.hpp>
#include <libchiaking/platform/factory.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test {

using test_support::play::RecordedMedia;
using test_support::play::SessionRig;

using test_support::play::SourceStream;
namespace {

namespace media = libchiaking::media;
namespace play = libchiaking::play;
namespace takion = libchiaking::net::protocol::takion;
using play::ForwardedStream;
using play::SharedDatagram;
using Refusal = ForwardedStream::Refusal;

class ForwardedStreamTest : public ::testing::Test {
 protected:
  struct Vector {
    std::uint64_t key_position;
    std::string_view datagram;
    std::string_view plaintext;
  };

  static constexpr std::uint32_t version = 12;
  static constexpr std::array<Vector, 4> vectors{
      Vector{44992ULL, "03000700090000030105c28df95d0000afc0000013d880",
             "f8fffe"},
      Vector{45008ULL, "030008000a000003010542e5ccf40000afd0000047d22b",
             "010203"},
      Vector{4294967328ULL, "030009000b0000030105586404830000002000008c4006",
             "a0b1c2"},
      Vector{90016ULL, "03000a000c00000301057f4cd4ce00015fa00000599313",
             "ffeedd"},
  };

  static auto bytes(std::string_view hex) -> std::vector<std::byte> {
    return test_support::Bytes::unhex(hex);
  }

  static auto source_keys(std::byte difference = {})
      -> takion::SourceReceiveKeys {
    libchiaking::SecretArray<takion::source_key_size> key;
    libchiaking::SecretArray<takion::source_key_size> iv;
    for (std::size_t index = 0; index < takion::source_key_size; ++index) {
      key[index] = static_cast<std::byte>(index);
      iv[index] = static_cast<std::byte>(16 + index);
    }
    key[0] ^= difference;
    return takion::SourceReceiveKeys{std::move(key), std::move(iv)};
  }

  static auto receive_keys(std::byte difference = {}) -> play::ReceiveKeys {
    return play::SessionShare::Core::copied(source_keys(difference));
  }

  static auto description() -> media::StreamDescription {
    media::StreamDescription description;
    description.audio.channels = 2;
    description.audio.bits = 16;
    description.audio.rate = 48000;
    description.audio.frame_size = 960;
    description.profiles.push_back(
        {1280, 720,
         bytes("000000016764002aacd9405005bb0110000003001000000780f1831960000"
               "0000168ebe3cb22c0")});
    description.picture =
        media::GrantedPicture{media::VideoCodec::h264, 1280, 720, 60, 10000};
    const auto header = bytes("02100000bb80000003c000000001");
    std::ranges::copy(header, description.audio_header.begin());
    return description;
  }

  static auto shared_stream(std::uint32_t protocol_version = version)
      -> play::SharedStream {
    return {protocol_version, play::Where::home, description(), false, 60};
  }

  static auto sealed(const Vector& vector, const std::vector<std::byte>& wire)
      -> SharedDatagram {
    SharedDatagram datagram;
    datagram.media = SharedDatagram::Media::audio;
    datagram.key_position = vector.key_position;
    datagram.bytes = wire;
    return datagram;
  }

  auto open(play::ReceiveKeys keys = receive_keys()) {
    return ForwardedStream::open(platform_, shared_stream(), std::move(keys),
                                 shown_);
  }

  libchiaking::platform::openssl::OpenSslCryptoProvider crypto_;
  libchiaking::platform::DefaultPlatformFactory platform_{&crypto_};
  RecordedMedia shown_;
};

TEST_F(ForwardedStreamTest, TurnsAHostsSharedStreamBackIntoUnitsOnTheGuest) {
  SessionRig host;
  RecordedAudience audience;
  auto sharing = host.session->share().begin(audience);
  ASSERT_TRUE(sharing);
  host.start_playing();
  host.link.then([&host](play::testing::ScriptedLink&) {
    host.core->share_rules().tap().on_source_keys(version, source_keys());
  });
  host.link.describe(description());
  audience.until("stream_started");
  auto started = audience.started().front();
  EXPECT_EQ(started.stream.protocol_version, version);
  EXPECT_EQ(started.stream.description, description());
  EXPECT_FALSE(started.stream.hevc);
  EXPECT_EQ(started.stream.frames_per_second, 60);

  std::vector<std::vector<std::byte>> wire;
  for (const auto& vector : vectors) wire.push_back(bytes(vector.datagram));
  host.link.then([&](play::testing::ScriptedLink&) {
    auto& tap = host.core->share_rules().tap();
    for (std::size_t index = 0; index < vectors.size(); ++index) {
      takion::SourceDatagram datagram;
      datagram.media = takion::SourceMedia::audio;
      datagram.key_position = vectors[index].key_position;
      datagram.bytes = wire[index];
      tap.on_source_datagram(datagram);
    }
  });
  audience.until_datagrams(vectors.size());

  libchiaking::SecretArray<play::ReceiveKeys::size> key;
  libchiaking::SecretArray<play::ReceiveKeys::size> iv;
  std::ranges::copy(started.key, key.begin());
  std::ranges::copy(started.iv, iv.begin());
  auto guest = ForwardedStream::open(
      platform_, std::move(started.stream),
      play::ReceiveKeys{std::move(key), std::move(iv)}, shown_);
  ASSERT_TRUE(guest);
  const auto described = shown_.descriptions();
  ASSERT_EQ(described.size(), 1U);
  EXPECT_EQ(described.front().profiles, description().profiles);
  EXPECT_EQ(described.front().audio.channels, 2);
  EXPECT_EQ(described.front().audio.bits, 16);
  EXPECT_EQ(described.front().audio.rate, 48000U);
  EXPECT_EQ(described.front().audio.frame_size, 960U);
  EXPECT_EQ(described.front().picture, description().picture);
  EXPECT_EQ(described.front().audio_header, description().audio_header);

  for (std::size_t index = 0; index < vectors.size(); ++index) {
    SCOPED_TRACE(index);
    const auto forwarded = audience.datagram(index).view();
    EXPECT_EQ(forwarded.key_position, vectors[index].key_position);
    EXPECT_TRUE(guest.value()->receive(forwarded));
  }
  const auto sounds = shown_.sounds();
  ASSERT_EQ(sounds.size(), vectors.size());
  for (std::size_t index = 0; index < vectors.size(); ++index) {
    SCOPED_TRACE(index);
    EXPECT_EQ(sounds[index].kind, media::AudioUnit::Kind::samples);
    EXPECT_EQ(sounds[index].bytes, bytes(vectors[index].plaintext));
  }
  EXPECT_TRUE(shown_.shown().empty());
  EXPECT_EQ(shown_.gaps(), 0U);
  EXPECT_FALSE(guest.value()->fault());
  EXPECT_FALSE(guest.value()->wants_keyframe());
  const auto idle = guest.value()->service(std::chrono::milliseconds{8});
  ASSERT_TRUE(idle);
  EXPECT_EQ(idle.value(), std::chrono::milliseconds{8});
  const auto statistics = guest.value()->statistics();
  EXPECT_EQ(statistics.units, 0U);
  EXPECT_EQ(statistics.lost, 0U);
  EXPECT_EQ(statistics.keyframe_requests, 0U);
  EXPECT_EQ(guest.value()->video_presented().frames_lost, 0U);
}

TEST_F(ForwardedStreamTest, GivesBackTheVideoFramesTheConsoleSent) {
  auto guest = ForwardedStream::open(
      platform_,
      play::SharedStream{SourceStream::takion_version, play::Where::home,
                         SourceStream::description(), false, 60},
      SourceStream::receive_keys(), shown_);
  ASSERT_TRUE(guest);
  for (std::size_t index = 0; index < SourceStream::vectors.size();
       ++index) {
    SCOPED_TRACE(index);
    const auto datagram_bytes = SourceStream::datagram(index);
    SharedDatagram datagram;
    datagram.media = SourceStream::vectors[index].media;
    datagram.key_position = SourceStream::vectors[index].key_position;
    datagram.bytes = datagram_bytes;
    EXPECT_TRUE(guest.value()->receive(datagram));
  }

  const auto header = test_support::Bytes::unhex(SourceStream::video_header);
  const auto units = shown_.shown();
  ASSERT_EQ(units.size(), SourceStream::video_frames.size() + 1);
  EXPECT_EQ(units.front().bytes, header);
  EXPECT_EQ(units.front().header, header);
  EXPECT_TRUE(units.front().profile_changed);
  EXPECT_FALSE(units.front().hevc);
  for (std::size_t index = 0; index < SourceStream::video_frames.size();
       ++index) {
    SCOPED_TRACE(index);
    const auto& unit = units[index + 1];
    EXPECT_EQ(unit.bytes,
              test_support::Bytes::unhex(SourceStream::video_frames[index]));
    EXPECT_TRUE(unit.header.empty());
    EXPECT_FALSE(unit.profile_changed);
    EXPECT_FALSE(unit.hevc);
    EXPECT_EQ(unit.frame_index, index + 1);
    EXPECT_EQ(unit.width, SourceStream::video_width);
    EXPECT_EQ(unit.height, SourceStream::video_height);
    EXPECT_EQ(unit.padded, unit.bytes.size() + media::VideoFrame::tail_padding);
  }
  std::vector<std::vector<std::byte>> heard;
  for (const auto& sound : shown_.sounds())
    if (sound.kind == media::AudioUnit::Kind::samples)
      heard.push_back(sound.bytes);
  ASSERT_EQ(heard.size(), SourceStream::audio_frames.size());
  for (std::size_t index = 0; index < heard.size(); ++index)
    EXPECT_EQ(heard[index],
              test_support::Bytes::unhex(SourceStream::audio_frames[index]))
        << index;
  EXPECT_EQ(shown_.gaps(), 0U);
  EXPECT_FALSE(guest.value()->fault());
  EXPECT_FALSE(guest.value()->wants_keyframe());
  const auto statistics = guest.value()->statistics();
  EXPECT_EQ(statistics.units, units.size());
  EXPECT_EQ(statistics.lost, 0U);
  EXPECT_EQ(statistics.keyframe_requests, 0U);
  EXPECT_EQ(guest.value()->video_presented().frames_lost, 0U);
}

TEST_F(ForwardedStreamTest, PutsDatagramsOutOfOrderBackInOrder) {
  auto guest = open();
  ASSERT_TRUE(guest);
  for (const std::size_t index :
       {std::size_t{2}, std::size_t{0}, std::size_t{1}, std::size_t{1},
        std::size_t{3}}) {
    SCOPED_TRACE(index);
    EXPECT_TRUE(guest.value()->receive(
        sealed(vectors[index], bytes(vectors[index].datagram))));
  }
  const auto sounds = shown_.sounds();
  ASSERT_EQ(sounds.size(), vectors.size());
  for (std::size_t index = 0; index < vectors.size(); ++index)
    EXPECT_EQ(sounds[index].bytes, bytes(vectors[index].plaintext)) << index;
}

TEST_F(ForwardedStreamTest, RefusesAStreamItCannotForward) {
  EXPECT_EQ(
      ForwardedStream::open(platform_, shared_stream(7), receive_keys(), shown_)
          .error(),
      Refusal::unsupported_version);
  EXPECT_EQ(ForwardedStream::open(platform_, shared_stream(4096),
                                  receive_keys(), shown_)
                .error(),
            Refusal::unsupported_version);
  auto empty = shared_stream();
  empty.description.profiles.clear();
  EXPECT_EQ(
      ForwardedStream::open(platform_, std::move(empty), receive_keys(), shown_)
          .error(),
      Refusal::invalid_stream);
  auto headerless = shared_stream();
  headerless.description.profiles.front().header.clear();
  EXPECT_EQ(ForwardedStream::open(platform_, std::move(headerless),
                                  receive_keys(), shown_)
                .error(),
            Refusal::invalid_stream);
  libchiaking::platform::DefaultPlatformFactory bare;
  EXPECT_EQ(ForwardedStream::open(bare, shared_stream(), receive_keys(), shown_)
                .error(),
            Refusal::crypto_unavailable);
  EXPECT_TRUE(shown_.descriptions().empty());
}

TEST_F(ForwardedStreamTest, RefusesADatagramThatIsNotTheHosts) {
  const auto& vector = vectors.front();
  const auto valid = bytes(vector.datagram);
  auto guest = open();
  ASSERT_TRUE(guest);

  EXPECT_EQ(guest.value()->receive(sealed(vector, {})).error(),
            Refusal::malformed_datagram);
  auto video = sealed(vector, valid);
  video.media = SharedDatagram::Media::video;
  EXPECT_EQ(guest.value()->receive(video).error(), Refusal::malformed_datagram);
  auto moved = sealed(vector, valid);
  moved.key_position += 1;
  EXPECT_EQ(guest.value()->receive(moved).error(), Refusal::malformed_datagram);
  auto tampered = valid;
  tampered.back() ^= std::byte{1};
  EXPECT_EQ(guest.value()->receive(sealed(vector, tampered)).error(),
            Refusal::authentication_failed);
  EXPECT_TRUE(shown_.sounds().empty());

  for (const auto& next : vectors)
    EXPECT_TRUE(guest.value()->receive(sealed(next, bytes(next.datagram))));
  const auto sounds = shown_.sounds();
  ASSERT_EQ(sounds.size(), vectors.size());
  EXPECT_EQ(sounds.front().bytes, bytes(vector.plaintext));
  EXPECT_EQ(sounds.back().bytes, bytes(vectors.back().plaintext));
}

TEST_F(ForwardedStreamTest, RefusesADatagramSealedWithOtherKeys) {
  const auto& vector = vectors.front();
  RecordedMedia other;
  auto stranger = ForwardedStream::open(platform_, shared_stream(),
                                        receive_keys(std::byte{1}), other);
  ASSERT_TRUE(stranger);
  EXPECT_EQ(
      stranger.value()->receive(sealed(vector, bytes(vector.datagram))).error(),
      Refusal::authentication_failed);
  EXPECT_TRUE(other.sounds().empty());
}

TEST_F(ForwardedStreamTest, KeepsWhatTheAppSaysAboutItsDecoder) {
  auto guest = open();
  ASSERT_TRUE(guest);
  EXPECT_FALSE(guest.value()->fault());
  guest.value()->video_rejected(4, 9);
  EXPECT_EQ(guest.value()->statistics().decoder_rejections, 1U);
  EXPECT_EQ(guest.value()->statistics().last_decoder_error, 9);
  guest.value()->video_output_failed(17);
  const auto fault = guest.value()->fault();
  ASSERT_TRUE(fault);
  EXPECT_EQ(fault->kind, media::MediaFailure::decoder_output);
  EXPECT_EQ(fault->cause, 17);
  const auto& vector = vectors.front();
  EXPECT_EQ(
      guest.value()->receive(sealed(vector, bytes(vector.datagram))).error(),
      Refusal::media_failed);
  EXPECT_EQ(guest.value()->service(std::chrono::milliseconds{8}).error(),
            Refusal::media_failed);
}

}  // namespace
}  // namespace play_test
