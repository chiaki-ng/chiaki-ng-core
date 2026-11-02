/// @file
/// @brief Declares ReplyInteger, a GraphQL reply integer that tolerates the
/// wrong type and out-of-range values, and its glaze reader.
#pragma once

#include <glaze/glaze.hpp>

#include <algorithm>
#include <charconv>
#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>

namespace libchiaking::psn::graphql {

/// @brief An integer field that tells "absent" from "present but not an
/// integer". An immutable value.
///
/// @tparam Int The signed integer type held; values outside it are clamped.
template <std::signed_integral Int>
class ReplyInteger {
 public:
  /// @brief Makes an absent field.
  ReplyInteger() = default;

  /// @brief Reads a raw JSON value.
  ///
  /// @param raw The value as written.
  /// @return A present field whose integer is `raw` clamped to `Int`, or that
  ///     has no integer when `raw` is not a whole number (null, a string, a
  ///     fraction).
  static auto of_raw(std::string_view raw) noexcept -> ReplyInteger {
    ReplyInteger read;
    read.present_ = true;
    if (const auto signed_value = whole<std::int64_t>(raw))
      read.value_ = static_cast<Int>(std::clamp<std::int64_t>(
          *signed_value, std::numeric_limits<Int>::min(),
          std::numeric_limits<Int>::max()));
    else if (const auto unsigned_value = whole<std::uint64_t>(raw))
      read.value_ = static_cast<Int>(std::min<std::uint64_t>(
          *unsigned_value,
          static_cast<std::uint64_t>(std::numeric_limits<Int>::max())));
    return read;
  }

  /// @brief Tells whether the field was in the reply.
  ///
  /// @return True when it was, whatever its value.
  auto present() const noexcept -> bool { return present_; }
  /// @brief Reads the integer.
  ///
  /// @return It, or nothing when absent or not a whole number.
  auto integer() const noexcept -> std::optional<Int> { return value_; }
  /// @brief Reads the integer with a default.
  ///
  /// @param fallback What to answer without one.
  /// @return The integer, or `fallback`.
  auto value_or(Int fallback) const noexcept -> Int {
    return value_.value_or(fallback);
  }

 private:
  /// @brief Parses a whole number in full.
  ///
  /// @tparam Wide The type parsed into.
  /// @param raw The text.
  /// @return The number, or nothing when the text is empty, not a number of
  ///     that type, or has anything after it.
  template <class Wide>
  static auto whole(std::string_view raw) noexcept -> std::optional<Wide> {
    Wide parsed{};
    const auto* last = raw.data() + raw.size();
    const auto [stop, failed] = std::from_chars(raw.data(), last, parsed);
    if (raw.empty() || failed != std::errc{} || stop != last)
      return std::nullopt;
    return parsed;
  }

  /// @brief Whether the field was in the reply.
  bool present_{};
  /// @brief The integer, clamped.
  std::optional<Int> value_;
};

}  // namespace libchiaking::psn::graphql

/// @brief Lets glaze read a ReplyInteger from any JSON value.
///
/// @tparam Int The integer type held.
template <class Int>
struct glz::from<glz::JSON, libchiaking::psn::graphql::ReplyInteger<Int>> {
  /// @brief Reads a ReplyInteger.
  ///
  /// The value is taken raw and passed to ReplyInteger::of_raw(); a value of
  /// the wrong type is not an error.
  ///
  /// @tparam Opts The glaze read options.
  /// @param value Where the result goes.
  /// @param ctx The glaze context; an error in it ends the read.
  /// @param it The read position, advanced past the value.
  /// @param end The end of the input.
  template <auto Opts>
  static void op(libchiaking::psn::graphql::ReplyInteger<Int>& value,
                 glz::is_context auto&& ctx, auto&& it, auto end) {
    glz::raw_json_view raw{};
    glz::parse<glz::JSON>::op<Opts>(raw, ctx, it, end);
    if (bool(ctx.error)) return;
    value = libchiaking::psn::graphql::ReplyInteger<Int>::of_raw(raw.str);
  }
};
