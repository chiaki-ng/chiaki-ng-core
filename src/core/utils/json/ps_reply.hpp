/// @file
/// @brief Declares how the library reads PS's JSON replies and writes its
/// request bodies: the glaze options for each, and value types that cope
/// with how PS actually writes numbers, strings and optional sections.
///
/// Also teaches glaze to read and write SecretString without passing the
/// secret through a plain string.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/secret_string.hpp>
#include "src/core/utils/json/escape.hpp"
#include "src/core/utils/json/json.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/utf8.hpp"

#include <glaze/json.hpp>
#include <glaze/util/key_transformers.hpp>

#include <charconv>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace libchiaking::json::ps {

/// @brief The options replies are read with: unknown keys ignored, strings
/// required to be valid UTF-8 as glaze checks by default.
inline constexpr glz::opts reply_options{.error_on_unknown_keys = false};

/// @brief Glaze read options for text WireDialect has already made canonical.
struct WireOptions : glz::opts {
  /// @brief Off: strings are kept as sent, even when they are not UTF-8,
  /// as consoles and PS's push service sometimes send them.
  bool validate_utf8 = false;
};

/// @brief The options canonical wire text is read with: unknown keys
/// ignored and UTF-8 not checked.
inline constexpr WireOptions wire_options = [] {
  WireOptions options{};
  options.error_on_unknown_keys = false;
  return options;
}();

/// @brief Glaze write options for request bodies.
struct BodyOptions : glz::opts {
  /// @brief On: control characters are written as `\u00XX` escapes
  /// instead of raw bytes.
  bool escape_control_characters = true;
};

/// @brief The options request bodies are written with: control characters
/// escaped, empty optional members left out.
inline constexpr BodyOptions body_options{};

/// @brief Like body_options, but empty optional members are written as
/// `null`, for messages whose readers expect every member, such as the
/// holepunch session messages.
inline constexpr BodyOptions body_options_with_nulls = [] {
  BodyOptions options{};
  options.skip_null_members = false;
  return options;
}();

/// @brief The tag a reply type names as its `keys` type to have glaze map its
/// snake_case members to camelCase keys.
///
/// Write `using keys = CamelCaseKeys;` in the type.
struct CamelCaseKeys {};

/// @brief Satisfied by types that opted into camelCase keys with
/// CamelCaseKeys.
///
/// @tparam Reply The reply type.
template <class Reply>
concept CamelCaseReply = std::same_as<typename Reply::keys, CamelCaseKeys>;

/// @brief A number PS sometimes sends as a JSON number and sometimes as a
/// string holding one, such as `"42"`.
///
/// Written back as a plain number. A plain value; copy freely.
///
/// @tparam Number The arithmetic type to hold.
template <class Number>
class LenientNumber {
 public:
  /// @brief Makes zero.
  constexpr LenientNumber() noexcept = default;

  /// @brief Wraps a value.
  ///
  /// @param value The number.
  constexpr explicit LenientNumber(Number value) noexcept : value_(value) {}

  /// @brief Reads a number from the text of a JSON string.
  ///
  /// @param text The whole string must be the number, by `std::from_chars`
  ///     rules: no white space, no leading `+`.
  /// @return The number, or nothing when the text is empty, has other
  ///     characters, or the value does not fit `Number`.
  static auto from_text(std::string_view text) noexcept
      -> std::optional<LenientNumber> {
    Number parsed{};
    const auto* last = text.data() + text.size();
    const auto [stop, failed] = std::from_chars(text.data(), last, parsed);
    if (text.empty() || failed != std::errc{} || stop != last)
      return std::nullopt;
    return LenientNumber{parsed};
  }

  /// @brief Returns the number.
  ///
  /// @return The value.
  constexpr auto value() const noexcept -> Number { return value_; }

  /// @brief Compares the values.
  ///
  /// @return True when both hold the same number.
  friend constexpr auto operator==(LenientNumber, LenientNumber) noexcept
      -> bool = default;

