/// @file
/// @brief Declares the address value types shared by every networking port:
/// IpAddress, SocketEndpoint and their text conversions.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/net/error.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace libchiaking::platform {

/// @brief Address family of an IpAddress.
enum class IpFamily : std::uint8_t {
  /// @brief A 4-byte IPv4 address.
  ipv4,
  /// @brief A 16-byte IPv6 address, optionally with a scope id.
  ipv6,
};

/// @brief An IPv4 or IPv6 address held by value, with no operating-system
/// address type involved.
///
/// Bytes are kept in network order, most significant first. An IPv6 address can
/// also carry a scope id, which names the interface a link-local address
/// belongs to. A default-constructed IpAddress is the IPv4 unspecified address
/// 0.0.0.0.
///
/// Providers receive and return this type instead of `sockaddr`, so a host can
/// implement the networking ports on any socket API.
///
/// @see SocketEndpoint, parse_ip_address, to_string
class IpAddress {
 public:
  /// @brief Builds an IPv4 address from its four octets.
  ///
  /// @param bytes The octets in the order they are written, so 192.168.1.20
  ///     is {192, 168, 1, 20}.
  /// @return The IPv4 address.
  static constexpr IpAddress ipv4(std::array<std::uint8_t, 4> bytes) noexcept {
    IpAddress address;
    address.family_ = IpFamily::ipv4;
    for (std::size_t i = 0; i < 4; ++i) address.bytes_[i] = bytes[i];
    return address;
  }

  /// @brief Builds an IPv6 address from its sixteen bytes.
  ///
  /// @param bytes The address in network order.
  /// @return The IPv6 address with no scope id.
  static constexpr IpAddress ipv6(std::array<std::uint8_t, 16> bytes) noexcept {
    IpAddress address;
    address.family_ = IpFamily::ipv6;
    address.bytes_ = bytes;
    return address;
  }

  /// @brief Builds an address from its bytes in network order.
  ///
  /// @param bytes Four bytes for an IPv4 address or sixteen for an IPv6
  ///     address. The result carries no scope id.
  /// @return The address, or std::nullopt when @p bytes is neither four nor
  ///     sixteen bytes long.
  static constexpr auto from_bytes(std::span<const std::byte> bytes) noexcept
      -> std::optional<IpAddress> {
    if (bytes.size() != 4 && bytes.size() != 16) return std::nullopt;
    IpAddress address;
    address.family_ = bytes.size() == 4 ? IpFamily::ipv4 : IpFamily::ipv6;
    for (std::size_t i = 0; i < bytes.size(); ++i)
      address.bytes_[i] = std::to_integer<std::uint8_t>(bytes[i]);
    return address;
  }

  /// @brief Returns 0.0.0.0, the IPv4 wildcard used to bind on every
  /// interface.
  ///
  /// @return The unspecified IPv4 address, with scope id 0.
  static constexpr IpAddress any_ipv4() noexcept { return ipv4({}); }

  /// @brief Returns ::, the IPv6 wildcard used to bind on every interface.
  ///
  /// @return The unspecified IPv6 address, with scope id 0.
  static constexpr IpAddress any_ipv6() noexcept { return ipv6({}); }

  /// @brief Returns 127.0.0.1.
  ///
  /// @return The usual IPv4 loopback address.
  static constexpr IpAddress loopback_ipv4() noexcept {
    return ipv4({127, 0, 0, 1});
  }

