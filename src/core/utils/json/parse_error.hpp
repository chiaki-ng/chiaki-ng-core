/// @file
/// @brief Declares the limits of the bounded JSON document reader, why it can
/// refuse a document, and the checks that need no parsing library.
#pragma once

#include <libchiaking/detail/expected.hpp>

#include <cstddef>
#include <cstdint>
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

}  // namespace libchiaking::json
