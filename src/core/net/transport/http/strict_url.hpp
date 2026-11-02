/// @file
/// @brief Declares StrictUrl, the checked reading of URLs the library requests
/// or
/// shows, and of the OAuth redirects PSN sign-in answers with.
#pragma once

#include "src/core/net/transport/http/percent_encoding.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/secret_string.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace libchiaking::net::http {

/// @brief A URL whose scheme, authority and host have been checked, and the
/// readers that take a secret value out of a redirect's query.
///
/// read() accepts only `scheme://authority[tail]` with no user information and
/// a
/// host that is a DNS name or a bracketed IPv6 literal; it is used to vet URLs
/// before they are requested or shown. The secret readers pull OAuth codes and
/// tokens out of redirect locations without copying them into plain strings.
/// Holds views into the text read, which must outlive it. Immutable; safe to
/// share between threads.
class StrictUrl {
 public:
  /// @brief How strictly read() judges a URL.
  enum class Strictness : std::uint8_t {
    /// @brief For a URL the library will request: the host must be a DNS
    /// name
    /// of letters, digits, `-` and `.` (no empty labels), or a bracketed
    /// IPv6
    /// literal of at least 2 characters; port 0 is refused.
    request_target,
    /// @brief For a URL only shown to the player: any host without `%`, a
    /// bracketed literal of at least 1 character, and port 0 allowed.
    shown_link,
  };

  /// @brief Why only_secret_value() found no usable value.
  enum class QueryFault : std::uint8_t {
    /// @brief A parameter name is empty, too long, or not valid
    /// percent-encoding of visible ASCII.
    undecodable,
    /// @brief The refused parameter, such as `error`, is present.
    refused,
    /// @brief The wanted parameter appears more than once or has no `=`.
    ambiguous,
    /// @brief The wanted parameter is missing, empty, too long, or its
    /// value does not decode.
    unusable,
  };

  /// @brief Why redirect_query() found no query.
  enum class RedirectFault : std::uint8_t {
    /// @brief The location does not start with the expected address, holds
    /// a fragment or is too long.
    elsewhere,
    /// @brief What follows the address does not start a query.
    no_query,
  };

  /// @brief Reads and checks a URL.
  ///
  /// @param url The URL.
  /// @param strictness How strictly to judge it.
  /// @return The checked URL; nothing when there is no `://` or no scheme
  ///     before it, the authority is empty or holds `@`, the host is a bare
  ///     IPv6 literal, an invalid bracketed literal, empty or holds `%`, the
  ///     host fails the DNS name rule under request_target, or the port is
  ///     not
  ///     a number up to 65535 (or is 0 under request_target).
  [[nodiscard]] static auto read(std::string_view url,
                                 Strictness strictness) noexcept
      -> std::optional<StrictUrl>;

  /// @brief Takes the one value of a query parameter, refusing a query that
  /// also carries another named parameter.
  ///
  /// Every parameter name is percent-decoded and checked; `+` is not accepted
  /// as a space anywhere.
  ///
  /// @param query The query without its `?`, parameters separated by `&`.
  /// @param name The parameter wanted, such as `code`.
  /// @param refused_name A parameter whose presence refuses the query, such
  ///     as `error`.
  /// @param limits Size limits on each name and on the value.
  /// @return The decoded value.
  /// @retval QueryFault::undecodable A name does not decode or is too long.
  /// @retval QueryFault::refused `refused_name` is present.
  /// @retval QueryFault::ambiguous `name` appears twice or without `=`.
  /// @retval QueryFault::unusable `name` is missing or its value does not
  ///     decode within the limits.
  [[nodiscard]] static auto only_secret_value(
      std::string_view query, std::string_view name,
      std::string_view refused_name, PercentEncoding::DecodeLimits limits)
      -> Expected<SecretString, QueryFault>;

