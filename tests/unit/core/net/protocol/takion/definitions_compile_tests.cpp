#include "src/core/net/protocol/takion/detail/handshake_types.hpp"
#include "src/core/net/protocol/takion/detail/packet_types.hpp"
#include "src/core/net/protocol/takion/profile.hpp"

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>

namespace takion_test {
namespace {

namespace platform = libchiaking::platform;
namespace takion = libchiaking::net::protocol::takion;

class TakionDefinitionRules {
 public:
  static constexpr std::uint32_t build_pinned =
      takion::takion_build_version_override;

  static constexpr std::uint32_t spec_feature_mask(std::uint32_t version) {
    constexpr std::uint32_t masks[]{
        0x1f,    0x2f,     0x5f,     0xdf,     0xff,     0xff,
        0x1fff,  0x1fff,   0x1fff,   0xffff,   0x1ffff,  0x7ffff,
        0xfffff, 0x3fffff, 0x3fffff, 0x3fffff, 0x3fffff,
    };
    return version >= 9 && version <= 25 ? masks[version - 9] : 0;
  }

  static constexpr bool native_masks_are_exact() {
    for (std::uint32_t version = 0; version < 64; ++version) {
      if (takion::native_takion_feature_mask(version) !=
          spec_feature_mask(version))
        return false;
      if (takion::native_takion_features(version).bits() !=
          spec_feature_mask(version))
        return false;
    }
    return true;
  }

  static constexpr bool version_registry_is_exact() {
    constexpr std::uint32_t expected_versions[]{
        7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 23, 25,
    };
    constexpr auto registry_count = sizeof(takion::takion_version_registry) /
                                    sizeof(takion::takion_version_registry[0]);
    constexpr auto expected_count =
        sizeof(expected_versions) / sizeof(expected_versions[0]);
    if (registry_count != expected_count) return false;

    for (std::size_t index = 0; index < expected_count; ++index) {
      const auto& registration = takion::takion_version_registry[index];
      if (registration.version != expected_versions[index]) return false;
    }

    for (std::uint32_t version = 0; version < 32; ++version) {
      const bool known = version == 7 || (version >= 9 && version <= 21) ||
                         version == 23 || version == 25;
      if (takion::is_implemented_takion_version(version) != known ||
          takion::takion_version_registration(version).has_value() != known)
        return false;
    }
    return true;
  }

  static constexpr bool cloud_stream_only_versions_are_exact() {
    for (std::uint32_t version = 0; version < 64; ++version) {
      const bool cloud_only = version == 21 || version == 23 || version == 25;
      if (takion::is_cloud_stream_only_takion_version(version) != cloud_only)
        return false;
      const bool implemented = takion::is_implemented_takion_version(version);
      if (takion::is_takion_version_available(
              version, takion::SessionScope::cloud_stream) != implemented)
        return false;
      for (const auto scope : {takion::SessionScope::console,
                               takion::SessionScope::legacy_cloud_stream}) {
        if (takion::is_takion_version_available(version, scope) !=
            (implemented && !cloud_only))
          return false;
      }
    }
    return true;
  }

  static constexpr std::uint32_t pinned_or(std::uint32_t base,
                                           takion::SessionScope scope) {
    if (build_pinned == 0) return base;
    return takion::is_takion_version_available(build_pinned, scope)
               ? build_pinned
               : base;
  }

  static constexpr bool cloud_only_overrides_fall_back() {
    for (const auto version :
         {std::uint32_t{21}, std::uint32_t{23}, std::uint32_t{25}}) {
      if (takion::select_takion_version(15, version) !=
          pinned_or(15, takion::SessionScope::console))
        return false;
      if (takion::select_takion_version(9, version,
                                        takion::SessionScope::console) !=
          pinned_or(9, takion::SessionScope::console))
        return false;
      if (takion::select_takion_version(
              9, version, takion::SessionScope::legacy_cloud_stream) !=
          pinned_or(9, takion::SessionScope::legacy_cloud_stream))
        return false;
      if (takion::select_takion_version(
              12, version, takion::SessionScope::cloud_stream) != version)
        return false;
    }
    return takion::select_takion_version(12, 22,
                                         takion::SessionScope::cloud_stream) ==
               pinned_or(12, takion::SessionScope::cloud_stream) &&
           takion::select_takion_version(12, 24,
                                         takion::SessionScope::cloud_stream) ==
               pinned_or(12, takion::SessionScope::cloud_stream) &&
           takion::select_takion_version(15, 20) == 20;
  }

