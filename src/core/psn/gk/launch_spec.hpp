/// @file
/// @brief Declares LaunchSpec, the `requestGameSpecification` a GK cloud
/// session is started with, and the TimeZone and Resolution it names.
///
/// The specification describes the client as `qlite` (PS5, HEVC, up to 7.1.4
/// audio) for PS5 cloud streaming, or as `PC` for LegacyCloudStream. It is
/// built without authorisation codes for start; authorized() fills the codes in
/// for authorize.
#pragma once

#include <libchiaking/detail/secret_string.hpp>
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/runtime/utc_offset.hpp"
#include "src/core/net/protocol/takion/versions.hpp"
#include "src/core/psn/auth/authenticator.hpp"
#include "src/core/psn/gk/datacenter_ping.hpp"
#include "src/core/psn/gk/language.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace libchiaking::psn::gk {

/// @brief The UTC offset a launch specification states, written `UTC+hh:mm`.
/// An immutable value.
class TimeZone {
 public:
  /// @brief Why the host's offset could not be read.
  enum class Unreadable : std::uint8_t {
    /// @brief The host gave no offset.
    clock_unavailable,
    /// @brief The offset is 24 hours or more.
    out_of_range
  };

  /// @brief The length of a canonical offset text, `UTC+hh:mm`.
  static constexpr std::size_t text_length = 9;

  /// @brief Reads an offset, falling back to the host's.
  ///
  /// @param text An offset in any form parse() reads.
  /// @param clock The host's UTC offset, used when `text` does not parse; UTC
  ///     when that cannot be read either.
  TimeZone(std::string_view text, const platform::UtcOffsetProvider& clock);

  /// @brief Writes the host's offset.
  ///
  /// @param clock The host's UTC offset.
  /// @return `UTC+hh:mm` or `UTC-hh:mm`.
  /// @retval Unreadable::clock_unavailable The host gave no offset.
  /// @retval Unreadable::out_of_range It is 24 hours or more.
  [[nodiscard]] static auto text_for(const platform::UtcOffsetProvider& clock)
      -> Expected<std::string, Unreadable>;
  /// @brief Tells whether a text is already a canonical offset.
  ///
  /// @param text The text.
  /// @return True for `UTC±hh:mm` with hours up to 23 and minutes up to 59.
  static auto is_canonical(std::string_view text) noexcept -> bool;

  /// @brief Reads an offset.
  ///
  /// Accepts an optional `UTC` or `GMT` prefix in any case, then `Z`, nothing
  /// (UTC), or a sign with `h`, `hh`, `hhmm`, `hmm` or `hh:mm`; blanks around
  /// it are ignored.
  ///
  /// @param text The text.
  /// @return The offset; nothing for any other text, hours over 23 or minutes
  ///     over 59.
  [[nodiscard]] static auto parse(std::string_view text)
      -> std::optional<TimeZone>;
  /// @brief Makes an offset.
  ///
  /// @param offset_minutes Minutes east of UTC.
  /// @return The offset.
  static auto from_minutes(long offset_minutes) -> TimeZone;
  /// @brief Reads the host's offset.
  ///
  /// @param clock The host's UTC offset.
  /// @return The offset; UTC when it cannot be read or is out of range.
  static auto local(const platform::UtcOffsetProvider& clock) -> TimeZone;

  /// @brief Reads the offset.
  ///
  /// @return Minutes east of UTC.
  auto offset_minutes() const noexcept -> long { return offset_minutes_; }
  /// @brief Reads the offset as text.
  ///
  /// @return `UTC±hh:mm`.
  auto view() const noexcept -> std::string_view { return text_; }

 private:
  /// @brief Minutes in an hour.
  static constexpr long minutes_per_hour = 60;
  /// @brief The largest offset accepted from the host, in minutes.
  static constexpr long maximum_offset_minutes = 24 * minutes_per_hour - 1;

  /// @brief Writes an offset.
  ///
  /// @param offset_minutes Minutes east of UTC.
  /// @return `UTC±hh:mm`.
  static auto formatted(long offset_minutes) -> std::string;
  /// @brief Reads an offset's minutes.
  ///
  /// @param value The text.
  /// @return Minutes east of UTC, as parse() describes.
  static auto parsed_offset(std::string_view value) noexcept
      -> std::optional<long>;
  /// @brief Reads the host's offset in minutes.
  ///
  /// @param clock The host's UTC offset.
  /// @return The minutes.
  /// @retval Unreadable The failures of text_for().
  static auto clock_offset(const platform::UtcOffsetProvider& clock) noexcept
      -> Expected<long, Unreadable>;

  /// @brief Wraps an offset and its text.
  ///
  /// @param offset_minutes Minutes east of UTC.
  /// @param text Its text.
  TimeZone(long offset_minutes, std::string text) noexcept;

  /// @brief Minutes east of UTC.
  long offset_minutes_;
  /// @brief The offset as `UTC±hh:mm`.
  std::string text_;
};