  /// @brief Takes the query of a redirect that leads to an expected address.
  ///
  /// The query must follow the address at once. An address with no path,
  /// such as `gaikai://local`, also matches with the path `/`.
  ///
  /// @param location The redirect location.
  /// @param address The redirect URI the request named.
  /// @param max_bytes The longest location accepted.
  /// @return The query without its `?`, a view into @p location.
  /// @retval RedirectFault::elsewhere @p location does not start with
  ///     @p address, holds `#`, or is longer than @p max_bytes.
  /// @retval RedirectFault::no_query No `?` follows the address.
  [[nodiscard]] static auto redirect_query(std::string_view location,
                                           std::string_view address,
                                           std::size_t max_bytes) noexcept
      -> Expected<std::string_view, RedirectFault>;

  /// @brief Takes the first value of a parameter from a redirect location,
  /// undecoded.
  ///
  /// The parameter may follow `?`, `&` or `#`, or start the text. The value
  /// ends at `&`, `#`, CR or LF and is cut to `capacity` less one bytes. The
  /// location is read up to its first NUL.
  ///
  /// @param location The redirect location.
  /// @param name The parameter wanted.
  /// @param capacity The size of the buffer the value must fit, including a
  ///     terminator.
  /// @return The value as written; empty when `name` is empty, `capacity` is
  ///     below 2, or the parameter is absent.
  static auto first_secret_value(std::string_view location,
                                 std::string_view name, std::size_t capacity)
      -> SecretString;

  /// @brief The scheme, before `://`.
  ///
  /// @return A view into the URL read, such as `https`.
  auto scheme() const noexcept -> std::string_view { return scheme_; }
  /// @brief The authority: host and optional port, as written.
  ///
  /// @return A view into the URL read.
  auto authority() const noexcept -> std::string_view { return authority_; }
  /// @brief The host as written, brackets kept for an IPv6 literal.
  ///
  /// @return A view into the URL read.
  auto host() const noexcept -> std::string_view { return host_; }
  /// @brief The port written in the authority.
  ///
  /// @return The port; nothing when none was written.
  auto port() const noexcept -> std::optional<unsigned> { return port_; }
  /// @brief Everything after the authority: path, query and fragment.
  ///
  /// @return A view into the URL read; may be empty.
  auto tail() const noexcept -> std::string_view { return tail_; }

 private:
  /// @brief The text between scheme and authority.
  static constexpr std::string_view scheme_separator = "://";
  /// @brief Characters that end the authority.
  static constexpr std::string_view authority_end = "/?#";
  /// @brief Characters that end a value read by first_secret_value().
  static constexpr std::string_view verbatim_value_end = "&#\r\n";
  /// @brief Shortest bracketed IPv6 literal under request_target, such as
  /// `::`.
  static constexpr std::size_t request_address_minimum = 2;
  /// @brief Shortest bracketed IPv6 literal under shown_link.
  static constexpr std::size_t shown_address_minimum = 1;

  /// @brief An empty URL, filled in by read().
  StrictUrl() = default;

  /// @brief Whether bracketed text looks like an IPv6 literal.
  ///
  /// @param address The text inside the brackets.
  /// @return True when it holds a `:` and only hex digits, `:` and `.`.
  static auto address_literal(std::string_view address) noexcept -> bool;
  /// @brief Whether a host is a plausible DNS name.
  ///
  /// @param host The host.
  /// @return True when it is letters, digits, `-` and `.`, starts and ends
  ///     with
  ///     a letter or digit, and has no empty label.
  static auto dns_name(std::string_view host) noexcept -> bool;
  /// @brief Reads a port.
  ///
  /// @param digits The text after the colon.
  /// @param strictness Whether port 0 is refused.
  /// @return The port; nothing when it is not a decimal number up to 65535,
  ///     or
  ///     is 0 under request_target.
  static auto port_number(std::string_view digits,
                          Strictness strictness) noexcept
      -> std::optional<unsigned>;
  /// @brief Whether a character may precede a parameter name in a location.
  ///
  /// @param value The character.
  /// @return True for `?`, `&` and `#`.
  static auto component_start(char value) noexcept -> bool;

  /// @brief The scheme.
  std::string_view scheme_;
  /// @brief The authority.
  std::string_view authority_;
  /// @brief The host as written.
  std::string_view host_;
  /// @brief The port, if written.
  std::optional<unsigned> port_;
  /// @brief The path, query and fragment.
  std::string_view tail_;
};

}  // namespace libchiaking::net::http