  static constexpr bool build_pin_respects_scope() {
    if (build_pinned == 0) return true;
    const bool cloud_only =
        takion::is_cloud_stream_only_takion_version(build_pinned);
    return takion::select_takion_version(15, 0) ==
               (cloud_only ? 15U : build_pinned) &&
           takion::select_takion_version(
               9, 0, takion::SessionScope::legacy_cloud_stream) ==
               (cloud_only ? 9U : build_pinned) &&
           takion::select_takion_version(
               12, 0, takion::SessionScope::cloud_stream) == build_pinned;
  }

  static constexpr bool expected_named_feature(std::uint32_t version,
                                               takion::Feature feature) {
    return (spec_feature_mask(version) >> static_cast<std::uint8_t>(feature)) &
           1U;
  }

  static constexpr takion::AvLayout expected_av_layout(std::uint32_t version) {
    return version == 7    ? takion::AvLayout::version_7
           : version <= 10 ? takion::AvLayout::versions_9_to_10
           : version == 11 ? takion::AvLayout::version_11
           : version <= 14 ? takion::AvLayout::versions_12_to_14
           : version <= 19 ? takion::AvLayout::versions_15_to_19
           : version <= 21 ? takion::AvLayout::versions_20_to_21
           : version <= 23 ? takion::AvLayout::versions_22_to_23
                           : takion::AvLayout::versions_24_to_25;
  }

  static constexpr takion::FeedbackStateLayout expected_feedback_state_layout(
      std::uint32_t version) {
    switch (version) {
      case 7:
      case 9:
      case 10:
      case 11:
        return takion::FeedbackStateLayout::version_9;
      case 12:
      case 13:
      case 14:
      case 15:
      case 16:
      case 17:
      case 18:
      case 19:
      case 20:
      case 21:
      case 23:
      case 25:
        return takion::FeedbackStateLayout::version_12;
      default:
        return takion::FeedbackStateLayout::version_9;
    }
  }

  static constexpr bool profile_registry_is_exact() {
    constexpr takion::Feature named_features[]{
        takion::Feature::unresolved_feature_0,
        takion::Feature::packet_authentication,
        takion::Feature::trace_messages,
        takion::Feature::audio_processing,
        takion::Feature::direct_message_encryption,
        takion::Feature::p521_ecdh,
        takion::Feature::separate_audio_stream,
        takion::Feature::unresolved_feature_7,
        takion::Feature::generic_control_data,
        takion::Feature::automatic_dynamic_range,
        takion::Feature::audio_profiles,
        takion::Feature::av_unit_count_only_encoding,
        takion::Feature::render_metrics,
        takion::Feature::extended_messages,
        takion::Feature::unresolved_feature_14,
        takion::Feature::audio_channel_count_state,
        takion::Feature::wide_audio_available_bits,
        takion::Feature::extended_packet_headers,
        takion::Feature::unresolved_feature_18,
        takion::Feature::expanded_idr_requests,
        takion::Feature::video_frame_metrics,
        takion::Feature::received_tos_metrics,
    };

    for (std::uint32_t version = 0; version < 32; ++version) {
      const auto profile = takion::resolve_takion_wire_profile(version);
      const takion::VersionRegistration* registration = nullptr;
      if (profile.has_value() != takion::is_implemented_takion_version(version))
        return false;
      if (!profile) continue;

      for (const auto& row : takion::takion_version_registry) {
        if (row.version != version) continue;
        registration = &row;
      }
      auto expected_features = takion::FeatureSet{};
      for (const auto feature : named_features) {
        if (expected_named_feature(version, feature))
          expected_features = expected_features.with(feature);
      }
      if (registration == nullptr ||
          registration->features.bits() != expected_features.bits() ||
          registration->av_layout != expected_av_layout(version) ||
          registration->congestion_layout !=
              takion::CongestionLayout::standard ||
          registration->feedback_state_layout !=
              expected_feedback_state_layout(version) ||
          (registration->extended_header ==
           takion::ExtendedHeaderPolicy::
               version_20_except_bootstrap_families) != (version >= 20) ||
          (registration->data_prefix ==
           takion::DataMessagePrefixPolicy::version_20_fixed_layout_messages) !=
              (version >= 20) ||
          registration->ecdh_curve !=
              (expected_named_feature(version, takion::Feature::p521_ecdh)
                   ? platform::EcdhCurve::secp521r1
                   : platform::EcdhCurve::secp256k1))
        return false;

      if (profile->version() != version ||
          profile->av_layout() != expected_av_layout(version) ||
          profile->congestion_layout() != takion::CongestionLayout::standard ||
          profile->feedback_state_layout() !=
              expected_feedback_state_layout(version))
        return false;

      for (const auto feature : named_features) {
        if (profile->features().contains(feature) !=
            expected_named_feature(version, feature))
          return false;
      }

      const bool is_v20 = version >= 20;
      if ((profile->extended_header() ==
           takion::ExtendedHeaderPolicy::
               version_20_except_bootstrap_families) != is_v20 ||
          (profile->data_prefix() ==
           takion::DataMessagePrefixPolicy::version_20_fixed_layout_messages) !=
              is_v20)
        return false;
      const auto expected_curve =
          expected_named_feature(version, takion::Feature::p521_ecdh)
              ? platform::EcdhCurve::secp521r1
              : platform::EcdhCurve::secp256k1;
      if (profile->ecdh_curve() != expected_curve) return false;
    }
    return true;
  }

