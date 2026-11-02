/// @file
/// @brief Declares ContentRating, a game's age rating as the store's GraphQL
/// replies give it, and its tolerant glaze reader.
#pragma once

#include "src/core/utils/json/ps_reply.hpp"

#include <glaze/glaze.hpp>

#include <string>
#include <utility>
#include <vector>

namespace libchiaking::psn::graphql {

/// @brief A game's age rating, such as PEGI 18. Plain data; text fields are
/// empty when PS omitted them.
struct ContentRating {
  /// @brief The rating board, such as `PEGI` or `ESRB`.
  std::string authority;
  /// @brief The rating's name, such as `PEGI_18`.
  std::string name;
  /// @brief The rating's description as shown.
  std::string description;
  /// @brief The rating icon's `url`.
  std::string icon_url;
  /// @brief The non-empty content descriptor descriptions, such as
  /// `Violence`.
  std::vector<std::string> descriptors;
};

}  // namespace libchiaking::psn::graphql

/// @brief Lets glaze read a ContentRating, tolerating malformed fields.
template <>
struct glz::from<glz::JSON, libchiaking::psn::graphql::ContentRating> {
  /// @brief A field tolerated when malformed.
  ///
  /// @tparam Value The field's type.
  template <class Value>
  using Tolerant = libchiaking::json::ps::Tolerant<Value>;

  /// @brief A content descriptor.
  struct Descriptor {
    /// @brief Its description.
    Tolerant<std::string> description;
  };
  /// @brief The rating's fields as sent.
  struct Rated {
    /// @brief The rating board.
    Tolerant<std::string> authority;
    /// @brief The rating's name.
    Tolerant<std::string> name;
    /// @brief The rating's description.
    Tolerant<std::string> description;
    /// @brief The icon URL.
    Tolerant<std::string> url;
    /// @brief The content descriptors.
    Tolerant<std::vector<Tolerant<Descriptor>>> descriptors;
  };

  /// @brief Reads a ContentRating.
  ///
  /// A malformed field reads as empty; descriptors without a description are
  /// dropped.
  ///
  /// @tparam Opts The glaze read options.
  /// @param value Where the result goes.
  /// @param ctx The glaze context; an error in it ends the read.
  /// @param it The read position, advanced past the value.
  /// @param end The end of the input.
  template <auto Opts>
  static void op(libchiaking::psn::graphql::ContentRating& value,
                 glz::is_context auto&& ctx, auto&& it, auto end) {
    Rated rated{};
    glz::parse<glz::JSON>::op<Opts>(rated, ctx, it, end);
    if (bool(ctx.error)) return;
    value = {std::move(rated.authority).value_or(std::string{}),
             std::move(rated.name).value_or(std::string{}),
             std::move(rated.description).value_or(std::string{}),
             std::move(rated.url).value_or(std::string{}),
             {}};
    if (!rated.descriptors) return;
    for (auto& reason : *rated.descriptors)
      if (reason && reason->description && !reason->description->empty())
        value.descriptors.push_back(std::move(*reason->description));
  }
};
