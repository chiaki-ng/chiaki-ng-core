#include "libchiaking/play_types.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>

#include <opus.h>

namespace play_test {
namespace {

using Encoder = libchiaking::media::MicrophoneEncoder;

class MicrophoneEncoder : public ::testing::Test {
 protected:
  using Pcm = std::array<std::int16_t, 960>;
  using OpusPlayer =
      std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)>;

  static auto frame_of(std::size_t index) -> Pcm {
    Pcm pcm{};
    if (index < 50) return pcm;
    const double frequency = index < 120 ? 440.0 : 3100.0;
    for (std::size_t sample = 0; sample < 480; ++sample) {
      const double time = static_cast<double>(index * 480 + sample) / 48000.0;
      const auto level = static_cast<std::int16_t>(
          std::sin(2 * std::numbers::pi * frequency * time) * 12000);
      pcm[sample * 2] = level;
      pcm[sample * 2 + 1] = level;
    }
    return pcm;
  }
};

TEST_F(MicrophoneEncoder, PublishesTheFrameShapeTheConsoleExpects) {
  EXPECT_EQ(Encoder::sample_rate, 48000);
  EXPECT_EQ(Encoder::channels, 2);
  EXPECT_EQ(Encoder::samples_per_frame, 480);
  EXPECT_EQ(Encoder::frame_size, 40);
  EXPECT_EQ(Encoder::bitrate, 32000);
}

TEST_F(MicrophoneEncoder, EncodesEveryFrameIntoFortyDecodableBytes) {
  auto created = Encoder::create();
  ASSERT_TRUE(created);
  auto encoder = std::move(created.value());
  int error = OPUS_OK;
  const OpusPlayer decoder{opus_decoder_create(48000, 2, &error),
                           &opus_decoder_destroy};
  ASSERT_NE(decoder, nullptr);
  ASSERT_EQ(error, OPUS_OK);
  std::array<opus_int16, 960> decoded{};
  for (std::size_t index = 0; index < 200; ++index) {
    SCOPED_TRACE(index);
    const auto frame = encoder.encode(frame_of(index));
    ASSERT_TRUE(frame);
    ASSERT_EQ(frame.value().size(), 40);
    const int samples = opus_decode(
        decoder.get(),
        reinterpret_cast<const unsigned char*>(frame.value().data()), 40,
        decoded.data(), 480, 0);
    EXPECT_EQ(samples, 480);
  }
}

TEST_F(MicrophoneEncoder, KeepsEncodingAfterAMove) {
  auto created = Encoder::create();
  ASSERT_TRUE(created);
  auto encoder = std::move(created.value());
  for (std::size_t index = 0; index < 200; ++index) {
    ASSERT_TRUE(encoder.encode(frame_of(index)));
  }
  Encoder moved{std::move(encoder)};
  EXPECT_TRUE(moved.encode(frame_of(199)));
}

}  // namespace
}  // namespace play_test
