/// @file
/// @brief Declares ReplyStrings, a GraphQL list of strings that keeps the
/// readable entries and drops the rest, and its glaze reader.
#pragma once

#include "src/core/utils/json/ps_reply.hpp"

#include <glaze/json.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::psn::graphql {

/// @brief A list of strings from a reply, malformed entries dropped. A value.
class ReplyStrings {
 public:
  /// @brief The list as decoded, each entry and the list itself tolerated.
  using Listed =
      json::ps::Tolerant<std::vector<json::ps::Tolerant<std::string>>>;

  /// @brief Makes an empty list.
  ReplyStrings() = default;

  /// @brief Keeps the readable entries.
  ///
  /// @param listed The decoded list; empty when it was not a list.
  explicit ReplyStrings(Listed listed) {
    if (!listed) return;
    for (auto& value : *listed)
      if (value) values_.push_back(std::move(*value));
  }

  /// @brief Tells whether a string is listed.
  ///
  /// @param wanted The string.
  /// @return True when an entry equals it exactly.
  auto contains(std::string_view wanted) const -> bool {
    return std::ranges::find(values_, wanted) != values_.end();
  }
  /// @brief Reads the strings.
  ///
  /// @return The strings, in reply order.
  auto values() const& noexcept -> const std::vector<std::string>& {
    return values_;
  }
  /// @brief Gives up the strings.
  ///
  /// @return The strings, moved out.
  auto values() && noexcept -> std::vector<std::string> {
    return std::move(values_);
  }

 private:
  /// @brief The strings.
  std::vector<std::string> values_;
};

}  // namespace libchiaking::psn::graphql

/// @brief Lets glaze read a ReplyStrings from any JSON value.
template <>
struct glz::from<glz::JSON, libchiaking::psn::graphql::ReplyStrings> {
  /// @brief Reads a ReplyStrings.
  ///
  /// A value that is not a list reads as empty; entries that are not strings
  /// are dropped.
  ///
  /// @tparam Opts The glaze read options.
  /// @param value Where the result goes.
  /// @param ctx The glaze context; an error in it ends the read.
  /// @param it The read position, advanced past the value.
  /// @param end The end of the input.
  template <auto Opts>
  static void op(libchiaking::psn::graphql::ReplyStrings& value,
                 glz::is_context auto&& ctx, auto&& it, auto end) {
    libchiaking::psn::graphql::ReplyStrings::Listed listed{};
    glz::parse<glz::JSON>::op<Opts>(listed, ctx, it, end);
    if (bool(ctx.error)) return;
    value = libchiaking::psn::graphql::ReplyStrings{std::move(listed)};
  }
};
