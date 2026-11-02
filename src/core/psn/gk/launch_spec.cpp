#include "src/core/psn/gk/launch_spec.hpp"

#include "src/core/psn/gk/client.hpp"
#include "src/core/psn/legacy_cloud_stream_endpoints.hpp"
#include "src/core/psn/user_agents.hpp"
#include "src/core/utils/json/json.hpp"
#include "src/core/utils/json/ps_reply.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/integer.hpp"
#include "src/core/utils/text/utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace libchiaking::psn::gk {

struct LaunchSpec::Input {
  using keys = json::ps::CamelCaseKeys;
  std::vector<std::string> controllers;
};

struct LaunchSpec::VideoStreamSettings {
  using keys = json::ps::CamelCaseKeys;
  int client_height{};
  int supported_max_resolution{};
  std::vector<std::string> supported_video_encoder_profiles{"hevc_hw4"};
  std::string supported_dynamic_range{"sdr"};
  int preferred_max_resolution{};
  std::string preferred_dynamic_range{"sdr"};
  int hq_mode{1};
};

struct LaunchSpec::AudioStreamSettings {
  using keys = json::ps::CamelCaseKeys;
  std::string audio_encoder_profile{"default"};
  std::string max_audio_channels{"7.1.4"};
  std::string preferred_number_audio_channels;
};

struct LaunchSpec::CloudStream {
  std::string entitlement_id;
  std::string np_env{"np"};
  std::string language;
  std::string cloud_endpoint{Client::cloud_endpoint};
  std::string redirect_uri{psn::cloud_stream_redirect_uri};
  std::string resolution_setting;
  int client_width{};
  int client_height{};
  std::string adaptive_stream_mode{"resize"};
  bool use_client_bw_ladder{true};
  bool audio_upload_enabled{true};
  int audio_upload_num_channels{1};
  int audio_upload_sampling_frequency{48000};
  std::string accept_button{"X"};
  bool encryption_supported{true};
  int summer_time{};
  std::string time_zone;
  std::string http_user_agent{psn::cloud_stream_http_user_agent};
  SecretString gk_cloud_auth_code;
  int accessibility_marquee_speed{};
  int accessibility_large_text{};
  int accessibility_bold_text{};
  int accessibility_contrast{};
  int accessibility_tts_enable{};
  int accessibility_tts_speed{};
  int accessibility_tts_volume{};
  bool party_capability{};
  bool homesharing{};
  bool is_first_boot{};
  bool is_plus_member{true};
  int parental_level{};
  std::string yuv_coefficient;
  std::string video_encoder_profile{"hw5.0"};
  std::vector<std::string> connected_controllers{"ds4", "ds5", "xinput"};
  Input input{{"ds4", "ds5", "xinput"}};
  std::string model{"portal"};
  std::string platform{"qlite"};
  std::string player_version{"16.4.0"};
  std::uint32_t protocol_version{};
  SecretString ps3_auth_code;
  SecretString stream_server_auth_code;
  std::optional<std::string> video_codec;
  std::optional<std::string> dynamic_range;
  VideoStreamSettings video_stream_settings;
  std::string audio_channels;
  std::string audio_encoder_profile{"default"};
  AudioStreamSettings audio_stream_settings;
  std::vector<std::string> capabilities{"cloudDrivenSenkushaTest", "cronos"};
};