 private:
  /// @brief The number.
  Number value_{};
};

/// @brief A JSON number of any kind, kept exactly when it is a whole number
/// and as a double otherwise.
///
/// For replies whose numeric members may be integers or reals, and may be
/// larger than any one C++ type holds. A plain value; copy freely.
class Number {
 public:
  /// @brief The number: a non-negative whole number, a negative one, or a
  /// real.
  using Value = std::variant<std::uint64_t, std::int64_t, double>;

  /// @brief Makes the whole number zero.
  constexpr Number() noexcept = default;

  /// @brief Reads a JSON number token.
  ///
  /// A token without fraction or exponent becomes `std::uint64_t`, or
  /// `std::int64_t` when negative, if it fits; anything else, including a
  /// whole number too large for either, becomes a double read with
  /// `std::strtod`, which honours the C locale's decimal point.
  ///
  /// @param token The token as it appears in the JSON text.
  /// @return The number, or nothing when the token breaks the JSON number
  ///     grammar or its value is not finite as a double.
  static auto from_token(std::string_view token) -> std::optional<Number> {
    if (!grammatical(token)) return std::nullopt;
    if (token.find_first_of(".eE") == std::string_view::npos) {
      if (token.starts_with('-')) {
        if (const auto whole = parsed<std::int64_t>(token))
          return Number{*whole};
      } else if (const auto whole = parsed<std::uint64_t>(token)) {
        return Number{*whole};
      }
    }
    const auto real = std::strtod(std::string{token}.c_str(), nullptr);
    if (!std::isfinite(real)) return std::nullopt;
    return Number{real};
  }

  /// @brief Returns the number in whichever form it was read.
  ///
  /// @return The value.
  constexpr auto value() const noexcept -> const Value& { return value_; }

  /// @brief Returns the number as a signed whole number.
  ///
  /// @return The whole number, or nothing when the number is a real or a
  ///     whole number above `INT64_MAX`.
  constexpr auto integer() const noexcept -> std::optional<std::int64_t> {
    if (const auto* whole = std::get_if<std::uint64_t>(&value_)) {
      if (*whole >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return std::nullopt;
      return static_cast<std::int64_t>(*whole);
    }
    if (const auto* whole = std::get_if<std::int64_t>(&value_)) return *whole;
    return std::nullopt;
  }

 private:
  /// @brief Wraps a value.
  ///
  /// @param value The number.
  constexpr explicit Number(Value value) noexcept : value_(value) {}

  /// @brief Reads a whole token as one integer type.
  ///
  /// @tparam Whole The integer type.
  /// @param token The token.
  /// @return The integer, or nothing when the token is not entirely one or
  ///     it does not fit.
  template <class Whole>
  static auto parsed(std::string_view token) noexcept -> std::optional<Whole> {
    Whole whole{};
    const auto* last = token.data() + token.size();
    const auto [stop, failed] = std::from_chars(token.data(), last, whole);
    if (failed != std::errc{} || stop != last) return std::nullopt;
    return whole;
  }

  /// @brief Checks a token against the JSON number grammar of RFC 8259.
  ///
  /// @param token The token.
  /// @return True for an optional minus, an integer part without leading
  ///     zeros, an optional fraction and an optional exponent, and nothing
  ///     else.
  static constexpr auto grammatical(std::string_view token) noexcept -> bool {
    std::size_t index = 0;
    const auto digit = [&token](std::size_t at) {
      return at < token.size() && text::ascii_digit(token[at]);
    };
    const auto digits = [&] {
      const auto start = index;
      while (digit(index)) ++index;
      return index != start;
    };
    if (index < token.size() && token[index] == '-') ++index;
    if (!digit(index)) return false;
    if (token[index] == '0')
      ++index;
    else
      digits();
    if (index < token.size() && token[index] == '.') {
      ++index;
      if (!digits()) return false;
    }
    if (index < token.size() && (token[index] == 'e' || token[index] == 'E')) {
      ++index;
      if (index < token.size() && (token[index] == '+' || token[index] == '-'))
        ++index;
      if (!digits()) return false;
    }
    return index == token.size();
  }

  /// @brief The number.
  Value value_{std::uint64_t{}};
};

/// @brief A string member that may arrive as any JSON value, read as text when
/// it is a string and as empty otherwise.
///
/// For display strings PS sometimes sends as null or a number. The text is
/// cut at its first NUL when viewed. A plain value; copy freely.
class Text {
 public:
  /// @brief Makes empty text.
  Text() = default;