  static constexpr bool extended_header_rules_are_exact() {
    for (std::uint8_t code = 0; code < 16; ++code) {
      const bool expected = code != 0 && code != 4 && code != 8;
      if (takion::PacketType::has_extended_header(code) != expected)
        return false;
      const auto before = takion::resolve_takion_wire_profile(19);
      if (!before || before->packet_header_size(takion::PacketType{code}) != 1U)
        return false;
      for (const auto later : {std::uint32_t{20}, std::uint32_t{21},
                               std::uint32_t{23}, std::uint32_t{25}}) {
        const auto profile = takion::resolve_takion_wire_profile(later);
        if (!profile || profile->packet_header_size(takion::PacketType{code}) !=
                            1U + (expected ? 8U : 0U))
          return false;
      }
    }
    return true;
  }

  static constexpr bool data_prefix_rules_are_exact() {
    for (std::uint8_t code = 0; code < 16; ++code) {
      const bool expected = code == 5 || code == 7 || code == 9 || code == 11 ||
                            code == 12 || code == 13;
      if (takion::MessageDataType::has_fixed_prefix(code) != expected)
        return false;
      const auto before = takion::resolve_takion_wire_profile(19);
      if (!before ||
          before->data_message_prefix_size(takion::MessageDataType{code}) != 0)
        return false;
      for (const auto later : {std::uint32_t{20}, std::uint32_t{21},
                               std::uint32_t{23}, std::uint32_t{25}}) {
        const auto profile = takion::resolve_takion_wire_profile(later);
        if (!profile ||
            profile->data_message_prefix_size(takion::MessageDataType{code}) !=
                (expected ? 8U : 0U))
          return false;
      }
    }
    return true;
  }
};

static_assert(static_cast<std::uint8_t>(takion::PacketFamily::control) == 0x00);
static_assert(
    static_cast<std::uint8_t>(takion::PacketFamily::feedback_history) == 0x01);
static_assert(static_cast<std::uint8_t>(takion::PacketFamily::video) == 0x02);
static_assert(
    static_cast<std::uint8_t>(takion::PacketFamily::audio_and_haptics) == 0x03);
static_assert(
    static_cast<std::uint8_t>(takion::PacketFamily::length_prefixed) == 0x04);
static_assert(static_cast<std::uint8_t>(takion::PacketFamily::congestion) ==
              0x05);
static_assert(static_cast<std::uint8_t>(takion::PacketFamily::feedback_state) ==
              0x06);
static_assert(static_cast<std::uint8_t>(
                  takion::PacketFamily::client_information) == 0x08);
static_assert(static_cast<std::uint8_t>(takion::PacketFamily::trace) == 0x0a);
static_assert(static_cast<std::uint8_t>(takion::PacketFamily::generic_data) ==
              0x0c);
static_assert(
    static_cast<std::uint8_t>(takion::PacketFamily::extended_message) == 0x0e);

constexpr takion::PacketType flagged_video{0xa2};
static_assert(flagged_video.raw == 0xa2);
static_assert(flagged_video.family_code() == 0x02);
static_assert(flagged_video.flags() == 0xa0);
static_assert(flagged_video.family() == takion::PacketFamily::video);
static_assert(takion::PacketType{0xf7}.raw == 0xf7);
static_assert(!takion::PacketType{0xf7}.is_known());

static_assert(static_cast<std::uint8_t>(takion::ControlChunk::data) == 0x00);
static_assert(static_cast<std::uint8_t>(takion::ControlChunk::init) == 0x01);
static_assert(static_cast<std::uint8_t>(
                  takion::ControlChunk::init_acknowledgement) == 0x02);
static_assert(static_cast<std::uint8_t>(
                  takion::ControlChunk::data_acknowledgement) == 0x03);
static_assert(static_cast<std::uint8_t>(takion::ControlChunk::cookie) == 0x0a);
static_assert(static_cast<std::uint8_t>(
                  takion::ControlChunk::cookie_acknowledgement) == 0x0b);
static_assert(takion::ControlChunkType{0xfe}.raw == 0xfe);
static_assert(!takion::ControlChunkType{0xfe}.known().has_value());

static_assert(static_cast<std::uint8_t>(takion::MessageDataKind::protobuf) ==
              0x00);
static_assert(static_cast<std::uint8_t>(takion::MessageDataKind::rumble) ==
              0x07);
static_assert(static_cast<std::uint8_t>(
                  takion::MessageDataKind::pad_information) == 0x09);
static_assert(static_cast<std::uint8_t>(
                  takion::MessageDataKind::trigger_effects) == 0x0b);
static_assert(static_cast<std::uint8_t>(
                  takion::MessageDataKind::controller_message_13) == 0x0d);
static_assert(takion::MessageDataType{0x0d}.known() ==
              takion::MessageDataKind::controller_message_13);
static_assert(takion::MessageDataType{0x0e}.raw == 0x0e);
static_assert(!takion::MessageDataType{0x0e}.known().has_value());

static_assert(takion::control_header_size == 16);
static_assert(takion::handshake_cookie_size == 32);
static_assert(takion::default_advertised_receive_window == 0x19000);
static_assert(takion::default_outbound_stream_count == 100);
static_assert(takion::default_inbound_stream_count == 100);
static_assert(takion::psn_envelope_size == 4);
static_assert(takion::extended_header_size == 8);
static_assert(takion::fixed_data_message_prefix_size == 8);
static_assert(std::tuple_size_v<takion::HandshakeCookie> ==
              takion::handshake_cookie_size);

static_assert(takion::takion_version_ps4 == LIBCHIAKING_TAKION_VERSION_PS4);
static_assert(takion::takion_version_ps5 == LIBCHIAKING_TAKION_VERSION_PS5);
static_assert(takion::takion_version_legacy_cloud_stream ==
              LIBCHIAKING_TAKION_VERSION_LEGACY_CLOUD_STREAM);
static_assert(takion::takion_version_cloud_stream ==
              LIBCHIAKING_TAKION_VERSION_CLOUD_STREAM);
static_assert(takion::is_takion_version_available(
    takion::takion_version_ps4, takion::SessionScope::console));
static_assert(takion::is_takion_version_available(
    takion::takion_version_ps5, takion::SessionScope::console));
static_assert(takion::is_takion_version_available(
    takion::takion_version_legacy_cloud_stream,
    takion::SessionScope::legacy_cloud_stream));
static_assert(takion::is_takion_version_available(
    takion::takion_version_cloud_stream, takion::SessionScope::cloud_stream));
static_assert(
    TakionDefinitionRules::build_pinned == 0 ||
    takion::is_implemented_takion_version(TakionDefinitionRules::build_pinned));
static_assert(takion::select_takion_version(15, 0) ==
              (TakionDefinitionRules::build_pinned == 0 ||
                       takion::is_cloud_stream_only_takion_version(
                           TakionDefinitionRules::build_pinned)
                   ? 15
                   : TakionDefinitionRules::build_pinned));
static_assert(takion::select_takion_version(9, 0) ==
              (TakionDefinitionRules::build_pinned == 0 ||
                       takion::is_cloud_stream_only_takion_version(
                           TakionDefinitionRules::build_pinned)
                   ? 9
                   : TakionDefinitionRules::build_pinned));
static_assert(takion::select_takion_version(15, 12) == 12);
static_assert(takion::select_takion_version(9, 20) == 20);
static_assert(takion::select_takion_version(15, 8) ==
              takion::select_takion_version(15, 0));
static_assert(takion::select_takion_version(15, 0xffffffffu) ==
              takion::select_takion_version(15, 0));

static_assert(
    static_cast<std::uint8_t>(takion::Feature::unresolved_feature_0) == 0);
static_assert(
    static_cast<std::uint8_t>(takion::Feature::packet_authentication) == 1);
static_assert(static_cast<std::uint8_t>(takion::Feature::trace_messages) == 2);
static_assert(static_cast<std::uint8_t>(takion::Feature::audio_processing) ==
              3);
static_assert(
    static_cast<std::uint8_t>(takion::Feature::direct_message_encryption) == 4);
static_assert(static_cast<std::uint8_t>(takion::Feature::p521_ecdh) == 5);
static_assert(
    static_cast<std::uint8_t>(takion::Feature::separate_audio_stream) == 6);
static_assert(
    static_cast<std::uint8_t>(takion::Feature::unresolved_feature_7) == 7);
static_assert(
    static_cast<std::uint8_t>(takion::Feature::generic_control_data) == 8);
static_assert(
    static_cast<std::uint8_t>(takion::Feature::automatic_dynamic_range) == 9);
static_assert(static_cast<std::uint8_t>(takion::Feature::audio_profiles) == 10);
static_assert(static_cast<std::uint8_t>(
                  takion::Feature::av_unit_count_only_encoding) == 11);
static_assert(static_cast<std::uint8_t>(takion::Feature::render_metrics) == 12);
static_assert(static_cast<std::uint8_t>(takion::Feature::extended_messages) ==
              13);
static_assert(
    static_cast<std::uint8_t>(takion::Feature::unresolved_feature_14) == 14);
static_assert(static_cast<std::uint8_t>(
                  takion::Feature::audio_channel_count_state) == 15);
static_assert(static_cast<std::uint8_t>(
                  takion::Feature::wide_audio_available_bits) == 16);
static_assert(
    static_cast<std::uint8_t>(takion::Feature::extended_packet_headers) == 17);
static_assert(
    static_cast<std::uint8_t>(takion::Feature::unresolved_feature_18) == 18);
static_assert(
    static_cast<std::uint8_t>(takion::Feature::expanded_idr_requests) == 19);
static_assert(static_cast<std::uint8_t>(takion::Feature::video_frame_metrics) ==
              20);
static_assert(
    static_cast<std::uint8_t>(takion::Feature::received_tos_metrics) == 21);

static_assert(TakionDefinitionRules::native_masks_are_exact());
static_assert(takion::native_takion_features(21).contains(
    takion::Feature::expanded_idr_requests));
static_assert(!takion::native_takion_features(20).contains(
    takion::Feature::expanded_idr_requests));
static_assert(takion::native_takion_features(23).contains(
    takion::Feature::video_frame_metrics));
static_assert(takion::native_takion_features(25).contains(
    takion::Feature::received_tos_metrics));
static_assert(!takion::native_takion_features(21).contains(
    takion::Feature::video_frame_metrics));
static_assert(takion::native_takion_features(19).contains(
    takion::Feature::wide_audio_available_bits));
static_assert(!takion::native_takion_features(18).contains(
    takion::Feature::wide_audio_available_bits));
static_assert(!takion::native_takion_features(10).contains(
    takion::Feature::direct_message_encryption));
static_assert(takion::native_takion_features(11).contains(
    takion::Feature::direct_message_encryption));

constexpr auto features = takion::FeatureSet{}
                              .with(takion::Feature::p521_ecdh)
                              .with(takion::Feature::extended_packet_headers);
static_assert(features.contains(takion::Feature::p521_ecdh));
static_assert(features.contains(takion::Feature::extended_packet_headers));
static_assert(!features.contains(takion::Feature::unresolved_feature_7));

static_assert(TakionDefinitionRules::version_registry_is_exact());

static_assert(TakionDefinitionRules::cloud_stream_only_versions_are_exact());

static_assert(TakionDefinitionRules::cloud_only_overrides_fall_back());

static_assert(TakionDefinitionRules::build_pin_respects_scope());

static_assert(TakionDefinitionRules::profile_registry_is_exact());

static_assert(TakionDefinitionRules::extended_header_rules_are_exact());
static_assert(takion::resolve_takion_wire_profile(20)->packet_header_size(
                  takion::PacketType{0xf2}) == 9);

static_assert(TakionDefinitionRules::data_prefix_rules_are_exact());
static_assert(takion::MessageDataType{5}.raw == 5);
static_assert(!takion::MessageDataType{5}.known().has_value());

static_assert(TakionDefinitionRules::expected_av_layout(9) ==
              takion::AvLayout::versions_9_to_10);
static_assert(TakionDefinitionRules::expected_av_layout(10) ==
              takion::AvLayout::versions_9_to_10);
static_assert(TakionDefinitionRules::expected_av_layout(11) ==
              takion::AvLayout::version_11);
static_assert(takion::resolve_takion_wire_profile(10)->av_layout() ==
              takion::AvLayout::versions_9_to_10);
static_assert(takion::resolve_takion_wire_profile(11)->av_layout() ==
              takion::AvLayout::version_11);
static_assert(takion::resolve_takion_wire_profile(12)->av_layout() ==
              takion::AvLayout::versions_12_to_14);

static_assert(!std::is_aggregate_v<takion::WireProfile>);
static_assert(!std::is_constructible_v<
              takion::WireProfile, std::uint32_t, takion::FeatureSet,
              takion::AvLayout, takion::ExtendedHeaderPolicy,
              takion::DataMessagePrefixPolicy, platform::EcdhCurve>);

}  // namespace
}  // namespace takion_test
