/// @file
/// @brief Declares MicrophoneEncoder, which turns captured PCM into the frames
/// a console accepts.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/media/audio_error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace libchiaking::media {

/// @brief Encodes microphone PCM into the fixed-size Opus frames taken by
/// play::Microphone::push.
///
/// The encoder takes 48 kHz stereo audio in 10 ms blocks and produces one
/// constant-bitrate 40-byte frame per block, tuned for music with phase
/// inversion off. It keeps codec state
/// between calls, so one encoder serves one continuous capture and must not be
/// shared between threads without the caller's own locking.
///
/// @see play::Microphone
class MicrophoneEncoder {
 public:
  /// @brief Sample rate of the input, in hertz.
  static constexpr std::uint32_t sample_rate = 48000;
  /// @brief Number of interleaved channels in the input.
  static constexpr std::size_t channels = 2;
  /// @brief Samples per channel in one input block (10 ms).
  static constexpr std::size_t samples_per_frame = 480;
  /// @brief Size in bytes of every encoded frame.
  static constexpr std::size_t frame_size = 40;
  /// @brief Bit rate of the encoded stream, in bits per second.
  static constexpr std::uint32_t bitrate = static_cast<std::uint32_t>(
      frame_size * 8 * sample_rate / samples_per_frame);

  /// @brief One encoded frame.
  using Frame = std::array<std::byte, frame_size>;
  /// @brief One input block: samples_per_frame frames of interleaved signed
  /// 16-bit samples.
  using Pcm = std::span<const std::int16_t, samples_per_frame * channels>;

  /// @brief Creates an encoder configured for the console's microphone
  /// format.
  ///
  /// @return The encoder.
  /// @retval AudioError::codec_unavailable The Opus encoder could not be
  ///     created or configured.
  static auto create() -> Expected<MicrophoneEncoder, AudioError>;

  /// @brief Moves the encoder; the source is left without one and may only be
  /// assigned to or destroyed.
  MicrophoneEncoder(MicrophoneEncoder&&) noexcept;
  /// @brief Replaces this encoder with another, releasing the one held
  /// before.
  ///
  /// @return This object.
  auto operator=(MicrophoneEncoder&&) noexcept -> MicrophoneEncoder&;
  /// @brief Releases the codec.
  ~MicrophoneEncoder();

  /// @brief Encodes one block of PCM.
  ///
  /// @param pcm Exactly one 10 ms block; see Pcm.
  /// @return The encoded frame, ready for play::Microphone::push.
  /// @retval AudioError::encode_failed The codec did not produce a frame of
  ///     frame_size bytes.
  auto encode(Pcm pcm) -> Expected<Frame, AudioError>;

 private:
  class Impl;

  explicit MicrophoneEncoder(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace libchiaking::media