struct LaunchSpec::LegacyCloudStream {
  std::string entitlement_id;
  std::string np_env{"np"};
  std::string language;
  std::string cloud_endpoint{Client::cloud_endpoint};
  std::string redirect_uri{psn::legacy_cloud_stream_redirect_uri};
  std::string resolution_setting;
  int client_width{};
  int client_height{};
  std::string adaptive_stream_mode{"resize"};
  bool use_client_bw_ladder{true};
  bool audio_upload_enabled{true};
  int audio_upload_num_channels{1};
  int audio_upload_sampling_frequency{48000};
  std::string accept_button{"X"};
  bool encryption_supported{true};
  int summer_time{};
  std::string time_zone;
  std::string http_user_agent{psn::legacy_cloud_stream_http_user_agent};
  SecretString gk_cloud_auth_code;
  int accessibility_marquee_speed{};
  int accessibility_large_text{};
  int accessibility_bold_text{};
  int accessibility_contrast{};
  int accessibility_tts_enable{};
  int accessibility_tts_speed{};
  int accessibility_tts_volume{};
  bool party_capability{};
  bool homesharing{};
  bool is_first_boot{};
  bool is_plus_member{true};
  int parental_level{};
  std::string yuv_coefficient;
  std::string audio_channels{"2.1"};
  std::string audio_encoder_profile{"default"};
  std::string video_encoder_profile{"hw4.1"};
  std::vector<std::string> connected_controllers{"xinput"};
  Input input{{"xinput"}};
  std::string model{"WINDOWS"};
  std::string platform{"PC"};
  std::string player_version{"12.5.0"};
  std::uint32_t protocol_version{};
  SecretString ps3_auth_code;
  SecretString stream_server_auth_code;
  std::vector<std::string> capabilities{"cloudDrivenSenkushaTest", "kratos"};
};

}  // namespace libchiaking::psn::gk

template <>
struct glz::meta<libchiaking::psn::gk::LaunchSpec::CloudStream> {
  static constexpr auto rename_key(const auto key) -> std::string {
    if (std::string_view{key} == "player_version") return "gaikaiPlayer";
    return glz::to_camel_case(key);
  }
};

template <>
struct glz::meta<libchiaking::psn::gk::LaunchSpec::LegacyCloudStream> {
  static constexpr auto rename_key(const auto key) -> std::string {
    if (std::string_view{key} == "player_version") return "gaikaiPlayer";
    return glz::to_camel_case(key);
  }
};