  /// @brief Wraps a decoded string.
  ///
  /// @param value The text.
  explicit Text(std::string value) noexcept : value_(std::move(value)) {}

  /// @brief Returns the text.
  ///
  /// @return The text up to its first NUL; empty when the wire value was
  ///     not a string. Valid while this object lives.
  auto view() const noexcept -> std::string_view {
    return text::c_string(value_);
  }

 private:
  /// @brief The decoded string.
  std::string value_;
};

/// @brief A member PS sends either as a whole number or as a string, such as
/// an account id or a time stamp, read as decimal text.
///
/// A number must be a whole value from 0 to 2^64 - 1 and is kept exactly; any
/// other number, and any value that is neither a number nor a string, leaves
/// the member empty instead of failing the reply. A string is kept unchecked.
/// A plain value; copy freely.
class DecimalText {
 public:
  /// @brief The value as PS sent it.
  using Wire = std::variant<std::string, std::uint64_t>;

  /// @brief Makes an empty member.
  DecimalText() = default;

  /// @brief Wraps a value read from the wire.
  ///
  /// @param value The number or string.
  explicit DecimalText(Wire value) noexcept : value_(std::move(value)) {}

  /// @brief The value as text, as sent.
  ///
  /// @return A number in decimal; a string unchanged; empty when the member
  ///     was absent or unreadable.
  auto text() const -> std::string {
    if (!value_) return {};
    if (const auto* number = std::get_if<std::uint64_t>(&*value_))
      return std::to_string(*number);
    return std::get<std::string>(*value_);
  }
  /// @brief The value as text, without surrounding white space.
  ///
  /// @return text() with ASCII white space removed from a string's ends.
  auto trimmed() const -> std::string {
    if (!value_) return {};
    if (const auto* string = std::get_if<std::string>(&*value_))
      return std::string{text::trim(*string)};
    return text();
  }
  /// @brief The value as a signed 64-bit integer.
  ///
  /// @return trimmed() read as a whole decimal number, with an optional
  ///     leading minus; nothing when it is empty, has other characters or
  ///     does not fit.
  auto integer() const -> std::optional<std::int64_t> {
    const auto written = trimmed();
    std::int64_t value{};
    const auto* end = written.data() + written.size();
    const auto [stop, error] = std::from_chars(written.data(), end, value);
    if (error != std::errc{} || stop != end) return std::nullopt;
    return value;
  }

 private:
  /// @brief The value, when one was read.
  std::optional<Wire> value_;
};

/// @brief An optional member that is left empty, instead of failing the whole
/// reply, when its value cannot be read as `Value`.
///
/// For sections of a reply that PS fills inconsistently: one unreadable
/// section costs only that section. Behaves like a read-only optional;
/// dereferencing an empty one is undefined. Copyable when `Value` is.
///
/// @tparam Value The type the member should hold.
template <class Value>
class Tolerant {
 public:
  /// @brief Makes an empty member.
  constexpr Tolerant() noexcept = default;

  /// @brief Makes a member holding a value.
  ///
  /// @param value The value.
  constexpr explicit Tolerant(Value value) noexcept(
      std::is_nothrow_move_constructible_v<Value>)
      : value_(std::move(value)) {}

