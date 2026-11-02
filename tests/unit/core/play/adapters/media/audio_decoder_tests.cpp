#include <libchiaking/detail/media/audio_decoder.hpp>

#include "src/core/media/adapters/audio_output_layout.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

#include <opus.h>

namespace play_test {
namespace {

namespace media = libchiaking::media;
namespace remote_play = libchiaking::net::protocol::remote_play;
using Decoder = media::AudioDecoder;
using media::AudioError;

class AudioDecoder : public ::testing::Test {
 protected:
  using Packets = std::vector<std::vector<std::byte>>;
  using Encoder = std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)>;
  using OpusPlayer =
      std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)>;

  static auto format(std::uint8_t channels, std::uint32_t frame_size = 960)
      -> media::AudioFormat {
    media::AudioFormat value;
    value.channels = channels;
    value.bits = 16;
    value.rate = 48000;
    value.frame_size = frame_size;
    return value;
  }

  static auto wired(const media::AudioFormat& format)
      -> remote_play::event::AudioFormat {
    remote_play::event::AudioFormat value;
    value.channels = format.channels;
    value.bits = format.bits;
    value.rate = format.rate;
    value.frame_size = format.frame_size;
    value.streams = format.streams;
    value.coupled_streams = format.coupled_streams;
    value.mapping = format.mapping;
    std::ranges::transform(
        format.positions, value.positions.begin(),
        [](media::AudioChannelPosition position) {
          return static_cast<remote_play::AudioChannelPosition>(position);
        });
    return value;
  }

  static auto raw_packet(std::span<const std::int16_t> samples)
      -> std::vector<std::byte> {
    std::vector<std::byte> packet;
    for (const auto sample : samples) {
      const auto raw = static_cast<std::uint16_t>(sample);
      packet.push_back(static_cast<std::byte>(raw & 0xffU));
      packet.push_back(static_cast<std::byte>(raw >> 8U));
    }
    return packet;
  }

  static auto opus_player() -> OpusPlayer {
    int opened = OPUS_OK;
    OpusPlayer player{opus_decoder_create(48000, 2, &opened),
                      &opus_decoder_destroy};
    EXPECT_EQ(opened, OPUS_OK);
    return player;
  }

  void encode_tones(std::size_t count) {
    int error = OPUS_OK;
    const Encoder encoder{
        opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error),
        &opus_encoder_destroy};
    ASSERT_NE(encoder, nullptr);
    ASSERT_EQ(error, OPUS_OK);
    std::array<opus_int16, 1920> pcm{};
    std::array<unsigned char, 1500> packet{};
    for (std::size_t block = 0; block < count; ++block) {
      for (std::size_t frame = 0; frame < 960; ++frame) {
        const double phase = 2.0 * std::numbers::pi * 440.0 *
                             static_cast<double>(block * 960 + frame) / 48000.0;
        pcm[frame * 2] = static_cast<opus_int16>(12000.0 * std::sin(phase));
        pcm[frame * 2 + 1] = static_cast<opus_int16>(-6000.0 * std::sin(phase));
      }
      const auto length =
          opus_encode(encoder.get(), pcm.data(), 960, packet.data(),
                      static_cast<opus_int32>(packet.size()));
      ASSERT_GT(length, 0);
      std::vector<std::byte> bytes(static_cast<std::size_t>(length));
      std::memcpy(bytes.data(), packet.data(), bytes.size());
      packets_.push_back(std::move(bytes));
    }
  }

  void decode_tones(Decoder& decoder) {
    ASSERT_NO_FATAL_FAILURE(encode_tones(6));
    for (const auto& packet : packets_) {
      const auto decoded = decoder.decode(packet);
      ASSERT_TRUE(decoded);
      ASSERT_EQ(decoded.value().size(), 1920);
    }
  }

  static auto sink_decoder() {
    media::AudioUnit described;
    described.kind = media::AudioUnit::Kind::format;
    described.format = format(2);
    return Decoder::create(described.format, described.raw_pcm);
  }

  auto packets() const -> const Packets& { return packets_; }

 private:
  Packets packets_;
};

TEST_F(AudioDecoder, PublishesItsLimits) {
  EXPECT_EQ(Decoder::max_packet_size, 65536);
  EXPECT_EQ(Decoder::max_frame_size, 5760);
  EXPECT_EQ(Decoder::sample_bits, 16);
  EXPECT_EQ(Decoder::max_output_channels, 12);
}

TEST_F(AudioDecoder, AcceptsEverySupportedSampleRate) {
  for (const std::uint32_t rate : Decoder::sample_rates) {
    SCOPED_TRACE(rate);
    auto value = format(2);
    value.rate = rate;
    value.frame_size = rate / 50;
    EXPECT_TRUE(Decoder::create(value));
  }
}

