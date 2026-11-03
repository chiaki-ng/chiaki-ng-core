/// @file
/// @brief Declares field types for reading PS replies and the library's own
/// caches leniently: timestamps that must name their zone, durations, and
/// members that keep their default when the value on the wire is of an
/// unexpected type.
#pragma once

#include "src/core/utils/numeric/checked.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/integer.hpp"
#include "src/core/utils/timing/instant.hpp"

#include <glaze/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace libchiaking::json::ps {

/// @brief A zoned ISO 8601 timestamp, as PS replies and the caches carry it.
using Instant = timing::Instant;

/// @brief A length of time from an ISO 8601 duration, as PS's REST services
/// write play time (`PT12H34M5S`).
///
/// Weeks, days, hours, minutes and seconds are read, each at most once and in
/// that order; a fraction of a second is dropped. Years and months are
/// refused, since how long they last depends on the calendar. A plain value;
/// copy freely.
class Elapsed {
 public:
  /// @brief A whole number of seconds.
  using Length = std::chrono::seconds;

  /// @brief Wraps a length.
  ///
  /// @param length The length.
  constexpr explicit Elapsed(Length length) noexcept : length_(length) {}

  /// @brief Reads an ISO 8601 duration, letters in either case.
  ///
  /// @param text The duration, such as `PT47H12M`, `P1DT2H` or `PT0S`.
  /// @return The length, or nothing when the text does not start with `P`,
  ///     names no part, repeats or misorders a part, holds a year, a month
  ///     or a fraction outside the seconds, ends with a bare `T`, or totals
  ///     more seconds than Length holds.
  static auto from_text(std::string_view text) -> std::optional<Elapsed> {
    if (text.size() < 2 || text::ascii_upper(text.front()) != 'P')
      return std::nullopt;
    std::uint64_t total = 0;
    std::size_t next_rank = 0;
    std::size_t at = 1;
    bool in_time = false;
    bool time_named = false;
    bool named = false;
    while (at < text.size()) {
      if (text::ascii_upper(text[at]) == 'T') {
        if (in_time) return std::nullopt;
        in_time = true;
        ++at;
        continue;
      }
      const auto whole = digits_from(text, at);
      if (whole.empty()) return std::nullopt;
      at += whole.size();
      bool fraction = false;
      if (at < text.size() && (text[at] == '.' || text[at] == ',')) {
        const auto rest = digits_from(text, at + 1);
        if (rest.empty()) return std::nullopt;
        at += 1 + rest.size();
        fraction = true;
      }
      if (at == text.size()) return std::nullopt;
      const auto unit = unit_of(text::ascii_upper(text[at]), in_time);
      ++at;
      if (!unit || unit->rank < next_rank || (fraction && !unit->fractional))
        return std::nullopt;
      next_rank = unit->rank + 1;
      const auto count = text::parse_integer<std::uint64_t>(whole);
      const auto seconds =
          count ? numeric::checked_mul(*count, unit->seconds) : std::nullopt;
      const auto sum =
          seconds ? numeric::checked_add(total, *seconds) : std::nullopt;
      if (!sum) return std::nullopt;
      total = *sum;
      named = true;
      time_named = time_named || in_time;
    }
    if (!named || (in_time && !time_named) ||
        total > static_cast<std::uint64_t>(Length::max().count()))
      return std::nullopt;
    return Elapsed{Length{static_cast<Length::rep>(total)}};
  }

  /// @brief Returns the length.
  ///
  /// @return The length in whole seconds.
  constexpr auto length() const noexcept -> Length { return length_; }

  /// @brief Compares two lengths.
  ///
  /// @return True when both are the same number of seconds.
  friend constexpr auto operator==(Elapsed, Elapsed) noexcept -> bool = default;

 private:
  /// @brief One part a duration may name.
  struct Unit {
    /// @brief Where the part must come, counting from weeks.
    std::size_t rank;
    /// @brief How many seconds one of it lasts.
    std::uint64_t seconds;
    /// @brief Whether it may carry a fraction.
    bool fractional;
  };

  /// @brief Finds the part a designator names.
  ///
  /// @param designator The designator, in upper case.
  /// @param in_time Whether it follows the `T`.
  /// @return The part: weeks and days before `T`, hours, minutes and
  ///     seconds after it; nothing for any other letter or place.
  static constexpr auto unit_of(char designator, bool in_time) noexcept
      -> std::optional<Unit> {
    constexpr std::uint64_t minute = 60;
    constexpr std::uint64_t hour = 60 * minute;
    constexpr std::uint64_t day = 24 * hour;
    if (!in_time && designator == 'W') return Unit{0, 7 * day, false};
    if (!in_time && designator == 'D') return Unit{1, day, false};
    if (in_time && designator == 'H') return Unit{2, hour, false};
    if (in_time && designator == 'M') return Unit{3, minute, false};
    if (in_time && designator == 'S') return Unit{4, 1, true};
    return std::nullopt;
  }

  /// @brief Takes the run of digits that starts at a position.
  ///
  /// @param text The duration.
  /// @param from Where the run starts.
  /// @return The digits, empty when none start there.
  static auto digits_from(std::string_view text, std::size_t from) noexcept
      -> std::string_view {
    if (from >= text.size()) return {};
    const auto rest = text.substr(from);
    const auto end = std::ranges::find_if_not(rest, text::ascii_digit);
    return rest.substr(0, static_cast<std::size_t>(end - rest.begin()));
  }

