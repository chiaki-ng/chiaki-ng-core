/// @file
/// @brief The audio side of a stream's negotiation: the Takion feature bits
/// that decide how audio is described, AudioProfileSelector, which follows the
/// audio profile each packet names, and AudioStateCodec, which builds the
/// audio state messages that change the console's audio output.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/net/protocol/remote_play/stream/stream_info.hpp"
#include "src/core/net/protocol/takion/detail/control_message.hpp"
#include "src/core/net/protocol/takion/profile.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::net::protocol::remote_play {

/// @brief An audio-related Takion feature, named by its bit in the negotiated
/// feature set.
enum class AudioFeature : std::uint8_t {
  /// @brief Bit 6: the stream info describes each audio channel separately.
  channel_setup = 6,
  /// @brief Bit 10: audio channels carry several profiles the client may pick
  /// from, and audio state messages may be sent.
  channel_profiles = 10,
  /// @brief Bit 15: the channel-count audio state, and profile selection in
  /// local modes 1 and 3.
  mode_channel_profiles = 15,
  /// @brief Bit 16: audio state may state more than eight available bits.
  wide_available_bits = 16,
};

/// @brief A new audio format the decoder must switch to before the next
/// packet.
struct AudioProfileChange {
  /// @brief The 14-byte audio header of the new profile.
  AudioHeaderBytes header{};
  /// @brief Whether the new profile carries raw PCM rather than Opus.
  bool raw_pcm{};

  /// @brief Compares every field.
  ///
  /// @return True when equal.
  bool operator==(const AudioProfileChange&) const = default;
};

/// @brief Tracks which audio channel and profile the console is sending.
///
/// Each audio packet names its channel (the AV lane) and the channel count it
/// carries. When profile selection is available the selector switches to the
/// matching profile of that channel; whenever the header in force differs
/// from the one last delivered, it reports a change. Not thread-safe.
class AudioProfileSelector {
 public:
  /// @brief Whether a feature set has an audio feature.
  ///
  /// @param features The negotiated Takion features.
  /// @param feature The feature.
  /// @return True when its bit is set.
  static constexpr bool has_feature(takion::FeatureSet features,
                                    AudioFeature feature) noexcept {
    return ((features.bits() >> static_cast<unsigned>(feature)) & 1U) != 0;
  }

  /// @brief Whether the client may choose among a channel's audio profiles.
  ///
  /// Local modes 1 and 3 need AudioFeature::mode_channel_profiles; other modes
  /// need AudioFeature::channel_profiles.
  ///
  /// @param features The negotiated Takion features.
  /// @param local_mode The stream's local audio mode.
  /// @return True when profile selection is available.
  static constexpr bool selection_enabled(takion::FeatureSet features,
                                          std::uint32_t local_mode) noexcept {
    const bool mode_one_or_three = (local_mode & ~2U) == 1U;
    return has_feature(features, mode_one_or_three
                                     ? AudioFeature::mode_channel_profiles
                                     : AudioFeature::channel_profiles);
  }

  /// @brief Starts from a validated stream info.
  ///
  /// @param info The stream info; its top-level header counts as delivered.
  /// @param features The negotiated Takion features.
  /// @param local_mode The stream's local audio mode.
  void configure(const ValidatedStreamInfo& info, takion::FeatureSet features,
                 std::uint32_t local_mode = 0);

  /// @brief Follows one audio packet.
  ///
  /// @param channel The packet's channel type, if it states one; otherwise the
  ///     last channel stays in force.
  /// @param requested_channels The channel count the packet carries.
  /// @return The new format when it changed, otherwise nothing.
  [[nodiscard]] auto select(std::optional<std::uint8_t> channel,
                            std::uint8_t requested_channels)
      -> std::optional<AudioProfileChange>;

  /// @brief The header in force for one channel, leaving the active channel
  /// as it is.
  ///
  /// @param channel The channel type.
  /// @return The channel's header and whether it carries raw PCM; nothing
  ///     when the stream info did not describe the channel.
  [[nodiscard]] auto header_of(std::uint8_t channel) const
      -> std::optional<AudioProfileChange>;

  /// @brief Forgets the stream info.
  void reset() noexcept;

 private:
  /// @brief One audio channel and the header in force for it.
  struct ChannelState {
    /// @brief What the stream info said about the channel.
    AudioChannelSetup setup;
    /// @brief The header in force.
    AudioHeaderBytes current{};
  };

  /// @brief The channels from the stream info.
  std::vector<ChannelState> channels_;
  /// @brief The index of the channel last named.
  std::optional<std::size_t> active_;
  /// @brief The header last delivered.
  AudioHeaderBytes delivered_header_{};
  /// @brief Whether the last delivered format was raw PCM.
  bool delivered_raw_pcm_{};
  /// @brief Whether profiles may be switched.
  bool selection_enabled_{};
};

/// @brief Why an audio state message could not be built.
enum class AudioStateError : std::uint8_t {
  /// @brief The negotiated Takion version lacks the needed audio feature.
  unsupported_by_version,
  /// @brief The kind is beyond AudioStateKind::channel_count.
  unsupported_kind,
};

/// @brief Builds audio state messages for the negotiated Takion features.
///
/// Stateless; every member is static.
class AudioStateCodec {
 public:
  /// @brief Where the little-endian 32-bit available-bits field starts in a
  /// flags or angle body.
  static constexpr std::size_t available_bits_offset = 1;
  /// @brief The largest available-bits value a version without
  /// AudioFeature::wide_available_bits accepts.
  static constexpr std::uint32_t narrow_available_bits = 0xff;

  /// @brief Builds the message.
  ///
  /// Needs AudioFeature::channel_profiles, and for the channel-count kind also
  /// AudioFeature::mode_channel_profiles. For kinds up to angle without wide
  /// available bits, an available-bits value above 0xff is clamped to 0xff.
  ///
  /// @param kind What the body describes.
  /// @param data The body.
  /// @param features The negotiated Takion features.
  /// @return The message.
  /// @retval AudioStateError::unsupported_by_version A needed feature is
  ///     missing.
  /// @retval AudioStateError::unsupported_kind The kind is out of range.
  [[nodiscard]] static auto build(AudioStateKind kind,
                                  std::span<const std::byte> data,
                                  takion::FeatureSet features)
      -> Expected<takion::AudioState, AudioStateError>;
};

}  // namespace libchiaking::net::protocol::remote_play
