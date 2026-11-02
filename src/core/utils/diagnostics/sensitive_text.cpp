#include "src/core/utils/diagnostics/sensitive_text.hpp"

#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <array>

namespace libchiaking::diagnostics {

auto SensitiveText::contains_ignoring_case(std::string_view value,
                                           std::string_view expected) noexcept
    -> bool {
  return !std::ranges::search(value, expected, [](char left, char right) {
            return text::ascii_lower(left) == right;
          }).empty();
}

auto SensitiveText::ends_with_ignoring_case(std::string_view value,
                                            std::string_view expected) noexcept
    -> bool {
  return value.size() >= expected.size() &&
         text::ieq(value.substr(value.size() - expected.size()), expected);
}

auto SensitiveText::header(std::string_view name) noexcept -> bool {
  constexpr std::string_view gk_prefix = "x-gaikai-";
  return text::ieq(name, "authorization") ||
         text::ieq(name, "proxy-authorization") || text::ieq(name, "cookie") ||
         text::ieq(name, "set-cookie") ||
         (name.size() >= gk_prefix.size() &&
          text::ieq(name.substr(0, gk_prefix.size()), gk_prefix));
}

auto SensitiveText::param(std::string_view key) noexcept -> bool {
  constexpr std::array<std::string_view, 8> names{
      "code",  "access_token", "refresh_token", "npsso",
      "token", "id_token",     "client_secret", "password"};
  return std::ranges::any_of(
      names, [key](std::string_view name) { return text::ieq(key, name); });
}

auto SensitiveText::parameter_values(std::string_view text, std::size_t from)
    -> std::vector<ParameterValue> {
  constexpr std::string_view separators = "?#&";
  std::vector<ParameterValue> values;
  for (auto index = from; index < text.size();) {
    if (separators.find(text[index]) != std::string_view::npos) {
      ++index;
      continue;
    }
    const auto end =
        std::min(text.find_first_of(separators, index), text.size());
    const auto field = text.substr(index, end - index);
    const auto equals = field.find('=');
    if (equals != std::string_view::npos && param(field.substr(0, equals)))
      values.push_back({index + equals + 1, field.size() - equals - 1});
    index = end;
  }
  return values;
}

auto SensitiveText::json_key(std::string_view key) noexcept -> bool {
  constexpr std::array<std::string_view, 4> words{"token", "npsso", "secret",
                                                  "password"};
  constexpr std::array<std::string_view, 7> names{
      "code",       "authorization_code", "authorizationcode", "device_code",
      "devicecode", "user_code",          "usercode"};
  constexpr std::array<std::string_view, 7> endings{
      "key",         "authcode", "auth_code", "postalcode",
      "postal_code", "zipcode",  "zip_code"};
  const auto contained = [key](std::string_view word) {
    return contains_ignoring_case(key, word);
  };
  const auto named = [key](std::string_view name) {
    return text::ieq(key, name);
  };
  const auto ended = [key](std::string_view ending) {
    return ends_with_ignoring_case(key, ending);
  };
  return std::ranges::any_of(words, contained) ||
         std::ranges::any_of(names, named) ||
         std::ranges::any_of(endings, ended);
}

}  // namespace libchiaking::diagnostics