  /// @brief Tells whether the member holds a value.
  ///
  /// @return False when the member was absent, null or unreadable.
  explicit operator bool() const noexcept { return value_.has_value(); }
  /// @brief Views the value; the member must hold one.
  ///
  /// @return The value.
  auto operator*() const& noexcept -> const Value& { return *value_; }
  /// @brief Accesses the value; the member must hold one.
  ///
  /// @return The value.
  auto operator*() & noexcept -> Value& { return *value_; }
  /// @brief Views the value's members; the member must hold one.
  ///
  /// @return The value.
  auto operator->() const noexcept -> const Value* { return &*value_; }
  /// @brief Accesses the value's members; the member must hold one.
  ///
  /// @return The value.
  auto operator->() noexcept -> Value* { return &*value_; }
  /// @brief Copies the value out, or a fallback when empty.
  ///
  /// @tparam Fallback A type convertible to `Value`.
  /// @param fallback The value to use when empty.
  /// @return The held value, or `fallback`.
  template <class Fallback>
  auto value_or(Fallback&& fallback) const& -> Value {
    return value_.value_or(std::forward<Fallback>(fallback));
  }
  /// @brief Moves the value out, or a fallback when empty.
  ///
  /// @tparam Fallback A type convertible to `Value`.
  /// @param fallback The value to use when empty.
  /// @return The held value, or `fallback`.
  template <class Fallback>
  auto value_or(Fallback&& fallback) && -> Value {
    return std::move(value_).value_or(std::forward<Fallback>(fallback));
  }

 private:
  /// @brief The value, when one was read.
  std::optional<Value> value_;
};

/// @brief Reads a PS reply body into its reply type.
///
/// Uses reply_options: unknown keys are ignored and strings must be valid
/// UTF-8. Applies no size or depth limit of its own; callers bound the body
/// when it is received.
///
/// @tparam Reply The reply type; default-constructible and readable by
///     glaze.
/// @param body The reply body.
/// @return The reply, or glaze's error when the body is not JSON of that
///     shape.
template <class Reply>
[[nodiscard]] auto read_reply(std::string_view body)
    -> Expected<Reply, glz::error_ctx> {
  Reply reply{};
  if (const auto failure = read_into<reply_options>(reply, body))
    return unexpected(failure);
  return reply;
}

/// @brief Reads canonical text from WireDialect into a message type.
///
/// Uses wire_options, so strings are kept as sent even when not UTF-8.
///
/// @tparam Reply The message type; default-constructible and readable by
///     glaze.
/// @param canonical Text produced by WireDialect.
/// @return The message, or glaze's error when the text does not fit it.
template <class Reply>
[[nodiscard]] auto read_wire(std::string_view canonical)
    -> Expected<Reply, glz::error_ctx> {
  Reply reply{};
  if (const auto failure = read_into<wire_options>(reply, canonical))
    return unexpected(failure);
  return reply;
}

/// @brief Writes a request body into wiping memory.
///
/// @tparam Options The glaze write options; body_options by default.
/// @tparam Value The type to write; writable by glaze.
/// @param value The body.
/// @return The JSON text.
/// @retval glz::error_code::invalid_utf8 The written text is not valid
///     UTF-8, because a string in `value` is not.
/// @retval glz::error_ctx Any failure glaze reports while writing.
template <auto Options = body_options, class Value>
[[nodiscard]] auto write(const Value& value)
    -> Expected<SecretString, glz::error_ctx> {
  SecretString body;
  if (const auto failure = glz::write<Options>(value, body))
    return unexpected(failure);
  if (!text::utf8::valid(body.view()))
    return unexpected(glz::error_ctx{.ec = glz::error_code::invalid_utf8});
  return body;
}

}  // namespace libchiaking::json::ps

/// @brief Gives every type tagged with CamelCaseKeys glaze's camelCase key
/// mapping.
///
/// @tparam Reply A type tagged with CamelCaseKeys.
template <libchiaking::json::ps::CamelCaseReply Reply>
struct glz::meta<Reply> : glz::camel_case {};