namespace libchiaking::psn::gk {

auto TimeZone::formatted(long offset_minutes) -> std::string {
  const long magnitude = std::labs(offset_minutes);
  return std::format("UTC{}{:02}:{:02}", offset_minutes < 0 ? '-' : '+',
                     magnitude / minutes_per_hour,
                     magnitude % minutes_per_hour);
}

auto TimeZone::parsed_offset(std::string_view value) noexcept
    -> std::optional<long> {
  value = text::trim(value);
  if (value.empty()) return std::nullopt;
  if (value.size() >= 3 && (text::ieq(value.substr(0, 3), "utc") ||
                            text::ieq(value.substr(0, 3), "gmt")))
    value.remove_prefix(3);
  if (value.empty() || value == "Z" || value == "z") return 0L;
  if (value.front() != '+' && value.front() != '-') return std::nullopt;
  const bool negative = value.front() == '-';
  value.remove_prefix(1);
  const auto colon = value.find(':');
  const auto hours_text =
      colon == std::string_view::npos
          ? value.substr(0, value.size() <= 2 ? value.size() : value.size() - 2)
          : value.substr(0, colon);
  const auto minutes_text = colon == std::string_view::npos
                                ? value.substr(hours_text.size())
                                : value.substr(colon + 1);
  const auto digits_only = [](std::string_view part) {
    return std::ranges::all_of(part, text::ascii_digit);
  };
  if (hours_text.empty() || hours_text.size() > 2 || !digits_only(hours_text) ||
      !digits_only(minutes_text) ||
      (colon != std::string_view::npos && minutes_text.size() != 2) ||
      (colon == std::string_view::npos && !minutes_text.empty() &&
       minutes_text.size() != 2))
    return std::nullopt;
  const auto hours = text::parse_integer<unsigned>(hours_text);
  const auto minutes = minutes_text.empty()
                           ? std::optional<unsigned>{0U}
                           : text::parse_integer<unsigned>(minutes_text);
  if (!hours || !minutes || *hours > 23 || *minutes > 59) return std::nullopt;
  const auto total = static_cast<long>(*hours * minutes_per_hour + *minutes);
  return negative ? -total : total;
}

auto TimeZone::clock_offset(const platform::UtcOffsetProvider& clock) noexcept
    -> Expected<long, Unreadable> {
  const auto offset = clock.utc_offset();
  if (!offset) return unexpected(Unreadable::clock_unavailable);
  const long minutes = static_cast<long>(offset->count());
  if (std::labs(minutes) > maximum_offset_minutes)
    return unexpected(Unreadable::out_of_range);
  return minutes;
}

Language::Language(std::string_view locale) {
  locale = text::c_string(locale);
  while (!locale.empty() && text::ascii_space(locale.front()))
    locale.remove_prefix(1);
  locale = locale.substr(0, locale.find_first_of("-_"))
               .substr(0, language_length_limit);
  for (const char character : locale)
    code_.push_back(text::ascii_lower(character));
  if (code_.empty()) code_ = "en";
}

TimeZone::TimeZone(long offset_minutes, std::string text) noexcept
    : offset_minutes_(offset_minutes), text_(std::move(text)) {}

TimeZone::TimeZone(std::string_view text,
                   const platform::UtcOffsetProvider& clock)
    : TimeZone(parse(text).value_or(local(clock))) {}

auto TimeZone::text_for(const platform::UtcOffsetProvider& clock)
    -> Expected<std::string, Unreadable> {
  return clock_offset(clock).transform(formatted);
}

auto TimeZone::is_canonical(std::string_view text) noexcept -> bool {
  return text.size() == text_length && text.starts_with("UTC") &&
         (text[3] == '+' || text[3] == '-') && text[6] == ':' &&
         std::ranges::all_of(text.substr(4, 2), text::ascii_digit) &&
         std::ranges::all_of(text.substr(7, 2), text::ascii_digit) &&
         parsed_offset(text);
}

auto TimeZone::parse(std::string_view text) -> std::optional<TimeZone> {
  return parsed_offset(text).transform(from_minutes);
}

auto TimeZone::from_minutes(long offset_minutes) -> TimeZone {
  return TimeZone{offset_minutes, formatted(offset_minutes)};
}

auto TimeZone::local(const platform::UtcOffsetProvider& clock) -> TimeZone {
  return from_minutes(clock_offset(clock).value_or(0));
}

LaunchSpec::LaunchSpec(StreamingService service, std::string json) noexcept
    : service_(service), json_(std::move(json)) {}

auto LaunchSpec::cloud_stream(std::string_view entitlement_id,
                              const Language& language,
                              const TimeZone& time_zone, int resolution,
                              std::uint32_t protocol_version,
                              std::uint64_t requested_audio_channels, bool hdr,
                              Accessibility accessibility, bool high_quality,
                              std::uint64_t max_audio_channels)
    -> Expected<LaunchSpec, Invalid> {
  if (!valid_entitlement(entitlement_id))
    return unexpected(Invalid::entitlement);
  const auto layout = audio_layout(requested_audio_channels);
  if (!layout) return unexpected(layout.error());
  const auto ceiling = audio_layout(max_audio_channels);
  if (!ceiling) return unexpected(ceiling.error());
  const auto res = Resolution::nearest(resolution);
  CloudStream fields;
  describe(fields, entitlement_id, language, time_zone, res, protocol_version,
           accessibility);
  auto& video = fields.video_stream_settings;
  video.client_height = res.height();
  video.supported_max_resolution = res.height();
  video.preferred_max_resolution = res.height();
  video.hq_mode = high_quality ? 1 : 0;
  if (hdr) {
    video.supported_dynamic_range = "HDR";
    video.preferred_dynamic_range = "HDR";
    fields.video_codec = "hevc";
    fields.dynamic_range = "HDR";
  }
  fields.audio_channels = layout.value();
  fields.audio_stream_settings.preferred_number_audio_channels = layout.value();
  fields.audio_stream_settings.max_audio_channels = ceiling.value();
  return built(StreamingService::cloud_stream, fields);
}

auto LaunchSpec::legacy_cloud_stream(std::string_view entitlement_id,
                                     const Language& language,
                                     const TimeZone& time_zone, int resolution,
                                     std::uint32_t protocol_version,
                                     Accessibility accessibility)
    -> Expected<LaunchSpec, Invalid> {
  if (!valid_entitlement(entitlement_id))
    return unexpected(Invalid::entitlement);
  LegacyCloudStream fields;
  describe(fields, entitlement_id, language, time_zone,
           Resolution::nearest(resolution), protocol_version, accessibility);
  return built(StreamingService::legacy_cloud_stream, fields);
}

auto LaunchSpec::prebuilt(StreamingService service, std::string json)
    -> LaunchSpec {
  return LaunchSpec{service, std::move(json)};
}

auto LaunchSpec::valid_entitlement(std::string_view entitlement_id) noexcept
    -> bool {
  return !entitlement_id.empty() &&
         entitlement_id.size() <= entitlement_id_limit &&
         entitlement_id.find('\0') == std::string_view::npos;
}

auto LaunchSpec::audio_layout(std::uint64_t requested_audio_channels)
    -> Expected<std::string_view, Invalid> {
  switch (requested_audio_channels) {
    case 0:
    case 12:
      return std::string_view{"7.1.4"};
    case 2:
      return std::string_view{"2"};
    case 6:
      return std::string_view{"5.1"};
    case 8:
      return std::string_view{"7.1"};
    default:
      return unexpected(Invalid::audio_channels);
  }
}

auto LaunchSpec::repaired(std::string_view text) -> std::string {
  std::string repaired;
  text::utf8::append_repaired(repaired, text);
  return repaired;
}

template <class Fields>
void LaunchSpec::describe(Fields& fields, std::string_view entitlement_id,
                          const Language& language, const TimeZone& time_zone,
                          const Resolution& resolution,
                          std::uint32_t protocol_version,
                          Accessibility accessibility) {
  fields.entitlement_id = repaired(entitlement_id);
  fields.language = repaired(language.view());
  fields.resolution_setting = resolution.setting();
  fields.client_width = resolution.width();
  fields.client_height = resolution.height();
  fields.time_zone = repaired(time_zone.view());
  fields.protocol_version = protocol_version;
  fields.accessibility_contrast = accessibility.higher_contrast ? 1 : 0;
  fields.accessibility_large_text = accessibility.text_size;
  fields.accessibility_bold_text = accessibility.bold_text ? 1 : 0;
  fields.accessibility_tts_enable = accessibility.screen_reader ? 1 : 0;
  fields.accessibility_tts_speed = accessibility.screen_reader_speed;
  fields.accessibility_tts_volume = accessibility.screen_reader_volume;
}

template <class Fields>
auto LaunchSpec::written(const Fields& fields)
    -> Expected<SecretString, Invalid> {
  auto text = json::ps::write(fields);
  if (!text) return unexpected(Invalid::unexpected_shape);
  if (text.value().size() > Client::body_limit)
    return unexpected(Invalid::oversize);
  return std::move(text).value();
}

template <class Fields>
auto LaunchSpec::built(StreamingService service, const Fields& fields)
    -> Expected<LaunchSpec, Invalid> {
  return written(fields).transform([service](const SecretString& text) {
    return LaunchSpec{service, std::string{text.view()}};
  });
}

auto LaunchSpec::authorized(const psn::auth::GkCodes& codes) const
    -> Expected<SecretString, Invalid> {
  if (json_.size() > Client::body_limit ||
      codes.client.view().size() > Client::body_limit ||
      codes.server.view().size() > Client::body_limit)
    return unexpected(Invalid::oversize);
  switch (service_) {
    case StreamingService::cloud_stream:
      return authorized_as<CloudStream>(codes);
    case StreamingService::legacy_cloud_stream:
      return authorized_as<LegacyCloudStream>(codes);
  }
  return unexpected(Invalid::unexpected_shape);
}

template <class Fields>
auto LaunchSpec::authorized_as(const psn::auth::GkCodes& codes) const
    -> Expected<SecretString, Invalid> {
  static constexpr json::DocumentOptions strict = [] {
    json::DocumentOptions options{};
    options.error_on_unknown_keys = true;
    options.error_on_missing_keys = true;
    return options;
  }();
  Fields fields{};
  if (json::read_into<strict>(fields, json_))
    return unexpected(Invalid::unexpected_shape);
  fields.gk_cloud_auth_code = SecretString{codes.client.view()};
  fields.stream_server_auth_code = SecretString{codes.server.view()};
  if constexpr (std::is_same_v<Fields, LegacyCloudStream>)
    fields.ps3_auth_code = SecretString{codes.server.view()};
  else
    fields.ps3_auth_code = SecretString{};
  return written(fields);
}

}  // namespace libchiaking::psn::gk
