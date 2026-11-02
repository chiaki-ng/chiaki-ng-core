/// @file
/// @brief Declares SearchDatagram, how a datagram answering a network search
/// is screened, parsed and judged into a SearchReply.
///
/// A reply is an HTTP-like text datagram (`HTTP/1.1 200 Ok` for a console
/// that is on, `620` for one in rest mode) followed by `key:value` headers.
/// Anything on the network can send one, so a datagram is screened and
/// bounded before any field is believed.
#pragma once

#include "libchiaking/detail/consoles/console_name.hpp"
#include "libchiaking/detail/consoles/hardware_id.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/net/types.hpp"
#include "src/core/consoles/model/search/search_reply.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace libchiaking::consoles {

/// @brief The search reply's wire format: its limits, its screening and its
/// parsing. Holds no state; every member is static.
class SearchDatagram {
 public:
  /// @brief The power state a reply's status line gave.
  using Status = SearchReply::Status;

  /// @brief Why a datagram was not taken as a console's reply.
  enum class Refusal : std::uint8_t {
    /// @brief Longer than reply_limit bytes.
    oversized,
    /// @brief It came from an IPv6 address; only IPv4 replies are trusted.
    not_ipv4,
    /// @brief It holds a zero byte.
    embedded_nul,
    /// @brief A header line has no colon.
    line_without_colon,
    /// @brief One of the headers that are read appears twice (keys compared
    /// without regard to ASCII case).
    duplicate_header,
    /// @brief The `host-request-port` header is not a port number from 1 to
    /// 65535.
    invalid_request_port,
    /// @brief A header that is read is longer than its limit.
    field_too_long,
    /// @brief The text did not parse as a reply with at least one header.
    malformed,
    /// @brief It parsed, but its type is not PS4 or PS5, or its id, name or
    /// system version is unusable.
    not_a_console,
    /// @brief It came from 0.0.0.0/8, a multicast or a reserved address.
    unusable_source,
  };

  /// @brief The raw fields of a parsed reply, before they are judged.
  struct Fields {
    /// @brief The power state from the status line.
    Status status{Status::unknown};
    /// @brief The address the datagram came from.
    platform::IpAddress address{platform::IpAddress::any_ipv4()};
    /// @brief The `host-request-port` header; zero when absent or not a
    /// number.
    std::uint16_t request_port{};
    /// @brief The `system-version` header, as sent.
    std::string system_version;
    /// @brief The `device-discovery-protocol-version` header, as sent.
    std::string protocol_version;
    /// @brief The `host-name` header, as sent.
    std::string name;
    /// @brief The `host-type` header: `PS4` or `PS5` for a console.
    std::string type;
    /// @brief The `host-id` header: the console's MAC address in
    /// hexadecimal.
    std::string id;
    /// @brief The `running-app-titleid` header; empty when nothing is
    /// running.
    std::string running_app_title_id;
    /// @brief The `running-app-name` header.
    std::string running_app_name;

    /// @brief Compares every field.
    friend auto operator==(const Fields&, const Fields&) noexcept
        -> bool = default;
  };

  /// @brief The discovery protocol version a PS4 is searched with.
  static constexpr std::string_view ps4_protocol_version = "00020020";
  /// @brief The discovery protocol version a PS5 is searched with.
  static constexpr std::string_view ps5_protocol_version = "00030010";
  /// @brief The `host-type` of a PS4.
  static constexpr std::string_view ps4_type = "PS4";
  /// @brief The `host-type` of a PS5.
  static constexpr std::string_view ps5_type = "PS5";
  /// @brief The status code of a console in rest mode.
  static constexpr long standby_status = 620;
  /// @brief The line terminator the reply is framed with before parsing.
  static constexpr char reply_terminator = '\n';

  /// @brief Longest datagram accepted, in bytes.
  static constexpr std::size_t reply_limit = 4096;
  /// @brief Size of the receive buffer, in bytes: one more than reply_limit,
  /// so an oversized datagram is recognised.
  static constexpr std::size_t receive_limit = reply_limit + 1;
  /// @brief Longest `host-id` value screened, in bytes.
  static constexpr std::size_t id_limit = 63;
  /// @brief Longest `host-name` value screened, in bytes.
  static constexpr std::size_t name_limit = 127;
  /// @brief Longest `host-type` value screened, in bytes.
  static constexpr std::size_t type_limit = 31;
  /// @brief Longest `system-version` value screened, in bytes.
  static constexpr std::size_t system_version_limit = 31;
  /// @brief Longest `device-discovery-protocol-version` value screened, in
  /// bytes.
  static constexpr std::size_t protocol_version_limit = 31;
  /// @brief Longest `running-app-titleid` value screened, in bytes.
  static constexpr std::size_t running_app_title_id_limit = 31;
  /// @brief Longest `running-app-name` value screened, in bytes.
  static constexpr std::size_t running_app_name_limit = 127;
  /// @brief Most digits a request port may have.
  static constexpr std::size_t request_port_digits = 5;

