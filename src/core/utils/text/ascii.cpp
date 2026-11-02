#include "src/core/utils/text/ascii.hpp"

#include "src/core/utils/text/utf8.hpp"

#include <algorithm>

namespace libchiaking::text {

auto casecmp(std::string_view left, std::string_view right) noexcept
    -> std::weak_ordering {
  const auto folded = [](char value) {
    return static_cast<unsigned char>(ascii_lower(value));
  };
  return std::lexicographical_compare_three_way(
      left.begin(), left.end(), right.begin(), right.end(),
      [&folded](char a, char b) -> std::weak_ordering {
        return folded(a) <=> folded(b);
      });
}

auto ieq(std::string_view left, std::string_view right) noexcept -> bool {
  return std::is_eq(casecmp(left, right));
}

auto truncate(std::string value, std::size_t capacity) -> std::string {
  if (capacity == 0) return {};
  value.resize(utf8::prefix(value, capacity - 1).size());
  return value;
}

auto c_string(std::string_view value) noexcept -> std::string_view {
  const auto nul = value.find('\0');
  return nul == std::string_view::npos ? value : value.substr(0, nul);
}

auto trim(std::string_view value) noexcept -> std::string_view {
  while (!value.empty() && ascii_space(value.front())) value.remove_prefix(1);
  while (!value.empty() && ascii_space(value.back())) value.remove_suffix(1);
  return value;
}

auto trim_http_space(std::string_view value) noexcept -> std::string_view {
  while (!value.empty() && http_space(value.front())) value.remove_prefix(1);
  while (!value.empty() && http_space(value.back())) value.remove_suffix(1);
  return value;
}

}  // namespace libchiaking::text
