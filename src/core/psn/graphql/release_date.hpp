/// @file
/// @brief Declares ReleaseDate, a game's release date as the store's GraphQL
/// replies give it, either a string or an object with a `value`, and its glaze
/// reader.
#pragma once

#include "src/core/utils/json/ps_reply.hpp"

#include <glaze/glaze.hpp>

#include <string>
#include <utility>
#include <variant>

namespace libchiaking::psn::graphql {

/// @brief A release date as PS wrote it, not parsed; empty when absent or
/// malformed.
class ReleaseDate {
 public:
  /// @brief The object form.
  struct Dated {
    /// @brief The date text.
    json::ps::Tolerant<std::string> value;
  };
  /// @brief The date as decoded: a string, or the object form.
  using Written = json::ps::Tolerant<std::variant<std::string, Dated>>;

  /// @brief Makes an empty date.
  ReleaseDate() = default;

  /// @brief Takes the text out of either form.
  ///
  /// @param written The decoded date.
  explicit ReleaseDate(Written written) {
    if (!written) return;
    if (auto* text = std::get_if<std::string>(&*written))
      text_ = std::move(*text);
    else
      text_ =
          std::move(std::get<Dated>(*written).value).value_or(std::string{});
  }

  /// @brief Reads the date text.
  ///
  /// @return It, or empty.
  auto text() const& noexcept -> const std::string& { return text_; }
  /// @brief Gives up the date text.
  ///
  /// @return It, moved out.
  auto text() && noexcept -> std::string { return std::move(text_); }

 private:
  /// @brief The date text.
  std::string text_;
};

}  // namespace libchiaking::psn::graphql

/// @brief Lets glaze read a ReleaseDate from either form.
template <>
struct glz::from<glz::JSON, libchiaking::psn::graphql::ReleaseDate> {
  /// @brief Reads a ReleaseDate.
  ///
  /// A value of neither form reads as empty.
  ///
  /// @tparam Opts The glaze read options.
  /// @param value Where the result goes.
  /// @param ctx The glaze context; an error in it ends the read.
  /// @param it The read position, advanced past the value.
  /// @param end The end of the input.
  template <auto Opts>
  static void op(libchiaking::psn::graphql::ReleaseDate& value,
                 glz::is_context auto&& ctx, auto&& it, auto end) {
    libchiaking::psn::graphql::ReleaseDate::Written written{};
    glz::parse<glz::JSON>::op<Opts>(written, ctx, it, end);
    if (bool(ctx.error)) return;
    value = libchiaking::psn::graphql::ReleaseDate{std::move(written)};
  }
};
