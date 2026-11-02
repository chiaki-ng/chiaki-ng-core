/// @file
/// @brief Declares Host, the address or host name a console is reached at.
#pragma once

#include "libchiaking/detail/consoles/firmware.hpp"
#include "libchiaking/detail/expected.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::consoles {

/// @brief An IP address or host name a console can be reached at on the home
/// network.
///
/// A Host is checked for shape only when it is created; it is not resolved or
/// probed. PS4 consoles are reached over IPv4 only, so an IPv6 literal is valid
/// for a PS5 alone.
class Host {
 public:
  /// @brief Why text was not accepted as a host.
  enum class Invalid : std::uint8_t {
    /// @brief The text is empty.
    empty,
    /// @brief The text is longer than limit.
    too_long,
    /// @brief The text holds a space, a control character or a byte outside
    /// ASCII.
    not_printable,
    /// @brief The text is an IPv6 literal and the console is a PS4.
    ipv6_needs_ps5,
  };

  /// @brief Longest host accepted, in bytes.
  static constexpr std::size_t limit = 255;
  /// @brief Lowest byte value accepted in a host.
  static constexpr char first_printable = 33;
  /// @brief Highest byte value accepted in a host.
  static constexpr char last_printable = 126;

  /// @brief Validates text as a host for a console of the given generation.
  ///
  /// @param text An IPv4 literal, an IPv6 literal with or without square
  ///     brackets and zone, or a host name.
  /// @param generation The generation of the console the host is for.
  /// @return The host holding the text unchanged.
  /// @retval Invalid::empty The text is empty.
  /// @retval Invalid::too_long The text is longer than limit.
  /// @retval Invalid::not_printable A byte lies outside first_printable to
  ///     last_printable.
  /// @retval Invalid::ipv6_needs_ps5 The text is an IPv6 literal and the
  ///     generation is Generation::ps4.
  [[nodiscard]] static auto create(std::string_view text, Generation generation)
      -> Expected<Host, Invalid>;

  /// @brief The host exactly as it was given.
  ///
  /// @return The text, which lives as long as this host.
  auto text() const noexcept -> const std::string& { return text_; }
  /// @brief The host as a resolver takes it.
  ///
  /// @return The text without the square brackets around an IPv6 literal,
  ///     or the text unchanged when it has none. The view lives as long as
  ///     this host.
  auto unbracketed() const noexcept -> std::string_view {
    return without_brackets(text_);
  }
  /// @brief Whether the host is an IPv6 literal. A host name is never one.
  ///
  /// @return True when the host is an IPv6 literal, false for an IPv4 literal
  /// or a host name.
  auto is_ipv6() const noexcept -> bool { return ipv6_; }
  /// @brief Whether a console of the given generation can be reached at this
  /// host.
  ///
  /// @param generation The console's generation.
  /// @return False only for an IPv6 literal and a PS4.
  auto reaches(Generation generation) const noexcept -> bool {
    return !ipv6_ || generation == Generation::ps5;
  }

  /// @brief Compares the text.
  friend auto operator==(const Host&, const Host&) noexcept -> bool = default;

 private:
  Host(std::string text, bool ipv6) noexcept
      : text_(std::move(text)), ipv6_(ipv6) {}

  static auto ipv6_literal(std::string_view text) -> bool;
  static constexpr auto without_brackets(std::string_view text) noexcept
      -> std::string_view {
    if (text.size() >= 2 && text.front() == '[' && text.back() == ']')
      return text.substr(1, text.size() - 2);
    return text;
  }

  std::string text_;
  bool ipv6_;
};

}  // namespace libchiaking::consoles
