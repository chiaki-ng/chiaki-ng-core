/// @file
/// @brief Declares the registry of Takion versions the library implements and
/// the wire profile each resolves to: feature set, AV layout, header and
/// prefix policies, ECDH curve, and which kinds of session may use it.
///
/// Implemented versions: 7 (bootstrap framing only), 9 to 21, 23 and 25;
/// 21, 23 and 25 are for CloudStream only. A version's features are the bits of
/// the PS5 Remote Play daemon's feature table for it, named where the library
/// knows what they gate.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "src/core/net/protocol/takion/detail/packet_types.hpp"
#include "src/core/net/protocol/takion/versions.hpp"

#include <libchiaking/platform/crypto/provider.hpp>

namespace libchiaking::net::protocol::takion {

/// @brief The features a Takion version may have; each is a bit of the
/// version's feature mask.
enum class Feature : std::uint8_t {
  /// @brief Bit 0, in every version from 9; what it gates is not known.
  unresolved_feature_0 = 0,
  /// @brief Bit 1, every version from 9: per-packet GMAC and payload
  /// encryption.
  packet_authentication = 1,
  /// @brief Bit 2, every version from 9: GKTRACE (packet type 0x0a) messages.
  trace_messages = 2,
  /// @brief Bit 3, every version from 9: audio processing.
  audio_processing = 3,
  /// @brief Bit 4, versions 9 and 11 on: encrypted direct messages; with
  /// generic_control_data it enables generic-data transfers.
  direct_message_encryption = 4,
  /// @brief Bit 5, version 10 and 13 on: ECDH over secp521r1 rather than
  /// secp256k1.
  p521_ecdh = 5,
  /// @brief Bit 6, version 11 on: a separate audio stream.
  separate_audio_stream = 6,
  /// @brief Bit 7, version 12 on; what it gates is not known.
  unresolved_feature_7 = 7,
  /// @brief Bit 8, version 15 on: generic control data.
  generic_control_data = 8,
  /// @brief Bit 9, version 15 on: automatic dynamic range.
  automatic_dynamic_range = 9,
  /// @brief Bit 10, version 15 on: audio profiles.
  audio_profiles = 10,
  /// @brief Bit 11, version 15 on: audio unit shapes carry only the source
  /// count (13 bits), the redundant count and unit size being derived.
  av_unit_count_only_encoding = 11,
  /// @brief Bit 12, version 15 on: render metrics.
  render_metrics = 12,
  /// @brief Bit 13, version 18 on: extended messages (packet type 0x0e).
  extended_messages = 13,
  /// @brief Bit 14, version 18 on; what it gates is not known.
  unresolved_feature_14 = 14,
  /// @brief Bit 15, version 18 on: an audio-channel-count audio state.
  audio_channel_count_state = 15,
  /// @brief Bit 16, version 19 on: wide audio availability bits.
  wide_audio_available_bits = 16,
  /// @brief Bit 17, version 20 on: the 8-byte extended packet header and the
  /// link metrics measured from it.
  extended_packet_headers = 17,
  /// @brief Bit 18, version 20 on; what it gates is not known.
  unresolved_feature_18 = 18,
  /// @brief Bit 19, version 21 on: expanded IDR requests.
  expanded_idr_requests = 19,
  /// @brief Bit 20, version 22 on: video frame metrics reports.
  video_frame_metrics = 20,
  /// @brief Bit 21, version 22 on: received traffic-class (DSCP and ECN)
  /// metrics reports.
  received_tos_metrics = 21,
};

/// @brief A set of Feature bits. A plain constexpr value.
class FeatureSet {
 public:
  /// @brief Makes an empty set.
  constexpr FeatureSet() noexcept = default;

  /// @brief Reads the set as a mask.
  ///
  /// @return Bit n set for feature n.
  constexpr std::uint32_t bits() const noexcept { return bits_; }

  /// @brief Tells whether a feature is in the set.
  ///
  /// @param feature The feature.
  /// @return Whether its bit is set.
  constexpr bool contains(Feature feature) const noexcept {
    return (bits_ & bit_for(feature)) != 0;
  }

  /// @brief Adds a feature.
  ///
  /// @param feature The feature.
  /// @return A new set with it.
  constexpr FeatureSet with(Feature feature) const noexcept {
    return FeatureSet{bits_ | bit_for(feature)};
  }

