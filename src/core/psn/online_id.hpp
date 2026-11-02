/// @file
/// @brief Declares OnlineId, a PSN player's public name as PS replies carry
/// it, checked once where it enters the library.
#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::psn {

/// @brief A PSN online id: the public player name shown to other players, as
/// opposed to the numeric account id.
///
/// A value is never empty, at most maximum_bytes long and made only of
/// printable bytes (no control characters and no DEL; bytes of 0x80 and above
/// are allowed, so UTF-8 passes through unchecked). PS replies that name a
/// player (the sign-in token reply, the user profile, push messages) are
/// checked through parse(). It is an immutable value and may be copied and
/// shared between threads freely.
class OnlineId {
 public:
  /// @brief The longest online id accepted, in bytes.
  static constexpr std::size_t maximum_bytes = 256;

  /// @brief Checks a name from a PS reply.
  ///
  /// @param text The name as received.
  /// @return The online id, or nothing when `text` is empty, longer than
  ///     maximum_bytes or holds a byte below 0x20 or equal to 0x7F.
  [[nodiscard]] static auto parse(std::string_view text)
      -> std::optional<OnlineId> {
    if (text.empty() || text.size() > maximum_bytes ||
        !std::ranges::all_of(text, printable))
      return std::nullopt;
    return OnlineId{std::string{text}};
  }

  /// @brief Reads the name.
  ///
  /// @return The name; valid while this object lives and is unchanged.
  auto view() const noexcept -> std::string_view { return text_; }

  /// @brief Gives up the name.
  ///
  /// @return The name, moved out; this object is left empty and must only be
  ///     destroyed or assigned.
  auto take() && noexcept -> std::string { return std::move(text_); }

  /// @brief Compares two online ids byte for byte; PS's names are not
  /// compared case-insensitively here.
  ///
  /// @return Whether the names are identical.
  friend auto operator==(const OnlineId&, const OnlineId&) -> bool = default;

 private:
  /// @brief Wraps a name parse() has checked.
  ///
  /// @param text The checked name.
  explicit OnlineId(std::string text) noexcept : text_(std::move(text)) {}

  /// @brief Tells whether a byte may appear in an online id.
  ///
  /// @param value The byte.
  /// @return False for control bytes below 0x20 and for DEL (0x7F); true
  ///     otherwise.
  static constexpr auto printable(char value) noexcept -> bool {
    const auto byte = static_cast<unsigned char>(value);
    return byte > 0x1FU && byte != 0x7FU;
  }

  /// @brief The name; satisfies the class invariant.
  std::string text_;
};

}  // namespace libchiaking::psn
