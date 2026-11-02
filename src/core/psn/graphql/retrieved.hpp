/// @file
/// @brief Declares Retrieved, a GraphQL reply field that tells "absent",
/// "null" and "present but unreadable" apart, and its glaze reader.
///
/// GraphQL answers `null` for a thing that does not exist, which the clients
/// report as not found; an unreadable value is malformed instead.
#pragma once

#include "src/core/utils/json/ps_reply.hpp"

#include <glaze/glaze.hpp>

#include <optional>
#include <string_view>
#include <utility>

namespace libchiaking::psn::graphql {

/// @brief A field of a GraphQL reply and how it arrived.
///
/// @tparam Value The shape the field is decoded into; default-constructible.
template <class Value>
class Retrieved {
 public:
  /// @brief Makes an absent field.
  Retrieved() = default;

  /// @brief Reads a raw JSON value.
  ///
  /// @param raw The value as written.
  /// @return A null field for `null`; otherwise an answered field holding the
  ///     decoded value, or none when it does not decode.
  static auto of_raw(std::string_view raw) -> Retrieved {
    Retrieved read;
    if (raw == "null") {
      read.null_ = true;
      return read;
    }
    read.answered_ = true;
    Value decoded{};
    if (!glz::read<json::ps::reply_options>(decoded, raw))
      read.value_.emplace(std::move(decoded));
    return read;
  }

  /// @brief Tells whether the field was not answered.
  ///
  /// @return True when it was absent or null.
  auto missing() const noexcept -> bool { return !answered_; }
  /// @brief Tells whether the field was `null`.
  ///
  /// @return True when it was.
  auto null() const noexcept -> bool { return null_; }
  /// @brief Tells whether a value was decoded.
  ///
  /// @return True when one is held.
  explicit operator bool() const noexcept { return value_.has_value(); }
  /// @brief Reaches the value; one must be held.
  ///
  /// @return The value.
  auto operator*() & noexcept -> Value& { return *value_; }
  /// @brief Reaches the value's members; one must be held.
  ///
  /// @return The value.
  auto operator->() noexcept -> Value* { return &*value_; }

 private:
  /// @brief Whether the field held something other than `null`.
  bool answered_{};
  /// @brief Whether the field was `null`.
  bool null_{};
  /// @brief The decoded value.
  std::optional<Value> value_;
};

}  // namespace libchiaking::psn::graphql

/// @brief Lets glaze read a Retrieved from any JSON value.
///
/// @tparam Value The shape the field is decoded into.
template <class Value>
struct glz::from<glz::JSON, libchiaking::psn::graphql::Retrieved<Value>> {
  /// @brief Reads a Retrieved.
  ///
  /// The value is taken raw and passed to Retrieved::of_raw().
  ///
  /// @tparam Opts The glaze read options.
  /// @param value Where the result goes.
  /// @param ctx The glaze context; an error in it ends the read.
  /// @param it The read position, advanced past the value.
  /// @param end The end of the input.
  template <auto Opts>
  static void op(libchiaking::psn::graphql::Retrieved<Value>& value,
                 glz::is_context auto&& ctx, auto&& it, auto end) {
    glz::raw_json_view raw{};
    glz::parse<glz::JSON>::op<Opts>(raw, ctx, it, end);
    if (bool(ctx.error)) return;
    value = libchiaking::psn::graphql::Retrieved<Value>::of_raw(raw.str);
  }
};