  /// @brief The length.
  Length length_;
};

/// @brief Glaze's view of one member read leniently: a reference to the
/// member, which the reader fills or leaves alone.
///
/// Made by LenientSpec while glaze reads an object; never stored.
///
/// @tparam Member The member's type.
/// @tparam Wire `void` to read the member as itself, or a `std::chrono`
///     duration to read a count of that unit into an optional duration
///     member.
template <class Member, class Wire>
struct LenientField {
  /// @brief Tells glaze this is a wrapper with its own reader.
  static constexpr bool glaze_wrapper = true;
  /// @brief Tells glaze not to reflect over the wrapper's own members.
  static constexpr bool glaze_reflect = false;
  /// @brief The wrapped type, for glaze.
  using value_type = Member;
  /// @brief The member being read.
  Member& val;
};

/// @brief The entry a glaze object definition holds for a lenient member:
/// which member it is, and how to read it.
///
/// @tparam MemberPointer A pointer to data member.
/// @tparam Wire As for LenientField.
template <class MemberPointer, class Wire>
struct LenientSpec {
  /// @brief The member.
  MemberPointer member;

  /// @brief Wraps the member of the object being read.
  ///
  /// @param parent The object being read.
  /// @return The lenient field referring to its member.
  constexpr auto operator()(auto&& parent) const {
    using Member = std::remove_reference_t<decltype(parent.*member)>;
    return LenientField<Member, Wire>{parent.*member};
  }
};

/// @brief Reads a member leniently in a glaze object definition: a value of
/// the wrong type is skipped and the member keeps its default.
///
/// @tparam MemberPointer A pointer to data member.
/// @param member The member.
/// @return The entry for `glz::object`.
template <class MemberPointer>
constexpr auto lenient(MemberPointer member) {
  return LenientSpec<MemberPointer, void>{member};
}

/// @brief Reads an optional seconds member leniently from a count of seconds
/// since the Unix epoch.
///
/// A number fills the member, null empties it, and anything else is skipped
/// leaving it as it was.
///
/// @tparam MemberPointer A pointer to a `std::optional<std::chrono::seconds>`
///     data member.
/// @param member The member.
/// @return The entry for `glz::object`.
template <class MemberPointer>
constexpr auto lenient_epoch_seconds(MemberPointer member) {
  return LenientSpec<MemberPointer, std::chrono::seconds>{member};
}

}  // namespace libchiaking::json::ps

/// @brief Teaches glaze to read an Instant from a JSON string.
template <>
struct glz::from<glz::JSON, libchiaking::json::ps::Instant> {
  /// @brief Reads the string and parses it as a zoned timestamp.
  ///
  /// @tparam Opts The glaze options in force.
  /// @param value Receives the instant.
  /// @param ctx Glaze's context; set to `parse_error` when the string is
  ///     not a zoned ISO 8601 timestamp.
  /// @param it The read position, advanced past the value.
  /// @param end The end of the input.
  template <auto Opts>
  static void op(libchiaking::json::ps::Instant& value,
                 glz::is_context auto&& ctx, auto&& it, auto end) {
    std::string text;
    glz::parse<glz::JSON>::op<Opts>(text, ctx, it, end);
    if (bool(ctx.error)) return;
    const auto read = libchiaking::json::ps::Instant::from_text(text);
    if (!read) {
      ctx.error = glz::error_code::parse_error;
      return;
    }
    value = *read;
  }
};

/// @brief Teaches glaze to read a lenient member, skipping a value it cannot
/// read instead of failing the document.
///
/// @tparam Member The member's type.
/// @tparam Wire `void`, or the duration unit of an epoch count.
template <class Member, class Wire>
struct glz::from<glz::JSON, libchiaking::json::ps::LenientField<Member, Wire>> {
  /// @brief Reads the value into the member, or skips it on failure.
  ///
  /// The member is assigned only when the whole value reads; otherwise the
  /// error is cleared, the position rewound and the value skipped, so the
  /// document carries on.
  ///
  /// @tparam Opts The glaze options in force.
  /// @param field The lenient field referring to the member.
  /// @param ctx Glaze's context; left without an error unless the value
  ///     cannot even be skipped.
  /// @param it The read position, advanced past the value.
  /// @param end The end of the input.
  template <auto Opts>
  static void op(auto&& field, glz::is_context auto&& ctx, auto&& it,
                 auto end) {
    const auto start = it;
    if constexpr (std::is_void_v<Wire>) {
      Member parsed{};
      glz::parse<glz::JSON>::op<Opts>(parsed, ctx, it, end);
      if (!bool(ctx.error)) {
        field.val = std::move(parsed);
        return;
      }
    } else {
      std::optional<std::uint64_t> count;
      glz::parse<glz::JSON>::op<Opts>(count, ctx, it, end);
      if (!bool(ctx.error)) {
        field.val.reset();
        if (count)
          field.val.emplace(Wire{static_cast<typename Wire::rep>(*count)});
        return;
      }
    }
    ctx.error = glz::error_code::none;
    it = start;
    glz::skip_value<glz::JSON>::op<Opts>(ctx, it, end);
  }
};