TEST_F(AudioDecoder, RefusesFormatsOutsideItsLimits) {
  auto value = format(2);
  value.rate = 44100;
  const auto rate = Decoder::create(value);
  ASSERT_FALSE(rate);
  EXPECT_EQ(rate.error(), AudioError::invalid_format);
  value = format(2);
  value.bits = 24;
  EXPECT_FALSE(Decoder::create(value));
  EXPECT_FALSE(Decoder::create(format(0)));
  EXPECT_FALSE(Decoder::create(format(13)));
  EXPECT_FALSE(Decoder::create(format(2, 0)));
  EXPECT_FALSE(Decoder::create(format(2, 5761)));
  EXPECT_TRUE(Decoder::create(format(2, 5760)));
}

TEST_F(AudioDecoder, RefusesAChannelCountWithoutALayout) {
  const auto five = Decoder::create(format(5));
  ASSERT_FALSE(five);
  EXPECT_EQ(five.error(), AudioError::unsupported_layout);
}

TEST_F(AudioDecoder, AcceptsEveryChannelCountWithALayout) {
  for (const std::uint8_t channels :
       {std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{6}, std::uint8_t{8},
        std::uint8_t{12}}) {
    SCOPED_TRACE(+channels);
    EXPECT_TRUE(Decoder::create(format(channels)));
  }
}

TEST_F(AudioDecoder, DescribesTheStereoFormatTheMediaSinkDelivers) {
  const auto created = sink_decoder();
  ASSERT_TRUE(created);
  const auto& decoder = created.value();
  EXPECT_FALSE(decoder.multistream());
  EXPECT_FALSE(decoder.raw_pcm());
  EXPECT_EQ(decoder.output_channels(), 2);
  EXPECT_EQ(decoder.input_channels(), 2);
  EXPECT_EQ(decoder.sample_rate(), 48000);
  EXPECT_EQ(decoder.layout(), Decoder::Layout::direct);
}

TEST_F(AudioDecoder, DecodesTheSameSamplesAsOpusWithoutPhaseInversion) {
  auto created = sink_decoder();
  ASSERT_TRUE(created);
  auto& decoder = created.value();
  const auto reference = opus_player();
  ASSERT_NE(reference, nullptr);
  EXPECT_EQ(
      opus_decoder_ctl(reference.get(), OPUS_SET_PHASE_INVERSION_DISABLED(1)),
      OPUS_OK);
  const auto inverting = opus_player();
  ASSERT_NE(inverting, nullptr);
  ASSERT_NO_FATAL_FAILURE(encode_tones(6));
  std::array<opus_int16, 1920> expected{};
  std::array<opus_int16, 1920> inverted{};
  bool phase_inversion_changes_samples = false;
  double loudest = 0;
  for (const auto& packet : packets()) {
    media::AudioUnit samples;
    samples.bytes = packet;
    const auto decoded = decoder.decode(samples.bytes);
    const int frames = opus_decode(
        reference.get(), reinterpret_cast<const unsigned char*>(packet.data()),
        static_cast<opus_int32>(packet.size()), expected.data(), 960, 0);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(frames, 960);
    ASSERT_EQ(decoded.value().size(), 1920);
    EXPECT_TRUE(std::ranges::equal(decoded.value(), expected));
    EXPECT_EQ(opus_decode(inverting.get(),
                          reinterpret_cast<const unsigned char*>(packet.data()),
                          static_cast<opus_int32>(packet.size()),
                          inverted.data(), 960, 0),
              960);
    if (!std::ranges::equal(decoded.value(), inverted))
      phase_inversion_changes_samples = true;
    for (const auto sample : decoded.value())
      loudest = std::max(loudest, std::fabs(static_cast<double>(sample)));
  }
  EXPECT_GT(loudest, 8000.0);
  EXPECT_LT(loudest, 16000.0);
  EXPECT_TRUE(phase_inversion_changes_samples);
}

TEST_F(AudioDecoder, ConcealsMissingAndEmptyPacketsWithAFullFrame) {
  auto created = sink_decoder();
  ASSERT_TRUE(created);
  auto& decoder = created.value();
  ASSERT_NO_FATAL_FAILURE(decode_tones(decoder));
  const auto concealed = decoder.conceal();
  ASSERT_TRUE(concealed);
  EXPECT_EQ(concealed.value().size(), 1920);
  const auto empty = decoder.decode({});
  ASSERT_TRUE(empty);
  EXPECT_EQ(empty.value().size(), 1920);
  const auto statistics = decoder.statistics();
  EXPECT_EQ(statistics.decoded_packets, 8);
  EXPECT_EQ(statistics.concealed_frames, 2);
  EXPECT_EQ(statistics.decode_failures, 0);
  EXPECT_EQ(statistics.pcm_frames, 8 * 960);
}

