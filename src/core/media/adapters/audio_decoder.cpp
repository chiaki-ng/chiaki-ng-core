#include "libchiaking/detail/media/audio_decoder.hpp"

#include "src/core/media/adapters/audio_output_layout.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/utils/bytes/endian.hpp"
#include "src/core/utils/memory/c_handle.hpp"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include <opus.h>
#include <opus_multistream.h>

namespace libchiaking::media {

static_assert(AudioDecoder::max_output_channels ==
              AudioOutputLayout::max_output_channels);
static_assert(AudioFormat::max_channels ==
              net::protocol::remote_play::audio_max_channels);
static_assert(
    static_cast<int>(AudioChannelPosition::mono) ==
    static_cast<int>(net::protocol::remote_play::AudioChannelPosition::mono));

class AudioDecoder::Core {
 public:
  [[nodiscard]] static auto open(const AudioFormat& format, bool raw_pcm)
      -> Expected<std::unique_ptr<Core>, media::AudioError> {
    const auto wired = wire(format);
    if (!valid(wired)) return unexpected(media::AudioError::invalid_format);
    const auto layout = AudioOutputLayout::plan(
        wired.channels, std::span{wired.positions}.first(wired.channels));
    if (!layout) return unexpected(media::AudioError::unsupported_layout);
    std::unique_ptr<Core> core{new Core{wired, layout.value(), raw_pcm}};
    if (const auto opened = core->open(); !opened)
      return unexpected(opened.error());
    return core;
  }

  auto decode(std::span<const std::byte> packet)
      -> Expected<Pcm, media::AudioError> {
    if (packet.size() > max_packet_size)
      return rejected(media::AudioError::invalid_packet, OPUS_BAD_ARG);
    if (raw_pcm_) return passthrough(packet);
    if (packet.empty()) ++statistics_.concealed_frames;
    const auto* data =
        packet.empty() ? nullptr
                       : reinterpret_cast<const unsigned char*>(packet.data());
    const auto length = static_cast<opus_int32>(packet.size());
    const auto frame_size = static_cast<int>(format_.frame_size);
    const int frames =
        multistream_ ? opus_multistream_decode(multistream_.get(), data, length,
                                               pcm_.data(), frame_size, 0)
                     : opus_decode(single_.get(), data, length, pcm_.data(),
                                   frame_size, 0);
    if (frames <= 0) return rejected(media::AudioError::decode_failed, frames);
    ++statistics_.decoded_packets;
    return routed(static_cast<std::size_t>(frames));
  }

  auto layout() const noexcept -> const AudioOutputLayout& { return layout_; }
  auto sample_rate() const noexcept -> std::uint32_t { return format_.rate; }
  auto multistream() const noexcept -> bool { return multistream_ != nullptr; }
  auto raw_pcm() const noexcept -> bool { return raw_pcm_; }
  auto statistics() const noexcept -> const Statistics& { return statistics_; }

 private:
  Core(const net::protocol::remote_play::event::AudioFormat& format,
       const AudioOutputLayout& layout, bool raw_pcm)
      : format_(format), layout_(layout), raw_pcm_(raw_pcm) {}

  static auto wire(const AudioFormat& format) noexcept
      -> net::protocol::remote_play::event::AudioFormat {
    net::protocol::remote_play::event::AudioFormat wired;
    wired.channels = format.channels;
    wired.bits = format.bits;
    wired.rate = format.rate;
    wired.frame_size = format.frame_size;
    wired.streams = format.streams;
    wired.coupled_streams = format.coupled_streams;
    wired.mapping = format.mapping;
    std::ranges::transform(
        format.positions, wired.positions.begin(),
        [](AudioChannelPosition position) {
          return static_cast<net::protocol::remote_play::AudioChannelPosition>(
              position);
        });
    return wired;
  }

  static auto valid(
      const net::protocol::remote_play::event::AudioFormat& format) noexcept
      -> bool {
    return format.channels >= 1 &&
           format.channels <= net::protocol::remote_play::audio_max_channels &&
           format.bits == sample_bits &&
           std::ranges::find(sample_rates, format.rate) != sample_rates.end() &&
           format.frame_size >= 1 && format.frame_size <= max_frame_size;
  }

  static auto needs_multistream(
      const net::protocol::remote_play::event::AudioFormat& format) noexcept
      -> bool {
    return format.channels > 2 ||
           (format.streams != 0 &&
            (format.streams != 1 ||
             format.coupled_streams != format.channels - 1));
  }

  static auto multistream_layout(
      const net::protocol::remote_play::event::AudioFormat& format) noexcept
      -> std::optional<net::protocol::remote_play::OpusMultistreamLayout> {
    if (format.streams == 0)
      return net::protocol::remote_play::opus_multistream_layout(
          format.channels);
    net::protocol::remote_play::OpusMultistreamLayout layout;
    layout.streams = format.streams;
    layout.coupled_streams = format.coupled_streams;
    layout.mapping = format.mapping;
    layout.positions = format.positions;
    return layout;
  }