/// @brief A stream resolution the launch specification can ask for: 720p,
/// 1080p, 1440p or 2160p. An immutable value.
class Resolution {
 public:
  /// @brief Picks the resolution for a number of lines.
  ///
  /// @param lines The lines wanted.
  /// @return 720p, 1440p or 2160p for exactly those; 1080p for anything else.
  static constexpr auto nearest(int lines) noexcept -> Resolution {
    switch (lines) {
      case 720:
        return {"720", 1280, 720};
      case 1440:
        return {"1440", 2560, 1440};
      case 2160:
        return {"2160", 3840, 2160};
      default:
        return {"1080", 1920, 1080};
    }
  }

  /// @brief Reads the `resolutionSetting`.
  ///
  /// @return The lines as text, such as `1080`.
  constexpr auto setting() const noexcept -> std::string_view {
    return setting_;
  }
  /// @brief Reads the width.
  ///
  /// @return Pixels.
  constexpr auto width() const noexcept -> int { return width_; }
  /// @brief Reads the height.
  ///
  /// @return Pixels.
  constexpr auto height() const noexcept -> int { return height_; }

 private:
  /// @brief Makes a resolution.
  ///
  /// @param setting The lines as text.
  /// @param width Pixels.
  /// @param height Pixels.
  constexpr Resolution(std::string_view setting, int width, int height) noexcept
      : setting_(setting), width_(width), height_(height) {}

  /// @brief The lines as text.
  std::string_view setting_;
  /// @brief The width in pixels.
  int width_;
  /// @brief The height in pixels.
  int height_;
};

/// @brief The accessibility settings a launch asks the cloud console to use.
struct Accessibility {
  /// @brief Whether the cloud console reads its screen aloud.
  bool screen_reader{};
  /// @brief Speech speed from 0 to 8.
  int screen_reader_speed{3};
  /// @brief Speech volume from 0 to 24.
  int screen_reader_volume{12};
  /// @brief Cloud text size from -1 (small) to 2 (very large); 0 is standard.
  int text_size{};
  /// @brief Whether bold text is enabled.
  bool bold_text{};
  /// @brief Whether the cloud console raises its contrast.
  bool higher_contrast{};
};

/// @brief A launch specification as JSON text, with the service it is for.
/// An immutable value; it holds no authorisation codes.
class LaunchSpec {
 public:
  /// @brief Why a specification could not be built.
  enum class Invalid : std::uint8_t {
    /// @brief The entitlement id is empty, longer than 4096 bytes or holds
    /// a NUL.
    entitlement,
    /// @brief The specification or a code exceeds the GK body limit.
    oversize,
    /// @brief The audio channel count is not 0, 2, 6, 8 or 12.
    audio_channels,
    /// @brief The specification could not be written, or a prebuilt one is
    /// not exactly the service's shape.
    unexpected_shape,
  };

  /// @brief Builds a PS5 cloud streaming specification.
  ///
  /// @param entitlement_id The entitlement to stream.
  /// @param language The language.
  /// @param time_zone The UTC offset.
  /// @param resolution The lines wanted, rounded by Resolution::nearest().
  /// @param protocol_version The Takion version offered.
  /// @param requested_audio_channels 2, 6, 8 or 12 channels, or 0 for the
  ///     default 7.1.4.
  /// @param hdr Whether HDR (HEVC) is asked for.
  /// @param accessibility The screen reader and contrast to ask for.
  /// @param high_quality Whether to favour picture quality over steady
  /// performance.
  /// @param max_audio_channels 2, 6, 8 or 12 channels to cap the layout the
  ///     service may send, or 0 for the default 7.1.4.
  /// @return The specification.
  /// @retval Invalid::entitlement The entitlement id is not acceptable.
  /// @retval Invalid::audio_channels A channel count is not supported.
  /// @retval Invalid The failures of written().
  [[nodiscard]] static auto cloud_stream(
      std::string_view entitlement_id, const Language& language,
      const TimeZone& time_zone, int resolution,
      std::uint32_t protocol_version =
          net::protocol::takion::takion_version_cloud_stream,
      std::uint64_t requested_audio_channels = 0, bool hdr = false,
      Accessibility accessibility = {}, bool high_quality = true,
      std::uint64_t max_audio_channels = 0)
      -> Expected<LaunchSpec, Invalid>;
  /// @brief Builds a LegacyCloudStream specification.
  ///
  /// @param entitlement_id The entitlement to stream.
  /// @param language The language.
  /// @param time_zone The UTC offset.
  /// @param resolution The lines wanted, rounded by Resolution::nearest().
  /// @param protocol_version The Takion version offered.
  /// @param accessibility The screen reader and contrast to ask for.
  /// @return The specification.
  /// @retval Invalid::entitlement The entitlement id is not acceptable.
  /// @retval Invalid The failures of written().
  [[nodiscard]] static auto legacy_cloud_stream(
      std::string_view entitlement_id, const Language& language,
      const TimeZone& time_zone, int resolution,
      std::uint32_t protocol_version =
          net::protocol::takion::takion_version_legacy_cloud_stream,
      Accessibility accessibility = {}) -> Expected<LaunchSpec, Invalid>;
  /// @brief Wraps a specification built elsewhere, unchecked.
  ///
  /// @param service The service it is for.
  /// @param json The specification.
  /// @return The specification; authorized() checks its shape.
  static auto prebuilt(StreamingService service, std::string json)
      -> LaunchSpec;

