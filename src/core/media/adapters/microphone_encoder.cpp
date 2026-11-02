#include "libchiaking/detail/media/microphone_encoder.hpp"

#include "src/core/net/protocol/remote_play/stream/microphone_sender.hpp"

#include <utility>

#include <opus.h>

namespace libchiaking::media {

static_assert(MicrophoneEncoder::frame_size ==
              net::protocol::remote_play::MicrophoneSender::frame_size);

class MicrophoneEncoder::Impl {
 public:
  explicit Impl(OpusEncoder* encoder) noexcept : encoder_(encoder) {}
  Impl(const Impl&) = delete;
  auto operator=(const Impl&) -> Impl& = delete;
  ~Impl() { opus_encoder_destroy(encoder_); }

  auto configure() noexcept -> bool {
    return opus_encoder_ctl(
               encoder_, OPUS_SET_BITRATE(static_cast<opus_int32>(bitrate))) ==
               OPUS_OK &&
           opus_encoder_ctl(encoder_, OPUS_SET_VBR(0)) == OPUS_OK &&
           opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC)) ==
               OPUS_OK &&
           opus_encoder_ctl(encoder_, OPUS_SET_PHASE_INVERSION_DISABLED(1)) ==
               OPUS_OK;
  }

  auto encode(Pcm pcm) noexcept -> Expected<Frame, AudioError> {
    Frame frame{};
    const auto written =
        opus_encode(encoder_, pcm.data(), static_cast<int>(samples_per_frame),
                    reinterpret_cast<unsigned char*>(frame.data()),
                    static_cast<opus_int32>(frame.size()));
    if (written != static_cast<opus_int32>(frame.size()))
      return unexpected(AudioError::encode_failed);
    return frame;
  }

 private:
  OpusEncoder* encoder_;
};

auto MicrophoneEncoder::create() -> Expected<MicrophoneEncoder, AudioError> {
  int error = OPUS_OK;
  auto* encoder = opus_encoder_create(
      static_cast<opus_int32>(sample_rate), static_cast<int>(channels),
      OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
  if (!encoder) return unexpected(AudioError::codec_unavailable);
  auto impl = std::make_unique<Impl>(encoder);
  if (error != OPUS_OK || !impl->configure())
    return unexpected(AudioError::codec_unavailable);
  return MicrophoneEncoder{std::move(impl)};
}

MicrophoneEncoder::MicrophoneEncoder(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
MicrophoneEncoder::MicrophoneEncoder(MicrophoneEncoder&&) noexcept = default;
auto MicrophoneEncoder::operator=(MicrophoneEncoder&&) noexcept
    -> MicrophoneEncoder& = default;
MicrophoneEncoder::~MicrophoneEncoder() = default;

auto MicrophoneEncoder::encode(Pcm pcm) -> Expected<Frame, AudioError> {
  return impl_->encode(pcm);
}

}  // namespace libchiaking::media