/// @brief Teaches glaze to read a LenientNumber from a number or a numeric
/// string.
///
/// @tparam Number The arithmetic type held.
template <class Number>
struct glz::from<glz::JSON, libchiaking::json::ps::LenientNumber<Number>> {
  /// @brief Reads a number, or a string holding one.
  ///
  /// @tparam Opts The glaze options in force.
  /// @param value Receives the number.
  /// @param ctx Glaze's context; set to `parse_number_failure` when a
  ///     string does not hold a number.
  /// @param it The read position, advanced past the value.
  /// @param end The end of the input.
  template <auto Opts>
  static void op(libchiaking::json::ps::LenientNumber<Number>& value,
                 glz::is_context auto&& ctx, auto&& it, auto end) {
    std::variant<Number, std::string> wire{};
    glz::parse<glz::JSON>::op<Opts>(wire, ctx, it, end);
    if (bool(ctx.error)) return;
    if (const auto* number = std::get_if<Number>(&wire)) {
      value = libchiaking::json::ps::LenientNumber<Number>{*number};
      return;
    }
    const auto parsed = libchiaking::json::ps::LenientNumber<Number>::from_text(
        std::get<std::string>(wire));
    if (!parsed) {
      ctx.error = glz::error_code::parse_number_failure;
      return;
    }
    value = *parsed;
  }
};

/// @brief Teaches glaze to write a LenientNumber as a plain number.
///
/// @tparam Number The arithmetic type held.
template <class Number>
struct glz::to<glz::JSON, libchiaking::json::ps::LenientNumber<Number>> {
  /// @brief Writes the held number.
  ///
  /// @tparam Opts The glaze options in force.
  /// @param value The number.
  /// @param ctx Glaze's context.
  /// @param args The output buffer and position, passed on to glaze.
  template <auto Opts>
  static void op(const libchiaking::json::ps::LenientNumber<Number>& value,
                 glz::is_context auto&& ctx, auto&&... args) {
    glz::serialize<glz::JSON>::op<Opts>(value.value(), ctx, args...);
  }
};

/// @brief Teaches glaze to read a Number from a JSON number token.
template <>
struct glz::from<glz::JSON, libchiaking::json::ps::Number> {
  /// @brief Reads the raw number token and classifies it.
  ///
  /// @tparam Opts The glaze options in force.
  /// @param value Receives the number.
  /// @param ctx Glaze's context; set to `parse_number_failure` when the
  ///     value is not a number token, or not a finite one.
  /// @param it The read position, advanced past the value.
  /// @param end The end of the input.
  template <auto Opts>
  static void op(libchiaking::json::ps::Number& value,
                 glz::is_context auto&& ctx, auto&& it, auto end) {
    glz::raw_json_view raw{};
    glz::parse<glz::JSON>::op<Opts>(raw, ctx, it, end);
    if (bool(ctx.error)) return;
    const auto parsed = libchiaking::json::ps::Number::from_token(raw.str);
    if (!parsed) {
      ctx.error = glz::error_code::parse_number_failure;
      return;
    }
    value = *parsed;
  }
};

/// @brief Teaches glaze to read Text from any JSON value.
template <>
struct glz::from<glz::JSON, libchiaking::json::ps::Text> {
  /// @brief Reads any value, keeping its decoded text when it is a string.
  ///
  /// Never fails on a well-formed value: a non-string, or a string that
  /// does not decode, leaves the text empty.
  ///
  /// @tparam Opts The glaze options in force.
  /// @param value Receives the text.
  /// @param ctx Glaze's context; set only when the value is not well
  ///     formed.
  /// @param it The read position, advanced past the value.
  /// @param end The end of the input.
  template <auto Opts>
  static void op(libchiaking::json::ps::Text& value, glz::is_context auto&& ctx,
                 auto&& it, auto end) {
    glz::raw_json_view raw{};
    glz::parse<glz::JSON>::op<Opts>(raw, ctx, it, end);
    if (bool(ctx.error)) return;
    std::string decoded;
    if (raw.str.starts_with('"') &&
        !glz::read<libchiaking::json::ps::reply_options>(decoded, raw.str))
      value = libchiaking::json::ps::Text{std::move(decoded)};
    else
      value = {};
  }
};