  /// @brief Reads the service.
  ///
  /// @return The service.
  auto service() const noexcept -> StreamingService { return service_; }
  /// @brief Reads the specification.
  ///
  /// @return The JSON text.
  auto json() const noexcept -> std::string_view { return json_; }
  /// @brief Fills the authorisation codes in, for the authorize request.
  ///
  /// The client code becomes `gkCloudAuthCode`, the server code
  /// `streamServerAuthCode` and, on LegacyCloudStream, `ps3AuthCode` as well.
  ///
  /// @param codes The GK codes.
  /// @return The specification with the codes; secret.
  /// @retval Invalid::oversize The specification or a code is too large.
  /// @retval Invalid::unexpected_shape The specification has unknown or
  ///     missing keys for its service.
  [[nodiscard]] auto authorized(const psn::auth::GkCodes& codes) const
      -> Expected<SecretString, Invalid>;

 private:
  /// @brief The longest entitlement id, in bytes.
  static constexpr std::size_t entitlement_id_limit = 4096;
  /// @brief The `input` controllers.
  struct Input;
  /// @brief The PS5 `videoStreamSettings`.
  struct VideoStreamSettings;
  /// @brief The PS5 `audioStreamSettings`.
  struct AudioStreamSettings;
  /// @brief Every field of a PS5 cloud streaming specification.
  struct CloudStream;
  /// @brief Every field of a LegacyCloudStream specification.
  struct LegacyCloudStream;

  /// @brief Checks an entitlement id.
  ///
  /// @param entitlement_id The id.
  /// @return True when it is 1 to 4096 bytes with no NUL.
  static auto valid_entitlement(std::string_view entitlement_id) noexcept
      -> bool;
  /// @brief Names an audio layout.
  ///
  /// @param requested_audio_channels The channel count.
  /// @return `7.1.4` for 0 or 12, `2`, `5.1` for 6, `7.1` for 8.
  /// @retval Invalid::audio_channels Any other count.
  static auto audio_layout(std::uint64_t requested_audio_channels)
      -> Expected<std::string_view, Invalid>;
  /// @brief Replaces invalid UTF-8.
  ///
  /// @param text The text.
  /// @return Valid UTF-8.
  static auto repaired(std::string_view text) -> std::string;
  /// @brief Fills the fields both services share.
  ///
  /// @tparam Fields CloudStream or LegacyCloudStream.
  /// @param fields The fields.
  /// @param entitlement_id The entitlement.
  /// @param language The language.
  /// @param time_zone The UTC offset.
  /// @param resolution The resolution.
  /// @param protocol_version The Takion version.
  /// @param accessibility The screen reader and contrast to ask for.
  template <class Fields>
  static void describe(Fields& fields, std::string_view entitlement_id,
                       const Language& language, const TimeZone& time_zone,
                       const Resolution& resolution,
                       std::uint32_t protocol_version,
                       Accessibility accessibility);
  /// @brief Writes the fields.
  ///
  /// @tparam Fields CloudStream or LegacyCloudStream.
  /// @param fields The fields.
  /// @return The JSON; secret when codes are filled in.
  /// @retval Invalid::unexpected_shape It could not be written.
  /// @retval Invalid::oversize It exceeds the GK body limit.
  template <class Fields>
  [[nodiscard]] static auto written(const Fields& fields)
      -> Expected<SecretString, Invalid>;
  /// @brief Writes the fields as a specification.
  ///
  /// @tparam Fields CloudStream or LegacyCloudStream.
  /// @param service The service.
  /// @param fields The fields.
  /// @return The specification.
  /// @retval Invalid The failures of written().
  template <class Fields>
  [[nodiscard]] static auto built(StreamingService service,
                                  const Fields& fields)
      -> Expected<LaunchSpec, Invalid>;
  /// @brief Reads the specification strictly as one service's fields and
  /// fills the codes in.
  ///
  /// @tparam Fields CloudStream or LegacyCloudStream.
  /// @param codes The codes.
  /// @return The specification with the codes.
  /// @retval Invalid::unexpected_shape The specification is not exactly that
  ///     shape.
  /// @retval Invalid The failures of written().
  template <class Fields>
  [[nodiscard]] auto authorized_as(const psn::auth::GkCodes& codes) const
      -> Expected<SecretString, Invalid>;

  /// @brief Wraps a specification.
  ///
  /// @param service The service.
  /// @param json The JSON.
  LaunchSpec(StreamingService service, std::string json) noexcept;

  /// @brief The service.
  StreamingService service_;
  /// @brief The JSON text, without codes.
  std::string json_;
};

}  // namespace libchiaking::psn::gk
