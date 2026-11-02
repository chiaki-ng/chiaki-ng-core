/// @file
/// @brief Declares AudioDecoder, which turns a session's audio packets into
/// PCM.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/media/audio_error.hpp"
#include "libchiaking/detail/media/media_sink.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace libchiaking::media {

/// @brief Decodes the audio packets a MediaSink receives into interleaved
/// 16-bit PCM.
///
/// A frontend that does not want to drive Opus itself creates one decoder per
/// AudioFormat, feeds it the bytes of each AudioUnit of kind
/// AudioUnit::Kind::samples, and builds a new decoder when a unit of kind
/// AudioUnit::Kind::format announces a change.
///
/// Mono and stereo pass through unchanged. Six-channel streams are laid out as
/// 5.1 in the order front left, front right, centre, LFE, back left, back
/// right. Eight-channel streams are laid out as 7.1, adding side left and side
/// right after the back pair. Twelve-channel streams are laid out as 7.1.4 in
/// the order front left, front right, centre, LFE, side left, side right, back
/// left, back right, then the top front pair and the top back pair. Height
/// channels in a stream with fewer outputs are mixed into the matching
/// ear-level channels at reduced gain.
///
/// A decoder is not safe for concurrent use; call it from one thread at a time.
///
/// @see MediaSink::on_audio
class AudioDecoder {
 public:
  /// @brief Largest packet, in bytes, that decode accepts.
  static constexpr std::size_t max_packet_size = 65536;
  /// @brief Largest AudioFormat::frame_size accepted, in samples per channel.
  static constexpr std::uint32_t max_frame_size = 5760;
  /// @brief The only AudioFormat::bits value accepted.
  static constexpr std::uint8_t sample_bits = 16;
  /// @brief The AudioFormat::rate values accepted, in hertz.
  static constexpr std::array<std::uint32_t, 5> sample_rates{8000, 12000, 16000,
                                                             24000, 48000};
  /// @brief Largest number of channels the decoder outputs.
  static constexpr std::size_t max_output_channels = 12;

  /// @brief How the output channel order was decided.
  enum class Layout : std::uint8_t {
    /// @brief Mono or stereo, passed through as received.
    direct,
    /// @brief The stream named each channel's position and those were used.
    reported,
    /// @brief The positions were missing or inconsistent and the usual
    /// order for that channel count was assumed.
    assumed,
  };

  /// @brief Running totals for one decoder.
  struct Statistics {
    /// @brief Packets decoded successfully; with Opus this includes
    /// concealed ones.
    std::uint64_t decoded_packets{};
    /// @brief Calls that asked the codec to conceal a missing packet.
    std::uint64_t concealed_frames{};
    /// @brief Calls that returned an error.
    std::uint64_t decode_failures{};
    /// @brief Frames of PCM produced; one frame holds one sample per
    /// channel.
    std::uint64_t pcm_frames{};
    /// @brief Codec error code of the most recent failure; zero when none.
    int last_error{};

    /// @brief Compares every field.
    friend constexpr auto operator==(const Statistics&,
                                     const Statistics&) noexcept
        -> bool = default;
  };

  /// @brief Interleaved signed 16-bit samples, output_channels() per frame.
  using Pcm = std::span<const std::int16_t>;

  /// @brief Creates a decoder for one audio format.
  ///
  /// @param format The format from StreamDescription::audio or from an
  ///     AudioUnit.
  /// @param raw_pcm True when packets already hold little-endian 16-bit PCM
  ///     and need no codec; pass AudioUnit::raw_pcm.
  /// @return The decoder.
  /// @retval media::AudioError::invalid_format The channel count is not 1 to
  ///     AudioFormat::max_channels, the sample width is not sample_bits, the
  ///     rate is not one of sample_rates, or the frame size is not 1 to
  ///     max_frame_size.
  /// @retval media::AudioError::unsupported_layout More than two channels
  ///     were given without usable positions and the count is not 6, 8 or 12,
  ///     or the codec rejected the layout.
  /// @retval media::AudioError::codec_unavailable The Opus decoder could not
  ///     be created.
  [[nodiscard]] static auto create(const AudioFormat& format,
                                   bool raw_pcm = false)
      -> Expected<AudioDecoder, media::AudioError>;

  /// @brief Moves the decoder; the source is left without one and may only be
  /// assigned to or destroyed.
  AudioDecoder(AudioDecoder&&) noexcept;
  /// @brief Replaces this decoder with another, releasing the one held
  /// before.
  ///
  /// @return This object.
  auto operator=(AudioDecoder&&) noexcept -> AudioDecoder&;
  /// @brief Releases the codec.
  ~AudioDecoder();

  /// @brief Decodes one packet.
  ///
  /// An empty packet asks the codec to conceal a lost packet, the same as
  /// conceal.
  ///
  /// @param packet The bytes of one AudioUnit of kind
  ///     AudioUnit::Kind::samples.
  /// @return The decoded PCM. The span points into the decoder and is valid
  ///     until the next call to decode or conceal.
  /// @retval media::AudioError::invalid_packet The packet is larger than
  ///     max_packet_size, or a raw PCM packet does not hold whole frames.
  /// @retval media::AudioError::decode_failed The codec could not decode the
  ///     packet.
  [[nodiscard]] auto decode(std::span<const std::byte> packet)
      -> Expected<Pcm, media::AudioError>;
  /// @brief Produces PCM to cover one lost packet.
  ///
  /// @return One packet's worth of concealment PCM, valid until the next call
  ///     to decode or conceal. A raw PCM decoder returns an empty span.
  /// @retval media::AudioError::decode_failed The codec could not produce
  ///     concealment.
  [[nodiscard]] auto conceal() -> Expected<Pcm, media::AudioError>;

  /// @brief How the output channel order was decided; see Layout.
  ///
  /// @return How the output channel order was decided; see Layout.
  auto layout() const noexcept -> Layout;
  /// @brief Number of channels in the stream's packets.
  ///
  /// @return Number of channels in the stream's packets.
  auto input_channels() const noexcept -> std::size_t;
  /// @brief Number of channels per frame in the PCM this decoder returns.
  ///
  /// @return Number of channels per frame in the PCM this decoder returns.
  auto output_channels() const noexcept -> std::size_t;
  /// @brief Sample rate of the output, in hertz; the format's rate.
  ///
  /// @return Sample rate of the output, in hertz; the format's rate.
  auto sample_rate() const noexcept -> std::uint32_t;
  /// @brief Reports whether the stream is decoded as several coupled Opus
  /// streams, as surround formats are.
  ///
  /// @return `true` when the stream is decoded as several coupled Opus
  ///     streams, as surround formats are.
  auto multistream() const noexcept -> bool;
  /// @brief Reports whether the decoder passes PCM through without a codec.
  ///
  /// @return `true` when the decoder passes PCM through without a codec.
  auto raw_pcm() const noexcept -> bool;
  /// @brief Returns a copy of the running totals.
  ///
  /// @return A copy of the running totals.
  auto statistics() const noexcept -> Statistics;

 private:
  class Core;

  explicit AudioDecoder(std::unique_ptr<Core> core) noexcept;

  std::unique_ptr<Core> core_;
};

}  // namespace libchiaking::media
