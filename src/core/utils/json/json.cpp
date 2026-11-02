#include "src/core/utils/json/json.hpp"

#include "src/core/utils/text/ascii.hpp"

namespace libchiaking::json {

auto nested_within(std::string_view text, std::size_t depth) noexcept -> bool {
  std::size_t open = 0;
  bool quoted = false;
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char symbol = text[index];
    if (quoted) {
      if (symbol == '\\')
        ++index;
      else if (symbol == '"')
        quoted = false;
      continue;
    }
    if (symbol == '"')
      quoted = true;
    else if (symbol == '{' || symbol == '[') {
      if (++open > depth) return false;
    } else if ((symbol == '}' || symbol == ']') && open != 0)
      --open;
  }
  return true;
}

auto admitted(std::string_view text, std::size_t maximum_bytes)
    -> Expected<std::string_view, ParseError> {
  if (text.size() > maximum_bytes) return unexpected(ParseError::too_large);
  const auto document = text::c_string(text);
  if (!nested_within(document, max_depth))
    return unexpected(ParseError::too_deep);
  return document;
}

auto well_formed(std::string_view text) -> bool {
  glz::skip anything{};
  return !read_into<document_options>(anything, text);
}

}  // namespace libchiaking::json
