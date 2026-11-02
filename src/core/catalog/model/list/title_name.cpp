#include "src/core/catalog/model/list/title_name.hpp"

#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <utility>
#include <vector>

namespace libchiaking::catalog {

auto TitleName::has_word(std::string_view word) const -> bool {
  if (word.empty()) return false;
  for (std::size_t index = 0; index + word.size() <= raw_.size(); ++index) {
    if (!text::ieq(raw_.substr(index, word.size()), word)) continue;
    const bool before = index == 0 || !text::ascii_alnum(raw_[index - 1]);
    const auto end = index + word.size();
    const bool after = end == raw_.size() || !text::ascii_alnum(raw_[end]);
    if (before && after) return true;
  }
  return false;
}

auto TitleName::is_demo() const -> bool { return has_word("demo"); }

auto TitleName::is_trial() const -> bool { return has_word("trial"); }

auto TitleName::is_digital_extra() const -> bool {
  const auto spaced = normalized();
  std::string joined;
  std::ranges::copy_if(spaced, std::back_inserter(joined),
                       [](char ch) { return ch != ' '; });
  const auto holds = [](std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
  };
  const bool extra =
      holds(joined, "artbook") || spaced.starts_with("the art of ") ||
      holds(spaced, "soundtrack") || holds(joined, "contentviewer") ||
      holds(joined, "bonuscontent");
  const auto master = joined.find("mastercollection");
  const bool playable_bonus =
      master != std::string::npos &&
      joined.find("bonuscontent", master) != std::string::npos;
  return extra && !playable_bonus;
}

auto TitleName::normalized() const -> std::string {
  std::string output;
  bool previous_space = true;
  std::size_t index = 0;
  const auto at = [this](std::size_t position) -> unsigned char {
    return position < raw_.size() ? static_cast<unsigned char>(raw_[position])
                                  : 0;
  };
  while (index < raw_.size() && output.size() < normalized_limit) {
    const unsigned char ch = at(index);
    if (ch == 0xE2 && at(index + 1) == 0x84 &&
        (at(index + 2) == 0xA2 || at(index + 2) == 0xA0)) {
      index += 3;
      continue;
    }
    if (ch == 0xC2 && at(index + 1) == 0xAE) {
      index += 2;
      continue;
    }
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
      if (!previous_space) {
        output += ' ';
        previous_space = true;
      }
      ++index;
      continue;
    }
    output += text::ascii_lower(static_cast<char>(ch));
    previous_space = false;
    ++index;
  }
  while (!output.empty() && output.back() == ' ') output.pop_back();
  return output;
}

auto TitleName::squashed() const -> std::string {
  std::string output;
  std::ranges::copy(
      raw_ | std::views::filter([](char ch) {
        return static_cast<unsigned char>(ch) < 0x80 && text::ascii_alnum(ch);
      }) | std::views::transform(text::ascii_lower) |
          std::views::take(static_cast<std::ptrdiff_t>(squashed_limit)),
      std::back_inserter(output));
  return output;
}

auto TitleName::without_edition_words() const -> std::string {
  std::string lowered;
  lowered.reserve(raw_.size());
  std::ranges::transform(raw_, std::back_inserter(lowered), text::ascii_lower);
  std::vector<std::string> tokens;
  std::string current;
  for (std::size_t at = 0; at < lowered.size();) {
    const auto start = at;
    const auto value = code_point(lowered, at);
    if (letter_or_number(value)) {
      current.append(lowered, start, at - start);
    } else if (!current.empty()) {
      tokens.push_back(std::move(current));
      current.clear();
    }
  }
  if (!current.empty()) tokens.push_back(std::move(current));
  std::string result;
  for (const auto& token : tokens)
    if (std::ranges::find(edition_words, std::string_view{token}) ==
        edition_words.end())
      result += token;
  return result;
}

auto TitleName::code_point(std::string_view text, std::size_t& at) -> char32_t {
  const auto byte = static_cast<unsigned char>(text[at]);
  std::size_t length = byte < 0x80           ? 1
                       : (byte >> 5) == 0x6  ? 2
                       : (byte >> 4) == 0xE  ? 3
                       : (byte >> 3) == 0x1E ? 4
                                             : 1;
  if (at + length > text.size()) length = 1;
  char32_t value = length == 1   ? byte
                   : length == 2 ? (byte & 0x1F)
                   : length == 3 ? (byte & 0x0F)
                                 : (byte & 0x07);
  for (std::size_t index = 1; index < length; ++index)
    value =
        (value << 6) | (static_cast<unsigned char>(text[at + index]) & 0x3F);
  at += length;
  return value;
}

auto TitleName::letter_or_number(char32_t value) -> bool {
  if (value < 0x80) return text::ascii_alnum(static_cast<char>(value));
  if (value <= 0xBF || value == 0xD7 || value == 0xF7)
    return value == 0xAA || value == 0xB2 || value == 0xB3 || value == 0xB5 ||
           value == 0xB9 || value == 0xBA || (value >= 0xBC && value <= 0xBE);
  if ((value >= 0x2000 && value <= 0x206F) ||
      (value >= 0x20A0 && value <= 0x20CF) ||
      (value >= 0x2100 && value <= 0x214F && value != 0x2102 &&
       value != 0x2107 && !(value >= 0x210A && value <= 0x2113) &&
       value != 0x2115 && !(value >= 0x2119 && value <= 0x211D) &&
       value != 0x2124 && value != 0x2126 && value != 0x2128 &&
       !(value >= 0x212A && value <= 0x212D) &&
       !(value >= 0x212F && value <= 0x2139)) ||
      (value >= 0x2190 && value <= 0x2BFF) ||
      (value >= 0x3000 && value <= 0x303F) ||
      (value >= 0xFE30 && value <= 0xFE4F) ||
      (value >= 0xFF00 && value <= 0xFF0F) ||
      (value >= 0xFF1A && value <= 0xFF20) ||
      (value >= 0xFF3B && value <= 0xFF40) ||
      (value >= 0xFF5B && value <= 0xFF65) ||
      (value >= 0x1F000 && value <= 0x1FAFF))
    return false;
  return true;
}

}  // namespace libchiaking::catalog
