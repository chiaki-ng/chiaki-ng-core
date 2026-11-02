#include "libchiaking/detail/users/account_context.hpp"

#include "src/core/utils/text/ascii.hpp"

#include <algorithm>

namespace libchiaking::users {

auto AccountContext::stating(const Stated& stated) -> AccountContext {
  AccountContext context;
  if (stated.legal_country)
    context.legal_country_ = country_stated_by(*stated.legal_country);
  if (stated.locale) context.locale_ = locale_stated_by(*stated.locale);
  if (stated.age && *stated.age >= 0 && *stated.age <= oldest_age)
    context.age_ = static_cast<int>(*stated.age);
  context.is_child_ = stated.is_child;
  context.issued_at_ = stated.issued_at;
  return context;
}

auto AccountContext::language() const -> std::optional<std::string> {
  if (!locale_) return std::nullopt;
  return locale_->substr(0, locale_->find('-'));
}

auto AccountContext::is_a_letter(char character) noexcept -> bool {
  return text::ascii_alnum(character) && !text::ascii_digit(character);
}

auto AccountContext::country_stated_by(std::string_view text)
    -> std::optional<std::string> {
  if (text.size() != 2 || !std::ranges::all_of(text, is_a_letter))
    return std::nullopt;
  std::string country{text};
  std::ranges::transform(country, country.begin(), text::ascii_upper);
  return country;
}

auto AccountContext::locale_stated_by(std::string_view text)
    -> std::optional<std::string> {
  const auto dash = text.find('-');
  const auto language = text.substr(0, dash);
  const auto rest = dash == std::string_view::npos ? std::string_view{}
                                                   : text.substr(dash + 1);
  const bool has_rest = dash != std::string_view::npos;
  if (text.size() > locale_limit || language.size() < 2 ||
      language.size() > 3 || !std::ranges::all_of(language, is_a_letter) ||
      (has_rest && rest.empty()) ||
      !std::ranges::all_of(rest, [](char character) {
        return text::ascii_alnum(character) || character == '-';
      }))
    return std::nullopt;
  return std::string{text};
}

}  // namespace libchiaking::users
