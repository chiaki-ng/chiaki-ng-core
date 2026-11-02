/// @file
/// @brief Declares StreamInfoCodec, which checks the console's Takion stream
/// info and turns it into the formats the caller decodes.
///
/// The stream info is the console's last handshake message before media. It
/// lists the picture sizes it may switch between (each with codec parameter
/// sets) and the audio format as a 14-byte header: channel count, bits per
/// sample, then big-endian 32-bit sample rate, frame size and a field of
/// unknown meaning. Newer Takion versions also describe each audio channel and
/// its alternative profiles.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/net/protocol/takion/detail/control_message.hpp"
#include "src/core/net/protocol/takion/profile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::net::protocol::remote_play {

/// @brief A parsed 14-byte audio header.
struct AudioHeader {
  /// @brief The channel count.
  std::uint8_t channels{};
  /// @brief Bits per sample.
  std::uint8_t bits{};
  /// @brief Samples per second.
  std::uint32_t rate{};
  /// @brief Samples per channel in one frame.
  std::uint32_t frame_size{};
  /// @brief The last 32-bit field, whose meaning is unknown.
  std::uint32_t unknown{};

  /// @brief Compares every field.
  ///
  /// @return True when equal.
  constexpr bool operator==(const AudioHeader&) const noexcept = default;
};

/// @brief A raw 14-byte audio header.
using AudioHeaderBytes = std::array<std::byte, event::audio_header_size>;

/// @brief One audio channel the stream info describes (AudioFeature::
/// channel_setup).
struct AudioChannelSetup {
  /// @brief The channel's type; 0 is the main channel.
  std::uint32_t type{};
  /// @brief The channel's default header.
  AudioHeaderBytes header{};
  /// @brief Whether the channel carries raw PCM.
  bool raw_pcm{};
  /// @brief The channel's FEC mode, when stated.
  std::optional<std::uint32_t> fec_mode{};
  /// @brief The channel's alternative headers by channel count.
  std::map<std::uint8_t, AudioHeaderBytes> profiles{};

  /// @brief Compares every field.
  ///
  /// @return True when equal.
  bool operator==(const AudioChannelSetup&) const = default;
};

/// @brief A stream info that passed validation.
struct ValidatedStreamInfo {
  /// @brief The parsed audio header.
  AudioHeader audio;
  /// @brief The picture sizes that have codec headers, at most eight, in the
  /// console's order.
  std::vector<takion::StreamInfoResolution> resolutions;
  /// @brief The raw audio header.
  AudioHeaderBytes audio_header{};
  /// @brief The audio channels, the first of each type only.
  std::vector<AudioChannelSetup> audio_channels{};
  /// @brief How often the console wants congestion reports, in milliseconds,
  /// when it says.
  std::optional<std::uint32_t> congestion_control_interval_ms{};
};

/// @brief Why a stream info was refused.
enum class StreamInfoError : std::uint8_t {
  /// @brief The audio header is not 14 bytes.
  invalid_audio_header,
};

/// @brief Validates stream infos and converts them for the caller.
///
/// Stateless; every member is static.
class StreamInfoCodec {
 public:
  /// @brief The caller's view of an audio header, with its Opus layout.
  ///
  /// @param header The header.
  /// @return The format; layout fields stay zero for unsupported channel
  ///     counts.
  static auto to_public_audio_format(const AudioHeader& header) noexcept
      -> event::AudioFormat;

  /// @brief The caller's view of a validated stream info.
  ///
  /// @param info The stream info.
  /// @return The event, without an effective launch.
  static auto to_public_stream_info(const ValidatedStreamInfo& info)
      -> event::StreamInfoReady {
    event::StreamInfoReady result;
    result.audio = to_public_audio_format(info.audio);
    result.profiles.reserve(info.resolutions.size());
    for (const auto& entry : info.resolutions)
      result.profiles.push_back(
          event::VideoProfile{entry.width, entry.height, entry.video_header});
    result.audio_header = info.audio_header;
    return result;
  }

  /// @brief Checks a stream info.
  ///
  /// Picture sizes without a codec header are dropped and at most eight are
  /// kept. Channel setups are read only with AudioFeature::channel_setup, and
  /// their profiles only with AudioFeature::channel_profiles.
  ///
  /// @param info The console's stream info; taken over.
  /// @param features The negotiated Takion features.
  /// @return The validated info.
  /// @retval StreamInfoError::invalid_audio_header The audio header is not 14
  ///     bytes.
  [[nodiscard]] static auto validate(takion::StreamInfo info,
                                     takion::FeatureSet features = {})
      -> Expected<ValidatedStreamInfo, StreamInfoError>;

  /// @brief Parses an audio header.
  ///
  /// @param bytes At least 14 bytes; any more are ignored.
  /// @return The header, or nothing when too short.
  [[nodiscard]] static auto parse_audio_header(
      std::span<const std::byte> bytes) noexcept -> std::optional<AudioHeader>;

 private:
  /// @brief The size of an audio header, in bytes.
  static constexpr std::size_t audio_header_size = event::audio_header_size;
  /// @brief The most picture sizes kept.
  static constexpr std::size_t max_video_profiles = 8;

  /// @brief Copies the first 14 bytes.
  ///
  /// @param bytes At least 14 bytes.
  /// @return The raw header.
  static auto header_bytes(std::span<const std::byte> bytes) noexcept
      -> AudioHeaderBytes;
  /// @brief Reads the audio channel setups.
  ///
  /// @param channels The stream info's channels; their data is moved.
  /// @param features The negotiated Takion features.
  /// @return The setups; empty without AudioFeature::channel_setup.
  static auto channel_setups(
      std::vector<takion::StreamInfoAudioChannel>& channels,
      takion::FeatureSet features) -> std::vector<AudioChannelSetup>;
};

}  // namespace libchiaking::net::protocol::remote_play
