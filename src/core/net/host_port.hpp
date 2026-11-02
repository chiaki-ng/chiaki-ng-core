/// @file
/// @brief Declares HostPort, the one reader of `host[:port]` text used for URL
/// authorities, STUN server lists and peer-link STUN targets.
#pragma once

#include "src/core/utils/text/integer.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace libchiaking::net {

/// @brief A host and optional port split out of text written as `host`,
/// `host:port`, `[ipv6]`, `[ipv6]:port` or a bare IPv6 literal.
///
/// Only the punctuation is read: the host is not resolved or validated and
/// the port text is kept as written until port() parses it. Text with two or
/// more colons and no brackets is taken as a bare IPv6 literal without a port.
/// Every view points into the text given to split(), which must outlive the
/// object. Immutable and safe to share between threads.
class HostPort {
 public:
  /// @brief Splits `host[:port]` text.
  ///
  /// @param text The text to split; views into it are kept.
  /// @return The host and port; nothing when `text` is empty, when a `[`
  ///     has no closing `]` or encloses nothing, or when anything other than
  ///     `:port` follows the `]`.
  [[nodiscard]] static constexpr auto split(std::string_view text) noexcept
      -> std::optional<HostPort> {
    if (text.empty()) return std::nullopt;
    if (text.front() == '[') return split_bracketed(text);
    const auto colon = text.find(':');
    if (colon == std::string_view::npos)
      return HostPort{text, text, std::nullopt, Shape::name_only};
    if (text.find(':', colon + 1) != std::string_view::npos)
      return HostPort{text, text, std::nullopt, Shape::bare_ipv6};
    return HostPort{text.substr(0, colon), text.substr(0, colon),
                    text.substr(colon + 1), Shape::name_and_port};
  }

  /// @brief The host without brackets, such as `example.com` or `::1`.
  ///
  /// @return A view into the split text.
  constexpr auto host() const noexcept -> std::string_view { return host_; }

  /// @brief The host as it was written, keeping the brackets of a bracketed
  /// IPv6 literal, such as `[::1]`; suitable for rebuilding a URL authority.
  ///
  /// @return A view into the split text.
  constexpr auto written_host() const noexcept -> std::string_view {
    return written_host_;
  }

  /// @brief The text after the port colon, unparsed.
  ///
  /// @return The port text, possibly empty or not a number; nothing when no
  ///     port was written.
  constexpr auto port_text() const noexcept -> std::optional<std::string_view> {
    return port_;
  }

  /// @brief The port as a number.
  ///
  /// @return The port; nothing when no port was written or its text is not
  ///     a decimal integer from 0 to 65535.
  auto port() const noexcept -> std::optional<std::uint16_t> {
    if (!port_) return std::nullopt;
    return text::parse_integer<std::uint16_t>(*port_);
  }

  /// @brief Whether the host was written in square brackets.
  ///
  /// @return True for `[host]` and `[host]:port`.
  constexpr auto bracketed() const noexcept -> bool {
    return shape_ == Shape::bracketed;
  }

  /// @brief Whether the text was taken as an IPv6 literal without brackets,
  /// because it held more than one colon.
  ///
  /// @return True for bare IPv6 text; such text never carries a port.
  constexpr auto bare_ipv6() const noexcept -> bool {
    return shape_ == Shape::bare_ipv6;
  }

  /// @brief Whether the host is written as an IPv6 literal, bracketed or
  /// bare.
  ///
  /// A bracketed host is assumed to be IPv6; its content is not checked.
  ///
  /// @return True when bracketed() or bare_ipv6().
  constexpr auto ipv6_literal() const noexcept -> bool {
    return bracketed() || bare_ipv6();
  }

 private:
  /// @brief How the host and port were written.
  enum class Shape : std::uint8_t {
    /// @brief A host with no colon, such as `example.com`.
    name_only,
    /// @brief A host and port separated by the only colon, such as
    /// `example.com:3478`.
    name_and_port,
    /// @brief A host in square brackets, with or without `:port` after.
    bracketed,
    /// @brief Unbracketed text with two or more colons, taken as an IPv6
    /// literal with no port.
    bare_ipv6
  };

  /// @brief Holds the pieces split() found.
  ///
  /// @param host The host without brackets.
  /// @param written_host The host as written, with any brackets.
  /// @param port The port text, or nothing when none was written.
  /// @param shape How the text was written.
  constexpr HostPort(std::string_view host, std::string_view written_host,
                     std::optional<std::string_view> port, Shape shape) noexcept
      : host_(host), written_host_(written_host), port_(port), shape_(shape) {}

  /// @brief Splits text that starts with `[`.
  ///
  /// @param text The text, starting with `[`.
  /// @return The bracketed host and optional port; nothing when there is no
  ///     `]`, the brackets are empty, or the `]` is followed by anything but
  ///     end of text or `:`.
  [[nodiscard]] static constexpr auto split_bracketed(
      std::string_view text) noexcept -> std::optional<HostPort> {
    const auto close = text.find(']');
    if (close == std::string_view::npos || close == 1) return std::nullopt;
    const auto host = text.substr(1, close - 1);
    const auto after = text.substr(close + 1);
    if (after.empty())
      return HostPort{host, text.substr(0, close + 1), std::nullopt,
                      Shape::bracketed};
    if (after.front() != ':') return std::nullopt;
    return HostPort{host, text.substr(0, close + 1), after.substr(1),
                    Shape::bracketed};
  }

  /// @brief The host without brackets.
  std::string_view host_;
  /// @brief The host as written, with any brackets.
  std::string_view written_host_;
  /// @brief The text after the port colon; nothing when no port was written.
  std::optional<std::string_view> port_;
  /// @brief How the text was written.
  Shape shape_;
};

}  // namespace libchiaking::net