  auto open() -> Expected<void, media::AudioError> {
    if (raw_pcm_) return {};
    int error = OPUS_OK;
    if (needs_multistream(format_)) {
      const auto streams = multistream_layout(format_);
      if (!streams) return unexpected(media::AudioError::unsupported_layout);
      multistream_.reset(opus_multistream_decoder_create(
          static_cast<opus_int32>(format_.rate), format_.channels,
          streams->streams, streams->coupled_streams, streams->mapping.data(),
          &error));
    } else {
      single_.reset(opus_decoder_create(static_cast<opus_int32>(format_.rate),
                                        format_.channels, &error));
    }
    if (error != OPUS_OK || (!single_ && !multistream_)) {
      statistics_.last_error = error != OPUS_OK ? error : OPUS_ALLOC_FAIL;
      return unexpected(error == OPUS_BAD_ARG
                            ? media::AudioError::unsupported_layout
                            : media::AudioError::codec_unavailable);
    }
    if (single_)
      static_cast<void>(opus_decoder_ctl(single_.get(),
                                         OPUS_SET_PHASE_INVERSION_DISABLED(1)));
    else
      static_cast<void>(opus_multistream_decoder_ctl(
          multistream_.get(), OPUS_SET_PHASE_INVERSION_DISABLED(1)));
    pcm_.resize(std::size_t{format_.frame_size} * format_.channels);
    return {};
  }

  auto passthrough(std::span<const std::byte> packet)
      -> Expected<Pcm, media::AudioError> {
    const auto frame_bytes =
        std::size_t{format_.channels} * sizeof(std::int16_t);
    if (packet.size() % frame_bytes != 0)
      return rejected(media::AudioError::invalid_packet, OPUS_BAD_ARG);
    pcm_.resize(packet.size() / sizeof(std::int16_t));
    for (std::size_t index = 0; index < pcm_.size(); ++index)
      pcm_[index] = static_cast<std::int16_t>(
          bytes::load_le<std::uint16_t>(packet.subspan(index * 2)));
    if (!packet.empty()) ++statistics_.decoded_packets;
    return routed(packet.size() / frame_bytes);
  }

  auto routed(std::size_t frames) -> Pcm {
    statistics_.pcm_frames += frames;
    const Pcm decoded{pcm_.data(), frames * layout_.input_channels()};
    if (!layout_.remaps()) return decoded;
    mix_.resize(frames * layout_.output_channels());
    layout_.apply(decoded, mix_);
    return mix_;
  }

  auto rejected(media::AudioError error, int code) noexcept
      -> Unexpected<media::AudioError> {
    ++statistics_.decode_failures;
    statistics_.last_error = code;
    return unexpected(error);
  }

  net::protocol::remote_play::event::AudioFormat format_;
  AudioOutputLayout layout_;
  bool raw_pcm_;
  memory::CHandle<OpusDecoder, opus_decoder_destroy> single_;
  memory::CHandle<OpusMSDecoder, opus_multistream_decoder_destroy> multistream_;
  std::vector<std::int16_t> pcm_;
  std::vector<std::int16_t> mix_;
  Statistics statistics_{};
};

auto AudioDecoder::create(const AudioFormat& format, bool raw_pcm)
    -> Expected<AudioDecoder, media::AudioError> {
  auto core = Core::open(format, raw_pcm);
  if (!core) return unexpected(core.error());
  return AudioDecoder{std::move(core.value())};
}

AudioDecoder::AudioDecoder(std::unique_ptr<Core> core) noexcept
    : core_(std::move(core)) {}

AudioDecoder::AudioDecoder(AudioDecoder&&) noexcept = default;
auto AudioDecoder::operator=(AudioDecoder&&) noexcept
    -> AudioDecoder& = default;
AudioDecoder::~AudioDecoder() = default;

auto AudioDecoder::decode(std::span<const std::byte> packet)
    -> Expected<Pcm, media::AudioError> {
  return core_->decode(packet);
}

auto AudioDecoder::conceal() -> Expected<Pcm, media::AudioError> {
  return core_->decode({});
}

auto AudioDecoder::layout() const noexcept -> Layout {
  switch (core_->layout().source()) {
    case AudioLayoutSource::direct:
      return Layout::direct;
    case AudioLayoutSource::reported:
      return Layout::reported;
    case AudioLayoutSource::assumed:
      break;
  }
  return Layout::assumed;
}

auto AudioDecoder::input_channels() const noexcept -> std::size_t {
  return core_->layout().input_channels();
}

auto AudioDecoder::output_channels() const noexcept -> std::size_t {
  return core_->layout().output_channels();
}

auto AudioDecoder::sample_rate() const noexcept -> std::uint32_t {
  return core_->sample_rate();
}

auto AudioDecoder::multistream() const noexcept -> bool {
  return core_->multistream();
}

auto AudioDecoder::raw_pcm() const noexcept -> bool { return core_->raw_pcm(); }

auto AudioDecoder::statistics() const noexcept -> Statistics {
  return core_->statistics();
}

}  // namespace libchiaking::media