TEST_F(AudioDecoder, CountsOversizedAndCorruptPacketsAsFailures) {
  auto created = sink_decoder();
  ASSERT_TRUE(created);
  auto& decoder = created.value();
  ASSERT_NO_FATAL_FAILURE(decode_tones(decoder));
  const std::vector<std::byte> oversized(Decoder::max_packet_size + 1);
  const auto rejected = decoder.decode(oversized);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), AudioError::invalid_packet);
  const std::vector<std::byte> garbage(7, std::byte{0xff});
  const auto corrupt = decoder.decode(garbage);
  ASSERT_FALSE(corrupt);
  EXPECT_EQ(corrupt.error(), AudioError::decode_failed);
  const auto statistics = decoder.statistics();
  EXPECT_EQ(statistics.decode_failures, 2);
  EXPECT_LT(statistics.last_error, 0);
}

TEST_F(AudioDecoder, KeepsConcealingAfterAMove) {
  auto created = sink_decoder();
  ASSERT_TRUE(created);
  auto decoder = std::move(created.value());
  ASSERT_NO_FATAL_FAILURE(decode_tones(decoder));
  Decoder moved{std::move(decoder)};
  EXPECT_TRUE(moved.conceal());
}

TEST_F(AudioDecoder, RoutesRawSurroundPcmThroughTheAssumedLayout) {
  auto created = Decoder::create(format(6, 480), true);
  ASSERT_TRUE(created);
  auto& decoder = created.value();
  EXPECT_TRUE(decoder.raw_pcm());
  EXPECT_FALSE(decoder.multistream());
  EXPECT_EQ(decoder.output_channels(), 6);
  EXPECT_EQ(decoder.layout(), Decoder::Layout::assumed);
  const std::array<std::int16_t, 12> samples{10, 20, 30, 40, 50, 60,
                                             -1, -2, -3, -4, -5, -6};
  const auto routed = decoder.decode(raw_packet(samples));
  ASSERT_TRUE(routed);
  ASSERT_EQ(routed.value().size(), 12);
  const std::array<std::int16_t, 12> expected{10, 20, 30, 40, 50, 60,
                                              -1, -2, -3, -4, -5, -6};
  EXPECT_TRUE(
      std::equal(expected.begin(), expected.end(), routed.value().begin()));
  EXPECT_EQ(decoder.statistics().pcm_frames, 2);
}

TEST_F(AudioDecoder, RefusesARaggedRawPacketAndConcealsNothing) {
  auto created = Decoder::create(format(6, 480), true);
  ASSERT_TRUE(created);
  auto& decoder = created.value();
  const std::array<std::int16_t, 12> samples{10, 20, 30, 40, 50, 60,
                                             -1, -2, -3, -4, -5, -6};
  auto packet = raw_packet(samples);
  packet.pop_back();
  const auto ragged = decoder.decode(packet);
  ASSERT_FALSE(ragged);
  EXPECT_EQ(ragged.error(), AudioError::invalid_packet);
  const auto nothing = decoder.conceal();
  ASSERT_TRUE(nothing);
  EXPECT_TRUE(nothing.value().empty());
  EXPECT_EQ(decoder.statistics().concealed_frames, 0);
}

TEST_F(AudioDecoder, ConcealsAFullFrameOfTwelveChannels) {
  auto twelve = Decoder::create(format(12));
  ASSERT_TRUE(twelve);
  EXPECT_TRUE(twelve.value().multistream());
  EXPECT_EQ(twelve.value().output_channels(), 12);
  EXPECT_EQ(twelve.value().input_channels(), 12);
  const auto concealed = twelve.value().conceal();
  ASSERT_TRUE(concealed);
  EXPECT_EQ(concealed.value().size(), 960 * 12);
}

TEST_F(AudioDecoder, PlansReportedPositionsLikeTheOutputLayout) {
  auto positioned = format(2);
  positioned.streams = 1;
  positioned.coupled_streams = 1;
  positioned.mapping[1] = 1;
  positioned.positions[0] = media::AudioChannelPosition::front_left;
  positioned.positions[1] = media::AudioChannelPosition::front_right;
  const auto stereo = Decoder::create(positioned);
  const auto positions = wired(positioned).positions;
  const auto reference =
      media::AudioOutputLayout::plan(2, std::span{positions}.first(2));
  ASSERT_TRUE(stereo);
  ASSERT_TRUE(reference);
  EXPECT_EQ(static_cast<int>(stereo.value().layout()),
            static_cast<int>(reference.value().source()));
  EXPECT_EQ(stereo.value().output_channels(),
            reference.value().output_channels());
  EXPECT_EQ(stereo.value().input_channels(),
            reference.value().input_channels());
}

}  // namespace
}  // namespace play_test
