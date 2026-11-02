#include "libchiaking/play_types.hpp"
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
#include <opus_multistream.h>

namespace play_test {
namespace {

namespace media = libchiaking::media;
using media::AudioError;
using Decoder = media::AudioDecoder;
using LayoutSource = media::AudioDecoder::Layout;

class OpusAudioDecoder : public ::testing::Test {
 protected:
  using Encoder = std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)>;

  static auto format(std::uint8_t channels, std::uint32_t frame_size = 960)
      -> media::AudioFormat {
    media::AudioFormat value;
    value.channels = channels;
    value.bits = 16;
    value.rate = 48000;
    value.frame_size = frame_size;
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

  void encode_silence() {
    int error = OPUS_OK;
    const Encoder encoder{
        opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error),
        &opus_encoder_destroy};
    ASSERT_NE(encoder, nullptr);
    ASSERT_EQ(error, OPUS_OK);
    const std::array<opus_int16, 1920> silence{};
    std::array<unsigned char, 1500> packet{};
    const auto length =
        opus_encode(encoder.get(), silence.data(), 960, packet.data(),
                    static_cast<opus_int32>(packet.size()));
    ASSERT_GT(length, 0);
    silence_.resize(static_cast<std::size_t>(length));
    std::memcpy(silence_.data(), packet.data(), silence_.size());
  }

  auto silence() const -> std::span<const std::byte> { return silence_; }