  /// @brief Makes a set from a mask.
  ///
  /// @param bits Bit n for feature n.
  /// @return The set.
  static constexpr FeatureSet from_bits(std::uint32_t bits) noexcept {
    return FeatureSet{bits};
  }

 private:
  /// @brief Wraps a mask.
  ///
  /// @param bits The mask.
  explicit constexpr FeatureSet(std::uint32_t bits) noexcept : bits_(bits) {}

  /// @brief Maps a feature to its bit.
  ///
  /// @param feature The feature.
  /// @return `1 << feature`.
  static constexpr std::uint32_t bit_for(Feature feature) noexcept {
    return std::uint32_t{1} << static_cast<std::uint8_t>(feature);
  }

  /// @brief The mask.
  std::uint32_t bits_{};
};

/// @brief Which AV header layout a version uses; see PacketCodec.
enum class AvLayout : std::uint8_t {
  /// @brief Version 7: the 17-byte audio and 20-byte video headers, with the
  /// 3-byte NALU descriptor for either medium on bit 0x10.
  version_7,
  /// @brief Versions 9 and 10: audio gains one byte after the header; audio
  /// unit words hold an 8-bit unit index and count.
  versions_9_to_10,
  /// @brief Version 11: audio also carries the lane tag byte.
  version_11,
  /// @brief Versions 12 to 14: as version 11.
  versions_12_to_14,
  /// @brief Versions 15 to 19: audio mode in the unit word, profile in the tag
  /// byte, bit 0x10 allowed on audio; the video dynamic-range flag.
  versions_15_to_19,
  /// @brief Versions 20 and 21: as 15 to 19, after the extended header.
  versions_20_to_21,
  /// @brief Versions 22 and 23: the video unit count is masked to 11 bits.
  versions_22_to_23,
  /// @brief Versions 24 and 25: adds the video suppress-presentation flag.
  versions_24_to_25,
};

/// @brief Which congestion report layout a version uses.
enum class CongestionLayout : std::uint8_t {
  /// @brief Three u16 fields and 8 zero bytes; every version.
  standard,
};

/// @brief Which controller state payload a version uses.
enum class FeedbackStateLayout : std::uint8_t {
  /// @brief The 25-byte payload, versions 7 to 11.
  version_9,
  /// @brief The 28-byte payload with trigger states, version 12 on.
  version_12,
};

/// @brief Whether packets carry the 8-byte extended header.
enum class ExtendedHeaderPolicy : std::uint8_t {
  /// @brief Never; versions before 20.
  absent,
  /// @brief Every family but control, length-prefixed and client information;
  /// version 20 on.
  version_20_except_bootstrap_families,
};

/// @brief Whether fixed-layout reliable messages carry an 8-byte prefix.
enum class DataMessagePrefixPolicy : std::uint8_t {
  /// @brief Never; versions before 20.
  absent,
  /// @brief Data types 5, 7, 9, 11, 12 and 13 do; version 20 on.
  version_20_fixed_layout_messages,
};

/// @brief Which sessions may use a version.
enum class VersionAvailability : std::uint8_t {
  /// @brief Any session.
  every_session,
  /// @brief CloudStream sessions only.
  cloud_stream_only,
};

/// @brief The kind of session a version is chosen for.
enum class SessionScope : std::uint8_t {
  /// @brief A local or internet session with a PS4 or PS5 console.
  console,
  /// @brief A LegacyCloudStream cloud session.
  legacy_cloud_stream,
  /// @brief A CloudStream (PS5 cloud streaming) session.
  cloud_stream,
};

/// @brief One implemented Takion version and everything its wire format
/// depends on.
struct VersionRegistration {
  /// @brief The version number.
  std::uint32_t version;
  /// @brief Its feature set.
  FeatureSet features;
  /// @brief Its AV header layout.
  AvLayout av_layout;
  /// @brief Its congestion report layout.
  CongestionLayout congestion_layout;
  /// @brief Its controller state layout.
  FeedbackStateLayout feedback_state_layout;
  /// @brief Whether it uses the extended header.
  ExtendedHeaderPolicy extended_header;
  /// @brief Whether it uses the reliable message prefix.
  DataMessagePrefixPolicy data_prefix;
  /// @brief Its ECDH curve: secp256k1 for versions 7, 9, 11 and 12, secp521r1
  /// for version 10 and 13 on.
  platform::EcdhCurve ecdh_curve;
  /// @brief Which sessions may use it.
  VersionAvailability availability{VersionAvailability::every_session};
};

/// @brief Gives a version's native feature mask.
///
/// @param version The Takion version.
/// @return The mask: 0x1f for 9, 0x2f for 10, 0x5f for 11, 0xdf for 12, 0xff
///     for 13 and 14, 0x1fff for 15 to 17, 0xffff for 18, 0x1ffff for 19,
///     0x7ffff for 20, 0xfffff for 21, 0x3fffff for 22 to 25; 0 otherwise.
constexpr std::uint32_t native_takion_feature_mask(
    std::uint32_t version) noexcept {
  switch (version) {
    case 9:
      return 0x1f;
    case 10:
      return 0x2f;
    case 11:
      return 0x5f;
    case 12:
      return 0xdf;
    case 13:
    case 14:
      return 0xff;
    case 15:
    case 16:
    case 17:
      return 0x1fff;
    case 18:
      return 0xffff;
    case 19:
      return 0x1ffff;
    case 20:
      return 0x7ffff;
    case 21:
      return 0xfffff;
    case 22:
    case 23:
    case 24:
    case 25:
      return 0x3fffff;
    default:
      return 0;
  }
}

/// @brief Gives a version's native feature set.
///
/// @param version The Takion version.
/// @return The set; empty for a version without one.
constexpr FeatureSet native_takion_features(std::uint32_t version) noexcept {
  return FeatureSet::from_bits(native_takion_feature_mask(version));
}

/// @brief Builds a feature set from a list.
///
/// @param features The features.
/// @return The set.
constexpr FeatureSet takion_features(
    std::initializer_list<Feature> features) noexcept {
  auto result = FeatureSet{};
  for (const auto feature : features) result = result.with(feature);
  return result;
}

/// @brief Every implemented version, ascending.
inline constexpr VersionRegistration takion_version_registry[]{
    {7,
     {},
     AvLayout::version_7,
     CongestionLayout::standard,
     FeedbackStateLayout::version_9,
     ExtendedHeaderPolicy::absent,
     DataMessagePrefixPolicy::absent,
     platform::EcdhCurve::secp256k1},
    {9, native_takion_features(9), AvLayout::versions_9_to_10,
     CongestionLayout::standard, FeedbackStateLayout::version_9,
     ExtendedHeaderPolicy::absent, DataMessagePrefixPolicy::absent,
     platform::EcdhCurve::secp256k1},
    {10, native_takion_features(10), AvLayout::versions_9_to_10,
     CongestionLayout::standard, FeedbackStateLayout::version_9,
     ExtendedHeaderPolicy::absent, DataMessagePrefixPolicy::absent,
     platform::EcdhCurve::secp521r1},
    {11, native_takion_features(11), AvLayout::version_11,
     CongestionLayout::standard, FeedbackStateLayout::version_9,
     ExtendedHeaderPolicy::absent, DataMessagePrefixPolicy::absent,
     platform::EcdhCurve::secp256k1},
    {12, native_takion_features(12), AvLayout::versions_12_to_14,
     CongestionLayout::standard, FeedbackStateLayout::version_12,
     ExtendedHeaderPolicy::absent, DataMessagePrefixPolicy::absent,
     platform::EcdhCurve::secp256k1},
    {13, native_takion_features(13), AvLayout::versions_12_to_14,
     CongestionLayout::standard, FeedbackStateLayout::version_12,
     ExtendedHeaderPolicy::absent, DataMessagePrefixPolicy::absent,
     platform::EcdhCurve::secp521r1},
    {14, native_takion_features(14), AvLayout::versions_12_to_14,
     CongestionLayout::standard, FeedbackStateLayout::version_12,
     ExtendedHeaderPolicy::absent, DataMessagePrefixPolicy::absent,
     platform::EcdhCurve::secp521r1},
    {15, native_takion_features(15), AvLayout::versions_15_to_19,
     CongestionLayout::standard, FeedbackStateLayout::version_12,
     ExtendedHeaderPolicy::absent, DataMessagePrefixPolicy::absent,
     platform::EcdhCurve::secp521r1},
    {16, native_takion_features(16), AvLayout::versions_15_to_19,
     CongestionLayout::standard, FeedbackStateLayout::version_12,
     ExtendedHeaderPolicy::absent, DataMessagePrefixPolicy::absent,
     platform::EcdhCurve::secp521r1},
    {17, native_takion_features(17), AvLayout::versions_15_to_19,
     CongestionLayout::standard, FeedbackStateLayout::version_12,
     ExtendedHeaderPolicy::absent, DataMessagePrefixPolicy::absent,
     platform::EcdhCurve::secp521r1},
    {18, native_takion_features(18), AvLayout::versions_15_to_19,
     CongestionLayout::standard, FeedbackStateLayout::version_12,
     ExtendedHeaderPolicy::absent, DataMessagePrefixPolicy::absent,
     platform::EcdhCurve::secp521r1},
    {19, native_takion_features(19), AvLayout::versions_15_to_19,
     CongestionLayout::standard, FeedbackStateLayout::version_12,
     ExtendedHeaderPolicy::absent, DataMessagePrefixPolicy::absent,
     platform::EcdhCurve::secp521r1},
    {20, native_takion_features(20), AvLayout::versions_20_to_21,
     CongestionLayout::standard, FeedbackStateLayout::version_12,
     ExtendedHeaderPolicy::version_20_except_bootstrap_families,
     DataMessagePrefixPolicy::version_20_fixed_layout_messages,
     platform::EcdhCurve::secp521r1},
    {21, native_takion_features(21), AvLayout::versions_20_to_21,
     CongestionLayout::standard, FeedbackStateLayout::version_12,
     ExtendedHeaderPolicy::version_20_except_bootstrap_families,
     DataMessagePrefixPolicy::version_20_fixed_layout_messages,
     platform::EcdhCurve::secp521r1, VersionAvailability::cloud_stream_only},
    {23, native_takion_features(23), AvLayout::versions_22_to_23,
     CongestionLayout::standard, FeedbackStateLayout::version_12,
     ExtendedHeaderPolicy::version_20_except_bootstrap_families,
     DataMessagePrefixPolicy::version_20_fixed_layout_messages,
     platform::EcdhCurve::secp521r1, VersionAvailability::cloud_stream_only},
    {25, native_takion_features(25), AvLayout::versions_24_to_25,
     CongestionLayout::standard, FeedbackStateLayout::version_12,
     ExtendedHeaderPolicy::version_20_except_bootstrap_families,
     DataMessagePrefixPolicy::version_20_fixed_layout_messages,
     platform::EcdhCurve::secp521r1, VersionAvailability::cloud_stream_only},
};

/// @brief Looks a version up in the registry.
///
/// @param version The Takion version.
/// @return Its registration; nothing when it is not implemented.
constexpr auto takion_version_registration(std::uint32_t version) noexcept
    -> std::optional<VersionRegistration> {
  const auto found = std::ranges::find(takion_version_registry, version,
                                       &VersionRegistration::version);
  if (found == std::ranges::end(takion_version_registry)) return std::nullopt;
  return *found;
}

/// @brief Tells whether a version is implemented.
///
/// @param version The Takion version.
/// @return Whether it is in the registry.
constexpr bool is_implemented_takion_version(std::uint32_t version) noexcept {
  return takion_version_registration(version).has_value();
}

/// @brief Tells whether a version is reserved for CloudStream.
///
/// @param version The Takion version.
/// @return Whether it is implemented and CloudStream only.
constexpr bool is_cloud_stream_only_takion_version(
    std::uint32_t version) noexcept {
  const auto registration = takion_version_registration(version);
  return registration &&
         registration->availability == VersionAvailability::cloud_stream_only;
}

/// @brief Tells whether a kind of session may use a version.
///
/// @param version The Takion version.
/// @param scope The kind of session.
/// @return Whether it is implemented and, if CloudStream only, the session is
/// PS
///     Cloud.
constexpr bool is_takion_version_available(std::uint32_t version,
                                           SessionScope scope) noexcept {
  return is_implemented_takion_version(version) &&
         (!is_cloud_stream_only_takion_version(version) ||
          scope == SessionScope::cloud_stream);
}

/// @brief Picks the version a session will use.
///
/// @param base The target's default version.
/// @param takion_version_override A runtime override, such as a developer
///     setting; 0 for none.
/// @param scope The kind of session.
/// @return The runtime override when available to the session, else the build
///     override when available, else `base`.
constexpr std::uint32_t select_takion_version(
    std::uint32_t base, std::uint32_t takion_version_override,
    SessionScope scope = SessionScope::console) noexcept {
  if (is_takion_version_available(takion_version_override, scope))
    return takion_version_override;
  if (is_takion_version_available(takion_build_version_override, scope))
    return takion_build_version_override;
  return base;
}

/// @brief The kinds of far end a build pins a version for.
enum class BuildTarget : std::uint8_t {
  /// @brief PS4 consoles.
  ps4,
  /// @brief PS5 consoles.
  ps5,
  /// @brief LegacyCloudStream.
  legacy_cloud_stream,
  /// @brief CloudStream.
  cloud_stream,
};

/// @brief Gives the version a build uses for a target, with the build override
/// applied.
///
/// @param target The far end.
/// @return The version.
constexpr std::uint32_t takion_build_version(BuildTarget target) noexcept {
  switch (target) {
    case BuildTarget::ps4:
      return select_takion_version(takion_version_ps4, 0,
                                   SessionScope::console);
    case BuildTarget::ps5:
      return select_takion_version(takion_version_ps5, 0,
                                   SessionScope::console);
    case BuildTarget::legacy_cloud_stream:
      return select_takion_version(takion_version_legacy_cloud_stream, 0,
                                   SessionScope::legacy_cloud_stream);
    case BuildTarget::cloud_stream:
      return select_takion_version(takion_version_cloud_stream, 0,
                                   SessionScope::cloud_stream);
  }
  return 0;
}

/// @brief The versions a negotiation offers when nothing narrower is asked
/// for: every implemented version from 9, ascending.
inline constexpr std::array<std::uint32_t, 14> takion_cloud_default_offer{
    9, 10, 11, 13, 14, 15, 16, 17, 18, 19, 20, 21, 23, 25};

/// @brief Lists the versions a kind of session may offer in negotiation.
///
/// @param scope The kind of session.
/// @return The default offer filtered to the versions available to `scope`.
inline auto takion_negotiation_offer(SessionScope scope)
    -> std::vector<std::uint32_t> {
  std::vector<std::uint32_t> offer;
  for (const auto version : takion_cloud_default_offer) {
    if (is_takion_version_available(version, scope)) offer.push_back(version);
  }
  return offer;
}

/// @brief Lists the implemented versions a kind of session may use.
///
/// @param scope The kind of session.
/// @return Every registered version available to `scope`, ascending.
inline auto takion_versions_available_to(SessionScope scope)
    -> std::vector<std::uint32_t> {
  std::vector<std::uint32_t> versions;
  for (const auto& registration : takion_version_registry)
    if (is_takion_version_available(registration.version, scope))
      versions.push_back(registration.version);
  return versions;
}

static_assert(
    takion_build_version_override == 0 ||
        is_implemented_takion_version(takion_build_version_override),
    "LIBCHIAKING_TAKION_VERSION_OVERRIDE must be 0 or an implemented Takion "
    "version");

/// @brief Checks that the default console negotiation list is console-safe.
///
/// @return Whether every version in it is available to console sessions.
constexpr bool default_negotiated_versions_are_console_safe() noexcept {
  for (const auto version : default_negotiated_takion_versions) {
    if (!is_takion_version_available(version, SessionScope::console))
      return false;
  }
  return true;
}

static_assert(default_negotiated_versions_are_console_safe());

static_assert(is_takion_version_available(takion_version_ps4,
                                          SessionScope::console),
              "LIBCHIAKING_TAKION_VERSION_PS4 must be an implemented console "
              "Takion version");
static_assert(is_takion_version_available(takion_version_ps5,
                                          SessionScope::console),
              "LIBCHIAKING_TAKION_VERSION_PS5 must be an implemented console "
              "Takion version");
static_assert(
    is_takion_version_available(takion_version_legacy_cloud_stream,
                                SessionScope::legacy_cloud_stream),
    "LIBCHIAKING_TAKION_VERSION_LEGACY_CLOUD_STREAM must be an implemented "
    "LegacyCloudStream Takion "
    "version");
static_assert(is_takion_version_available(takion_version_cloud_stream,
                                          SessionScope::cloud_stream),
              "LIBCHIAKING_TAKION_VERSION_CLOUD_STREAM must be an implemented "
              "CloudStream Takion "
              "version");

/// @brief The wire format of one Takion version, resolved from the registry.
///
/// Only resolve_takion_wire_profile() makes one, so every profile names an
/// implemented version. A plain constexpr value, safe to copy and share.
class WireProfile {
 public:
  /// @brief Reads the version.
  ///
  /// @return The version number.
  constexpr std::uint32_t version() const noexcept {
    return registration_.version;
  }
  /// @brief Reads the feature set.
  ///
  /// @return The features.
  constexpr FeatureSet features() const noexcept {
    return registration_.features;
  }
  /// @brief Reads the AV header layout.
  ///
  /// @return The layout.
  constexpr AvLayout av_layout() const noexcept {
    return registration_.av_layout;
  }
  /// @brief Reads the congestion layout.
  ///
  /// @return The layout.
  constexpr CongestionLayout congestion_layout() const noexcept {
    return registration_.congestion_layout;
  }
  /// @brief Reads the controller state layout.
  ///
  /// @return The layout.
  constexpr FeedbackStateLayout feedback_state_layout() const noexcept {
    return registration_.feedback_state_layout;
  }
  /// @brief Reads the extended header policy.
  ///
  /// @return The policy.
  constexpr ExtendedHeaderPolicy extended_header() const noexcept {
    return registration_.extended_header;
  }
  /// @brief Reads the reliable message prefix policy.
  ///
  /// @return The policy.
  constexpr DataMessagePrefixPolicy data_prefix() const noexcept {
    return registration_.data_prefix;
  }
  /// @brief Reads the ECDH curve.
  ///
  /// @return The curve.
  constexpr platform::EcdhCurve ecdh_curve() const noexcept {
    return registration_.ecdh_curve;
  }
  /// @brief Tells whether a packet of a type carries the extended header.
  ///
  /// @param type The type byte.
  /// @return Whether the version uses it and the family has one.
  constexpr bool uses_extended_header(PacketType type) const noexcept {
    return registration_.extended_header ==
               ExtendedHeaderPolicy::version_20_except_bootstrap_families &&
           PacketType::has_extended_header(type.family_code());
  }
  /// @brief Tells the size of a packet's headers before its payload.
  ///
  /// @param type The type byte.
  /// @return 1, or 9 with the extended header.
  constexpr std::size_t packet_header_size(PacketType type) const noexcept {
    return packet_type_size +
           (uses_extended_header(type) ? extended_header_size : 0);
  }
  /// @brief Tells the size of a reliable message's prefix.
  ///
  /// @param type The message's data type.
  /// @return 8 for a fixed-layout type from version 20, 0 otherwise.
  constexpr std::size_t data_message_prefix_size(
      MessageDataType type) const noexcept {
    return registration_.data_prefix == DataMessagePrefixPolicy::
                                            version_20_fixed_layout_messages &&
                   MessageDataType::has_fixed_prefix(type.raw)
               ? fixed_data_message_prefix_size
               : 0;
  }

 private:
  /// @brief Lets the resolver construct profiles.
  ///
  /// @return The profile, or nothing.
  friend constexpr auto resolve_takion_wire_profile(std::uint32_t) noexcept
      -> std::optional<WireProfile>;

  /// @brief Wraps a registration.
  ///
  /// @param registration The version's registration.
  explicit constexpr WireProfile(
      const VersionRegistration& registration) noexcept
      : registration_(registration) {}

  /// @brief The version's registration.
  VersionRegistration registration_;
};

/// @brief Resolves a version to its wire profile.
///
/// @param version The Takion version.
/// @return The profile; nothing when the version is not implemented.
[[nodiscard]] constexpr auto resolve_takion_wire_profile(
    std::uint32_t version) noexcept -> std::optional<WireProfile> {
  return takion_version_registration(version).transform(
      [](const VersionRegistration& registration) {
        return WireProfile{registration};
      });
}

}  // namespace libchiaking::net::protocol::takion