  /// @brief Returns ::1.
  ///
  /// @return The IPv6 loopback address, with scope id 0.
  static constexpr IpAddress loopback_ipv6() noexcept {
    return ipv6({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
  }

  /// @brief Returns a copy of this address that carries a scope id.
  ///
  /// @param scope_id Interface index the address is scoped to, or 0 for none.
  ///     It is discarded for an IPv4 address, which always keeps scope id 0.
  /// @return The scoped copy; this address is unchanged.
  constexpr IpAddress with_scope_id(std::uint32_t scope_id) const noexcept {
    IpAddress address = *this;
    address.scope_id_ = is_ipv6() ? scope_id : 0;
    return address;
  }

  /// @brief Returns the interface index an IPv6 address is scoped to, or 0
  /// when it has none.
  ///
  /// @return The scope id; always 0 for an IPv4 address.
  constexpr std::uint32_t scope_id() const noexcept { return scope_id_; }

  /// @brief Reports whether this is an IPv6 link-local address (fe80::/10).
  ///
  /// @return True for an IPv6 address in fe80::/10; false otherwise.
  /// @note An IPv4 link-local address (169.254.0.0/16) returns false.
  constexpr bool is_link_local() const noexcept {
    return is_ipv6() && bytes_[0] == 0xfe && (bytes_[1] & 0xc0) == 0x80;
  }

  /// @brief Returns whether the address is IPv4 or IPv6.
  ///
  /// @return The family the address was built with.
  constexpr IpFamily family() const noexcept { return family_; }
  /// @brief Reports whether the address is IPv4.
  ///
  /// @return True for an IPv4 address; false for IPv6, including an
  ///     IPv4-mapped IPv6 address.
  constexpr bool is_ipv4() const noexcept { return family_ == IpFamily::ipv4; }
  /// @brief Reports whether the address is IPv6.
  ///
  /// @return True for an IPv6 address, including an IPv4-mapped one; false
  ///     for IPv4.
  constexpr bool is_ipv6() const noexcept { return family_ == IpFamily::ipv6; }

  /// @brief Returns the four octets of an IPv4 address in network order.
  ///
  /// @pre is_ipv4(). For an IPv6 address the result is its first four bytes,
  ///     which is not an IPv4 address; use unmapped_ipv4() to extract a
  ///     mapped one.
  /// @return The first four address bytes, by value.
  constexpr std::array<std::uint8_t, 4> ipv4_bytes() const noexcept {
    return std::array<std::uint8_t, 4>{bytes_[0], bytes_[1], bytes_[2],
                                       bytes_[3]};
  }

  /// @brief Returns the sixteen bytes of an IPv6 address in network order.
  ///
  /// @pre is_ipv6(). For an address built with ipv4() the result is the four
  ///     octets followed by twelve zero bytes.
  /// @return All sixteen stored bytes, by value.
  constexpr std::array<std::uint8_t, 16> ipv6_bytes() const noexcept {
    return bytes_;
  }

  /// @brief Returns the address bytes in network order: four for IPv4,
  /// sixteen for IPv6.
  ///
  /// The view refers into this address and is valid while it lives. It is
  /// the form wire encoders and decoders work with.
  ///
  /// @return A view of four bytes for IPv4 or sixteen for IPv6; the scope id
  ///     is not included.
  auto bytes() const noexcept -> std::span<const std::byte> {
    return std::as_bytes(std::span{bytes_}).first(is_ipv4() ? 4 : 16);
  }

  /// @brief Builds an IPv4 address from a 32-bit number in host byte order.
  ///
  /// @param number The address as a number whose most significant byte is the
  ///     first octet, so 0xC0A80114 is 192.168.1.20.
  /// @return The IPv4 address.
  static constexpr IpAddress from_ipv4_number(std::uint32_t number) noexcept {
    return ipv4({static_cast<std::uint8_t>(number >> 24),
                 static_cast<std::uint8_t>(number >> 16),
                 static_cast<std::uint8_t>(number >> 8),
                 static_cast<std::uint8_t>(number)});
  }

  /// @brief Returns an IPv4 address as a 32-bit number in host byte order,
  /// with the first octet in the most significant byte.
  ///
  /// @pre is_ipv4().
  /// @return The number, so 192.168.1.20 is 0xC0A80114; for an IPv6 address
  ///     it is built from the first four bytes and is meaningless.
  constexpr std::uint32_t ipv4_number() const noexcept {
    return static_cast<std::uint32_t>(bytes_[0]) << 24 |
           static_cast<std::uint32_t>(bytes_[1]) << 16 |
           static_cast<std::uint32_t>(bytes_[2]) << 8 |
           static_cast<std::uint32_t>(bytes_[3]);
  }

  /// @brief Reports whether the address is all zeros (0.0.0.0 or ::).
  ///
  /// @return True when every address byte of its family is zero, whatever
  ///     the scope id.
  constexpr bool is_unspecified() const noexcept {
    for (std::size_t i = 0; i < (is_ipv4() ? 4 : 16); ++i) {
      if (bytes_[i] != 0) return false;
    }
    return true;
  }

  /// @brief Reports whether the address is a loopback address: any IPv4
  /// address in 127.0.0.0/8, or ::1.
  ///
  /// @return True for those addresses; false otherwise, including for an
  ///     IPv4-mapped loopback such as ::ffff:127.0.0.1.
  constexpr bool is_loopback() const noexcept {
    if (is_ipv4()) return bytes_[0] == 127;
    for (std::size_t i = 0; i < 15; ++i) {
      if (bytes_[i] != 0) return false;
    }
    return bytes_[15] == 1;
  }

  /// @brief Reports whether this is an IPv4-mapped IPv6 address
  /// (::ffff:a.b.c.d).
  ///
  /// @return True for an IPv6 address in ::ffff:0:0/96; false for every
  ///     IPv4 address.
  constexpr bool is_ipv4_mapped() const noexcept {
    if (!is_ipv6()) return false;
    for (std::size_t i = 0; i < 10; ++i) {
      if (bytes_[i] != 0) return false;
    }
    return bytes_[10] == 0xff && bytes_[11] == 0xff;
  }

  /// @brief Returns the IPv4 address inside an IPv4-mapped IPv6 address.
  ///
  /// @return The embedded IPv4 address when is_ipv4_mapped(); otherwise this
  ///     address unchanged.
  constexpr IpAddress unmapped_ipv4() const noexcept {
    if (!is_ipv4_mapped()) return *this;
    return ipv4({bytes_[12], bytes_[13], bytes_[14], bytes_[15]});
  }

  /// @brief Compares two addresses for equality.
  ///
  /// Addresses of different families are never equal, so an IPv4 address
  /// does not equal its IPv4-mapped IPv6 form. IPv6 addresses must also have
  /// the same scope id.
  ///
  /// @param left First address.
  /// @param right Second address.
  /// @return True when family, bytes and scope id all match.
  friend constexpr auto operator==(const IpAddress& left,
                                   const IpAddress& right) noexcept
      -> bool = default;

  /// @brief Orders two addresses: every IPv4 address before every IPv6
  /// address, then by bytes in network order, then by scope id.
  ///
  /// The order is total and stable, so addresses can key ordered containers
  /// and be sorted deterministically.
  ///
  /// @param left First address.
  /// @param right Second address.
  /// @return The ordering of @p left relative to @p right.
  friend constexpr auto operator<=>(const IpAddress& left,
                                    const IpAddress& right) noexcept
      -> std::strong_ordering = default;

 private:
  IpFamily family_{IpFamily::ipv4};
  std::array<std::uint8_t, 16> bytes_{};
  std::uint32_t scope_id_{0};
};

/// @brief An address and port pair naming one end of a UDP or TCP conversation.
struct SocketEndpoint {
  /// @brief The IP address.
  IpAddress address;
  /// @brief The port in host byte order. Port 0 passed to a bind call asks
  /// the system to choose a free port.
  std::uint16_t port{0};

  /// @brief Reports whether two endpoints have equal addresses and ports.
  ///
  /// @param left First endpoint.
  /// @param right Second endpoint.
  /// @return True when both the address and the port match.
  friend constexpr auto operator==(const SocketEndpoint& left,
                                   const SocketEndpoint& right) noexcept
      -> bool = default;

  /// @brief Orders two endpoints by address, then by port.
  ///
  /// @param left First endpoint.
  /// @param right Second endpoint.
  /// @return The ordering of @p left relative to @p right.
  friend constexpr auto operator<=>(const SocketEndpoint& left,
                                    const SocketEndpoint& right) noexcept
      -> std::strong_ordering = default;
};

/// @brief Parses a numeric IP address from text without consulting DNS.
///
/// Accepts dotted-decimal IPv4 ("192.168.1.20") and IPv6 text ("fe80::1"). An
/// IPv6 address may end in "%zone", where the zone is either an interface index
/// or an interface name that the system can map to one; it becomes the scope
/// id. IPv4 parsing is strict: four decimal fields, each at most 255, with no
/// leading zeros.
///
/// @param text The address text. Host names are not accepted.
/// @return The parsed address.
/// @retval NetError::invalid_argument The text is not a valid address, a zone
///     was given on an IPv4 address, or the zone names no known interface.
[[nodiscard]] auto parse_ip_address(std::string_view text)
    -> Expected<IpAddress, NetError>;

/// @brief Formats an address as text without its scope id.
///
/// @param address The address to format.
/// @return Dotted-decimal text for IPv4; lower-case hexadecimal groups with the
///     longest run of zero groups shortened to "::" for IPv6.
///     parse_ip_address() accepts the result.
auto to_string(const IpAddress& address) -> std::string;

/// @brief Formats an address as text, appending "%zone" when it has a scope id.
///
/// Use this form when the text must still identify the interface, for example
/// to show or store a link-local IPv6 address.
///
/// @param address The address to format.
/// @return The same text as to_string(), followed by "%" and the zone when
///     scope_id() is not 0. The zone is the interface name when the address is
///     link-local and the system can name that interface, otherwise the number.
auto to_scoped_string(const IpAddress& address) -> std::string;

}  // namespace libchiaking::platform
