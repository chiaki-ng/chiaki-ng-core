/// @file
/// @brief Declares the bounded JSON document reader the library parses
/// replies and saved records with, on top of glaze.
///
/// Every document is size- and depth-checked before glaze sees it, read
/// through a wiping copy so that secrets in it leave nothing behind, and a
/// failure says whether the text was not JSON or was JSON of the wrong
/// shape.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/secret_string.hpp>
#include <libchiaking/detail/wiping_allocator.hpp>

#include <glaze/glaze.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace libchiaking::json {

/// @brief The largest document read() accepts by default: 4 MiB.
inline constexpr std::size_t max_document_bytes = 4 * 1024 * 1024;
/// @brief The deepest nesting of objects and arrays admitted() accepts.
inline constexpr std::size_t max_depth = 64;

/// @brief Why a JSON document could not be read.
enum class ParseError : std::uint8_t {
  /// @brief The text is longer than the size limit.
  too_large,
  /// @brief Objects and arrays nest deeper than the depth limit.
  too_deep,
  /// @brief The text is not well-formed JSON.
  invalid_json,
  /// @brief The text is well-formed JSON, but not of the shape expected:
  /// a member has the wrong type or a required one is missing.
  unexpected_shape,
};

/// @brief Glaze read options with the two validations glaze leaves opt-in
/// switched on.
struct DocumentOptions : glz::opts {
  /// @brief Values skipped as unknown keys must still be valid JSON.
  bool validate_skipped = true;
  /// @brief Only white space may follow the document.
  bool validate_trailing_whitespace = true;
};

/// @brief The options every document is read with: strict validation, but
/// unknown keys ignored so that PS adding a field breaks nothing.
inline constexpr DocumentOptions document_options = [] {
  DocumentOptions options{};
  options.error_on_unknown_keys = false;
  return options;
}();

/// @brief Reads text into a value with glaze, through a NUL-terminated
/// wiping copy.
///
/// Glaze expects its input buffer to be NUL-terminated, which a view need
/// not be, so the text is copied into a SecretString with a terminator; the
/// copy is wiped when the call returns.
///
/// @tparam Options The glaze options.
/// @tparam Value The type to read into; needs glaze metadata or reflection.
/// @param value Receives what was read; partly filled on failure.
/// @param text The JSON text.
/// @return Glaze's error context, false-like when reading succeeded.
template <auto Options, class Value>
[[nodiscard]] auto read_into(Value& value, std::string_view text)
    -> glz::error_ctx {
  SecretString terminated;
  terminated.reserve(text.size() + 1);
  terminated.append(text);
  terminated.push_back('\0');
  return glz::read<Options>(value,
                            std::string_view{terminated.data(), text.size()});
}

/// @brief Checks that objects and arrays nest no deeper than a limit, without
/// parsing the text.
///
/// Counts brackets outside strings, honouring backslash escapes. Unbalanced
/// closing brackets are ignored; well-formedness is not checked.
///
/// @param text The JSON text.
/// @param depth The deepest nesting allowed.
/// @return False as soon as more than `depth` levels are open; true
///     otherwise.
[[nodiscard]] auto nested_within(std::string_view text,
                                 std::size_t depth) noexcept -> bool;

/// @brief Applies the limits a document must pass before it is parsed.
///
/// A document ends at its first NUL, as some replies carry trailing NULs.
///
/// @param text The received text.
/// @param maximum_bytes The size limit in bytes, checked against the whole
///     text, NULs included.
/// @return The part of the text before its first NUL.
/// @retval ParseError::too_large The text is longer than `maximum_bytes`.
/// @retval ParseError::too_deep It nests deeper than max_depth.
[[nodiscard]] auto admitted(std::string_view text, std::size_t maximum_bytes)
    -> Expected<std::string_view, ParseError>;

/// @brief Tells whether text is a well-formed JSON document, whatever its
/// shape.
///
/// @param text The JSON text.
/// @return True when glaze can skip it as one value followed only by white
///     space.
[[nodiscard]] auto well_formed(std::string_view text) -> bool;

/// @brief Reads a JSON document into a typed value, checking its limits
/// first.
///
/// @tparam Document The type to read; must be default-constructible and
///     readable by glaze.
/// @param text The JSON text; anything from its first NUL on is ignored.
/// @param maximum_bytes The size limit in bytes.
/// @return The document.
/// @retval ParseError::too_large The text exceeds `maximum_bytes`.
/// @retval ParseError::too_deep It nests deeper than max_depth.
/// @retval ParseError::unexpected_shape It is JSON but does not fit
///     `Document`.
/// @retval ParseError::invalid_json It is not well-formed JSON.
template <class Document>
[[nodiscard]] auto read(std::string_view text,
                        std::size_t maximum_bytes = max_document_bytes)
    -> Expected<Document, ParseError> {
  const auto bounded = admitted(text, maximum_bytes);
  if (!bounded) return unexpected(bounded.error());
  Document document{};
  if (read_into<document_options>(document, bounded.value()))
    return unexpected(well_formed(bounded.value())
                          ? ParseError::unexpected_shape
                          : ParseError::invalid_json);
  return document;
}

}  // namespace libchiaking::json