 private:
  std::vector<std::byte> silence_;
};

TEST_F(OpusAudioDecoder, PublishesItsLimits) {
  EXPECT_EQ(Decoder::max_packet_size, 65536);
  EXPECT_EQ(Decoder::max_frame_size, 5760);
}

TEST_F(OpusAudioDecoder, AcceptsEveryOpusSampleRate) {
  for (const std::uint32_t rate : {8000U, 12000U, 16000U, 24000U, 48000U}) {
    SCOPED_TRACE(rate);
    auto value = format(2);
    value.rate = rate;
    value.frame_size = rate / 50;
    EXPECT_TRUE(Decoder::create(value));
  }
}

TEST_F(OpusAudioDecoder, RefusesFormatsOutsideItsLimits) {
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

TEST_F(OpusAudioDecoder, RefusesAChannelCountWithoutALayout) {
  const auto five = Decoder::create(format(5));
  ASSERT_FALSE(five);
  EXPECT_EQ(five.error(), AudioError::unsupported_layout);
}

TEST_F(OpusAudioDecoder, AcceptsEveryChannelCountWithALayout) {
  for (const std::uint8_t channels :
       {std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{6}, std::uint8_t{8},
        std::uint8_t{12}}) {
    SCOPED_TRACE(+channels);
    EXPECT_TRUE(Decoder::create(format(channels)));
  }
}

TEST_F(OpusAudioDecoder, DescribesAStereoFormat) {
  const auto created = Decoder::create(format(2));
  ASSERT_TRUE(created);
  const auto& decoder = created.value();
  EXPECT_FALSE(decoder.multistream());
  EXPECT_FALSE(decoder.raw_pcm());
  EXPECT_EQ(decoder.output_channels(), 2);
  EXPECT_EQ(decoder.sample_rate(), 48000);
  EXPECT_EQ(decoder.layout(), LayoutSource::direct);
}

TEST_F(OpusAudioDecoder, DecodesStereoPacketsAndConcealsEmptyOnes) {
  auto created = Decoder::create(format(2));
  ASSERT_TRUE(created);
  auto& decoder = created.value();
  ASSERT_NO_FATAL_FAILURE(encode_silence());
  const auto decoded = decoder.decode(silence());
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded.value().size(), 1920);
  const auto concealed = decoder.conceal();
  ASSERT_TRUE(concealed);
  EXPECT_EQ(concealed.value().size(), 1920);
  const auto empty = decoder.decode({});
  ASSERT_TRUE(empty);
  EXPECT_EQ(empty.value().size(), 1920);
  const auto statistics = decoder.statistics();
  EXPECT_EQ(statistics.decoded_packets, 3);
  EXPECT_EQ(statistics.concealed_frames, 2);
  EXPECT_EQ(statistics.decode_failures, 0);
  EXPECT_EQ(statistics.pcm_frames, 2880);
}

TEST_F(OpusAudioDecoder, CountsOversizedAndCorruptPacketsAsFailures) {
  auto created = Decoder::create(format(2));
  ASSERT_TRUE(created);
  auto& decoder = created.value();
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

TEST_F(OpusAudioDecoder, KeepsConcealingAfterAMove) {
  auto created = Decoder::create(format(2));
  ASSERT_TRUE(created);
  auto decoder = std::move(created.value());
  Decoder moved{std::move(decoder)};
  EXPECT_TRUE(moved.conceal());
}

TEST_F(OpusAudioDecoder, ConcealsAFullFrameOfSixChannels) {
  auto created = Decoder::create(format(6));
  ASSERT_TRUE(created);
  EXPECT_TRUE(created.value().multistream());
  const auto concealed = created.value().conceal();
  ASSERT_TRUE(concealed);
  EXPECT_EQ(concealed.value().size(), 960 * 6);
}

TEST_F(OpusAudioDecoder, PassesRawPcmThroughTheLayout) {
  auto created = Decoder::create(format(6, 480), true);
  ASSERT_TRUE(created);
  auto& decoder = created.value();
  EXPECT_TRUE(decoder.raw_pcm());
  EXPECT_FALSE(decoder.multistream());
  EXPECT_EQ(decoder.output_channels(), 6);
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

TEST_F(OpusAudioDecoder, RefusesARaggedRawPacketAndConcealsNothing) {
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

TEST_F(OpusAudioDecoder, ReadsRawStereoAsLittleEndianSamples) {
  auto stereo = Decoder::create(format(2), true);
  ASSERT_TRUE(stereo);
  const std::array<std::byte, 4> frame{std::byte{0x34}, std::byte{0x12},
                                       std::byte{0xfe}, std::byte{0xff}};
  const auto direct = stereo.value().decode(frame);
  ASSERT_TRUE(direct);
  ASSERT_EQ(direct.value().size(), 2);
  EXPECT_EQ(direct.value()[0], 0x1234);
  EXPECT_EQ(direct.value()[1], -2);
}

struct SurroundRoute {
  std::uint8_t channels;
  std::uint8_t streams;
  std::uint8_t coupled;
  std::size_t tone_channel;
  std::size_t expected_output;
  double expected_gain;
};

class OpusSurroundRouting : public ::testing::TestWithParam<SurroundRoute> {
 protected:
  using Encoder = std::unique_ptr<OpusMSEncoder,
                                  decltype(&opus_multistream_encoder_destroy)>;

  static auto channel_rms(std::span<const std::int16_t> pcm,
                          std::size_t channels, std::size_t channel) -> double {
    double sum = 0;
    const auto frames = pcm.size() / channels;
    for (std::size_t frame = 0; frame < frames; ++frame) {
      const double value = pcm[frame * channels + channel];
      sum += value * value;
    }
    return frames ? std::sqrt(sum / static_cast<double>(frames)) : 0.0;
  }

  static auto surround_format(const SurroundRoute& route)
      -> media::AudioFormat {
    media::AudioFormat value;
    value.channels = route.channels;
    value.bits = 16;
    value.rate = 48000;
    value.frame_size = 960;
    value.streams = route.streams;
    value.coupled_streams = route.coupled;
    for (std::uint8_t index = 0; index < route.channels; ++index)
      value.mapping[index] = index;
    return value;
  }

  void decode_tone(Decoder& decoder, const media::AudioFormat& format) {
    const auto& route = GetParam();
    const std::size_t output = route.channels;
    int error = OPUS_OK;
    const Encoder encoder{
        opus_multistream_encoder_create(48000, route.channels, route.streams,
                                        route.coupled, format.mapping.data(),
                                        OPUS_APPLICATION_AUDIO, &error),
        &opus_multistream_encoder_destroy};
    ASSERT_NE(encoder, nullptr);
    ASSERT_EQ(error, OPUS_OK);
    opus_multistream_encoder_ctl(encoder.get(),
                                 OPUS_SET_BITRATE(96000 * route.streams));
    std::vector<opus_int16> pcm(std::size_t{960} * route.channels);
    std::array<unsigned char, 4000> packet{};
    for (int block = 0; block < 6; ++block) {
      for (std::size_t frame = 0; frame < 960; ++frame) {
        const double phase =
            2.0 * std::numbers::pi * 440.0 *
            static_cast<double>(static_cast<std::size_t>(block) * 960 + frame) /
            48000.0;
        pcm[frame * route.channels + route.tone_channel] =
            static_cast<opus_int16>(12000.0 * std::sin(phase));
      }
      const auto length =
          opus_multistream_encode(encoder.get(), pcm.data(), 960, packet.data(),
                                  static_cast<opus_int32>(packet.size()));
      ASSERT_GT(length, 0);
      const auto decoded = decoder.decode(std::as_bytes(
          std::span{packet}.first(static_cast<std::size_t>(length))));
      ASSERT_TRUE(decoded);
      ASSERT_EQ(decoded.value().size(), 960 * output);
      last_.assign(decoded.value().begin(), decoded.value().end());
    }
  }

  auto last() const -> std::span<const std::int16_t> { return last_; }

 private:
  std::vector<std::int16_t> last_;
};

TEST_P(OpusSurroundRouting, RoutesEachStreamToItsOutputChannel) {
  const auto& route = GetParam();
  const auto format = surround_format(route);
  auto created = Decoder::create(format);
  ASSERT_TRUE(created);
  auto& decoder = created.value();
  const std::size_t output = route.channels;
  EXPECT_TRUE(decoder.multistream());
  EXPECT_EQ(decoder.output_channels(), output);
  EXPECT_EQ(decoder.layout(), LayoutSource::assumed);
  ASSERT_NO_FATAL_FAILURE(decode_tone(decoder, format));
  const auto statistics = decoder.statistics();
  EXPECT_EQ(statistics.decode_failures, 0);
  EXPECT_EQ(statistics.pcm_frames, 6 * 960);
  const double target = channel_rms(last(), output, route.expected_output);
  const double reference = 12000.0 / std::numbers::sqrt2 * route.expected_gain;
  EXPECT_GE(target, reference * 0.7);
  EXPECT_LE(target, reference * 1.3);
  for (std::size_t channel = 0; channel < output; ++channel) {
    SCOPED_TRACE(channel);
    if (channel != route.expected_output) {
      EXPECT_LE(channel_rms(last(), output, channel), target * 0.1);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Layouts, OpusSurroundRouting,
                         ::testing::Values(SurroundRoute{6, 4, 2, 1, 1, 1.0},
                                           SurroundRoute{6, 4, 2, 5, 5, 1.0},
                                           SurroundRoute{8, 5, 3, 4, 6, 1.0},
                                           SurroundRoute{12, 7, 5, 8, 8, 1.0},
                                           SurroundRoute{12, 7, 5, 11, 11, 1.0},
                                           SurroundRoute{12, 7, 5, 4, 4, 1.0}));

}  // namespace
}  // namespace play_test
