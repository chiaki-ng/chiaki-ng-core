/// @file
/// @brief Declares ReplyText, a GraphQL reply field read as text whether PS
/// sent it as a string or a number, and its glaze reader.
#pragma once

#include "src/core/utils/json/ps_reply.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <glaze/glaze.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::psn::graphql {

/// @brief A text field that may arrive as a JSON string or number; anything
/// else reads as empty. An immutable value.
class ReplyText {
 public:
  /// @brief Makes empty text.
  ReplyText() = default;

  /// @brief Wraps text.
  ///
  /// @param text The text.
  explicit ReplyText(std::string text) noexcept : text_(std::move(text)) {}

  /// @brief Reads a raw JSON value.
  ///
  /// @param raw The value as written.
  /// @return The decoded string, a number token as written, or empty for
  ///     anything else or a string that does not decode.
  static auto of_raw(std::string_view raw) -> ReplyText {
    if (raw.starts_with('"')) {
      std::string decoded;
      if (glz::read<json::ps::reply_options>(decoded, raw)) return {};
      return ReplyText{std::move(decoded)};
    }
    if (number_token(raw)) return ReplyText{std::string{raw}};
    return {};
  }

  /// @brief Reads the text.
  ///
  /// @return The text; valid while this object lives.
  auto view() const noexcept -> std::string_view { return text_; }
  /// @brief Tells whether the text is empty.
  ///
  /// @return True when it is.
  auto empty() const noexcept -> bool { return text_.empty(); }
  /// @brief Gives up the text.
  ///
  /// @return The text, moved out.
  auto take() && noexcept -> std::string { return std::move(text_); }

 private:
  /// @brief Tells whether a raw value starts like a JSON number.
  ///
  /// @param raw The value.
  /// @return True when it starts with `-` or a digit.
  static auto number_token(std::string_view raw) noexcept -> bool {
    return !raw.empty() &&
           (raw.front() == '-' || text::ascii_digit(raw.front()));
  }

  /// @brief The text.
  std::string text_;
};

}  // namespace libchiaking::psn::graphql

/// @brief Lets glaze read a ReplyText from any JSON value.
template <>
struct glz::from<glz::JSON, libchiaking::psn::graphql::ReplyText> {
  /// @brief Reads a ReplyText.
  ///
  /// The value is taken raw and passed to ReplyText::of_raw(); a value of the
  /// wrong type is not an error.
  ///
  /// @tparam Opts The glaze read options.
  /// @param value Where the result goes.
  /// @param ctx The glaze context; an error in it ends the read.
  /// @param it The read position, advanced past the value.
  /// @param end The end of the input.
  template <auto Opts>
  static void op(libchiaking::psn::graphql::ReplyText& value,
                 glz::is_context auto&& ctx, auto&& it, auto end) {
    glz::raw_json_view raw{};
    glz::parse<glz::JSON>::op<Opts>(raw, ctx, it, end);
    if (bool(ctx.error)) return;
    value = libchiaking::psn::graphql::ReplyText::of_raw(raw.str);
  }
};
