/// @file
/// @brief Declares Genres, a game's genres as the store's GraphQL replies list
/// them, and its glaze reader.
#pragma once

#include "src/core/utils/json/ps_reply.hpp"

#include <glaze/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace libchiaking::psn::graphql {

/// @brief A game's genre names, in reply order, blanks and malformed entries
/// dropped.
class Genres {
 public:
  /// @brief A genre entry as sent.
  struct Named {
    /// @brief The genre's name, its `value`.
    json::ps::Tolerant<std::string> value;
  };
  /// @brief The list as decoded, each entry and the list itself tolerated.
  using Listed = json::ps::Tolerant<std::vector<json::ps::Tolerant<Named>>>;

  /// @brief Makes an empty list.
  Genres() = default;

  /// @brief Keeps the named genres.
  ///
  /// @param listed The decoded list; empty when it was not a list.
  explicit Genres(Listed listed) {
    if (!listed) return;
    for (auto& genre : *listed)
      if (genre && genre->value && !genre->value->empty())
        names_.push_back(std::move(*genre->value));
  }

  /// @brief Reads the names.
  ///
  /// @return The names.
  auto names() const& noexcept -> const std::vector<std::string>& {
    return names_;
  }
  /// @brief Gives up the names.
  ///
  /// @return The names, moved out.
  auto names() && noexcept -> std::vector<std::string> {
    return std::move(names_);
  }

 private:
  /// @brief The names.
  std::vector<std::string> names_;
};

}  // namespace libchiaking::psn::graphql

/// @brief Lets glaze read Genres from any JSON value.
template <>
struct glz::from<glz::JSON, libchiaking::psn::graphql::Genres> {
  /// @brief Reads Genres.
  ///
  /// A value that is not a list reads as empty.
  ///
  /// @tparam Opts The glaze read options.
  /// @param value Where the result goes.
  /// @param ctx The glaze context; an error in it ends the read.
  /// @param it The read position, advanced past the value.
  /// @param end The end of the input.
  template <auto Opts>
  static void op(libchiaking::psn::graphql::Genres& value,
                 glz::is_context auto&& ctx, auto&& it, auto end) {
    libchiaking::psn::graphql::Genres::Listed listed{};
    glz::parse<glz::JSON>::op<Opts>(listed, ctx, it, end);
    if (bool(ctx.error)) return;
    value = libchiaking::psn::graphql::Genres{std::move(listed)};
  }
};