/// @brief Teaches glaze to read a Tolerant member, leaving it empty when the
/// value does not read as `Value`.
///
/// @tparam Value The type the member should hold.
template <class Value>
struct glz::from<glz::JSON, libchiaking::json::ps::Tolerant<Value>> {
  /// @brief Reads any value and keeps it when it decodes as `Value`.
  ///
  /// Never fails on a well-formed value; one that does not decode leaves
  /// the member empty.
  ///
  /// @tparam Opts The glaze options in force.
  /// @param value Receives the member.
  /// @param ctx Glaze's context; set only when the value is not well
  ///     formed.
  /// @param it The read position, advanced past the value.
  /// @param end The end of the input.
  template <auto Opts>
  static void op(libchiaking::json::ps::Tolerant<Value>& value,
                 glz::is_context auto&& ctx, auto&& it, auto end) {
    glz::raw_json_view raw{};
    glz::parse<glz::JSON>::op<Opts>(raw, ctx, it, end);
    if (bool(ctx.error)) return;
    Value decoded{};
    if (glz::read<libchiaking::json::ps::reply_options>(decoded, raw.str))
      value = {};
    else
      value = libchiaking::json::ps::Tolerant<Value>{std::move(decoded)};
  }
};

/// @brief Teaches glaze to read DecimalText from any JSON value.
template <>
struct glz::from<glz::JSON, libchiaking::json::ps::DecimalText> {
  /// @brief Reads a whole number or a string, leaving the member empty for
  /// any other well-formed value.
  ///
  /// @tparam Opts The glaze options in force.
  /// @param value Receives the member.
  /// @param ctx Glaze's context; set only when the value is not well
  ///     formed.
  /// @param it The read position, advanced past the value.
  /// @param end The end of the input.
  template <auto Opts>
  static void op(libchiaking::json::ps::DecimalText& value,
                 glz::is_context auto&& ctx, auto&& it, auto end) {
    libchiaking::json::ps::Tolerant<libchiaking::json::ps::DecimalText::Wire>
        read{};
    glz::parse<glz::JSON>::op<Opts>(read, ctx, it, end);
    if (bool(ctx.error)) return;
    value = read ? libchiaking::json::ps::DecimalText{std::move(*read)}
                 : libchiaking::json::ps::DecimalText{};
  }
};

/// @brief Teaches glaze to write a SecretString as a JSON string straight
/// from its wiping storage.
template <>
struct glz::to<glz::JSON, libchiaking::SecretString> {
  /// @brief Writes the secret as a quoted, escaped string.
  ///
  /// @tparam Opts The glaze options in force.
  /// @param value The secret.
  /// @param ctx Glaze's context.
  /// @param args The output buffer and position, passed on to glaze.
  template <auto Opts>
  static void op(const libchiaking::SecretString& value,
                 glz::is_context auto&& ctx, auto&&... args) {
    glz::serialize<glz::JSON>::op<Opts>(value.view(), ctx, args...);
  }
};

/// @brief Teaches glaze to read a JSON string into a SecretString, unescaping
/// it straight into wiping storage.
template <>
struct glz::from<glz::JSON, libchiaking::SecretString> {
  /// @brief Reads the raw string and unescapes it into the secret.
  ///
  /// @tparam Opts The glaze options in force.
  /// @param value Receives the decoded secret, replacing what it held.
  /// @param ctx Glaze's context; set to `invalid_escape` when an escape is
  ///     malformed.
  /// @param it The read position, advanced past the value.
  /// @param end The end of the input.
  template <auto Opts>
  static void op(libchiaking::SecretString& value, glz::is_context auto&& ctx,
                 auto&& it, auto end) {
    std::string_view escaped{};
    glz::parse<glz::JSON>::op<Opts>(escaped, ctx, it, end);
    if (bool(ctx.error)) return;
    value.clear();
    if (!libchiaking::json::append_unescaped(value, escaped))
      ctx.error = glz::error_code::invalid_escape;
  }
};