  /// @brief Exact length of a usable `host-id`, in hexadecimal digits.
  static constexpr std::size_t id_length = HardwareId::digit_count;
  /// @brief Longest name a console may report, in bytes.
  static constexpr std::size_t console_name_limit = ConsoleName::limit;
  /// @brief Longest system version a console may report, in bytes.
  static constexpr std::size_t console_system_version_limit =
      SearchReply::system_version_limit;

  /// @brief Whether a reply may come from an IPv4 address.
  ///
  /// @param source The sender, as an IPv4 address.
  /// @return False for 0.0.0.0/8 and for 224.0.0.0 and above; true otherwise.
  static auto usable_source(platform::IpAddress source) noexcept -> bool;
  /// @brief Screens a datagram's size, sender and header lines before it is
  /// parsed.
  ///
  /// Only the headers the reply is read for are checked for repeats and
  /// length; others are ignored.
  ///
  /// @param datagram The datagram as received.
  /// @param source The sender.
  /// @return The first reason to refuse it, or nothing when it may be parsed.
  static auto refusal(std::span<const std::byte> datagram,
                      platform::IpAddress source) noexcept
      -> std::optional<Refusal>;
  /// @brief Parses a screened datagram into its raw fields.
  ///
  /// The text is cut at the first zero byte and at its last line terminator,
  /// then parsed as an HTTP response. Header keys are matched exactly; the
  /// first of a repeated header wins.
  ///
  /// @param datagram The datagram.
  /// @param source The sender, kept as the address.
  /// @return The fields, or nothing when the text is not a response with at
  ///     least one header.
  [[nodiscard]] static auto parse(std::span<const std::byte> datagram,
                                  platform::IpAddress source)
      -> std::optional<Fields>;
  /// @brief Judges parsed fields and makes the reply they describe.
  ///
  /// The firmware is read from the system version for the stated generation;
  /// the name becomes a ConsoleName::Source::discovered name when usable; a
  /// running title is kept when it has an id and both texts are printable.
  ///
  /// @param fields The fields.
  /// @return The reply, or nothing when the type is not PS4 or PS5, the id is
  ///     not a hardware id, or the name or system version is too long or not
  ///     printable.
  [[nodiscard]] static auto from(const Fields& fields)
      -> std::optional<SearchReply>;
  /// @brief Screens, parses and judges a datagram in one step.
  ///
  /// @param datagram The datagram as received.
  /// @param source The sender.
  /// @return The reply.
  /// @retval Refusal Any reason of refusal(), Refusal::malformed when parse()
  ///     fails, or Refusal::not_a_console when from() refuses.
  [[nodiscard]] static auto admit(std::span<const std::byte> datagram,
                                  platform::IpAddress source)
      -> Expected<SearchReply, Refusal>;

 private:
  /// @brief A header the reply is read for, with its length limit.
  struct LimitedHeader {
    /// @brief The header's key, compared without regard to ASCII case.
    std::string_view key;
    /// @brief Its longest value, in bytes; for the request port, in digits.
    std::size_t limit;
  };

  /// @brief The key of the request port header.
  static constexpr std::string_view request_port_key = "host-request-port";
  /// @brief Every header the reply is read for, with its limit.
  static constexpr std::array limited_headers{
      LimitedHeader{"host-id", id_limit},
      LimitedHeader{"host-name", name_limit},
      LimitedHeader{"host-type", type_limit},
      LimitedHeader{"system-version", system_version_limit},
      LimitedHeader{"device-discovery-protocol-version",
                    protocol_version_limit},
      LimitedHeader{"running-app-titleid", running_app_title_id_limit},
      LimitedHeader{"running-app-name", running_app_name_limit},
      LimitedHeader{request_port_key, request_port_digits},
  };

  /// @brief Whether a request port header holds a port from 1 to 65535.
  ///
  /// @param value The header value.
  /// @return True for a valid port, surrounding whitespace allowed.
  static auto valid_request_port(std::string_view value) noexcept -> bool;
  /// @brief Strips leading spaces and tabs from a header value.
  ///
  /// @param text The text after the colon.
  /// @return The value.
  static auto header_value(std::string_view text) noexcept -> std::string_view;
};

}  // namespace libchiaking::consoles
