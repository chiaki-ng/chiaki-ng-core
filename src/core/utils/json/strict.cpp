#include "src/core/utils/json/strict.hpp"

#include "src/core/utils/json/escape.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <functional>
#include <utility>

namespace libchiaking::json::strict {

class Value::Reader {
 public:
  explicit Reader(std::string_view text) noexcept : text_(text) {}

  [[nodiscard]] auto document() -> std::optional<Value> {
    auto root = value(0);
    skip_whitespace();
    if (!root || position_ != text_.size()) return std::nullopt;
    return root;
  }

 private:
  void skip_whitespace() noexcept {
    while (position_ < text_.size() && text::json_space(text_[position_]))
      ++position_;
  }

  [[nodiscard]] auto consume(char symbol) noexcept -> bool {
    skip_whitespace();
    if (position_ == text_.size() || text_[position_] != symbol) return false;
    ++position_;
    return true;
  }

  [[nodiscard]] auto value(std::size_t depth) -> std::optional<Value> {
    skip_whitespace();
    if (position_ == text_.size()) return std::nullopt;
    const char symbol = text_[position_];
    if (symbol == '{' || symbol == '[') {
      if (depth >= max_depth) return std::nullopt;
      return symbol == '{' ? object(depth + 1) : array(depth + 1);
    }
    if (symbol == '"') {
      auto decoded = string();
      if (!decoded) return std::nullopt;
      return Value{std::move(*decoded)};
    }
    return integer();
  }

  [[nodiscard]] auto integer() -> std::optional<Value> {
    const auto start = position_;
    while (position_ < text_.size() && text::ascii_digit(text_[position_]))
      ++position_;
    const auto digits = text_.substr(start, position_ - start);
    if (digits.empty() || (digits.size() > 1 && digits.front() == '0'))
      return std::nullopt;
    if (position_ < text_.size() &&
        (text_[position_] == '.' || text_[position_] == 'e' ||
         text_[position_] == 'E'))
      return std::nullopt;
    std::uint64_t number{};
    const auto [stop, failed] =
        std::from_chars(digits.data(), digits.data() + digits.size(), number);
    if (failed != std::errc{} || number > max_integer) return std::nullopt;
    return Value{number};
  }

  [[nodiscard]] auto string() -> std::optional<std::string> {
    ++position_;
    const auto start = position_;
    while (position_ < text_.size() && text_[position_] != '"') {
      if (static_cast<unsigned char>(text_[position_]) < 0x20U)
        return std::nullopt;
      position_ += text_[position_] == '\\' ? 2U : 1U;
    }
    if (position_ >= text_.size()) return std::nullopt;
    std::string decoded;
    if (!append_unescaped(decoded, text_.substr(start, position_ - start)) ||
        !std::ranges::all_of(decoded, plain))
      return std::nullopt;
    ++position_;
    return decoded;
  }

  [[nodiscard]] auto object(std::size_t depth) -> std::optional<Value> {
    ++position_;
    Object members;
    if (consume('}')) return Value{std::move(members)};
    do {
      skip_whitespace();
      if (position_ == text_.size() || text_[position_] != '"')
        return std::nullopt;
      auto key = string();
      if (!key || members.size() >= max_members ||
          std::ranges::find(members, *key, &Member::key) != members.end())
        return std::nullopt;
      if (!consume(':')) return std::nullopt;
      auto member = value(depth);
      if (!member) return std::nullopt;
      members.push_back(Member{std::move(*key), std::move(*member)});
      if (consume('}')) return Value{std::move(members)};
    } while (consume(','));
    return std::nullopt;
  }

  [[nodiscard]] auto array(std::size_t depth) -> std::optional<Value> {
    ++position_;
    Array items;
    if (consume(']')) return Value{std::move(items)};
    do {
      if (items.size() >= max_members) return std::nullopt;
      auto item = value(depth);
      if (!item) return std::nullopt;
      items.push_back(std::move(*item));
      if (consume(']')) return Value{std::move(items)};
    } while (consume(','));
    return std::nullopt;
  }

  std::string_view text_;
  std::size_t position_{};
};

Value::Value(std::uint64_t integer) : storage_(integer) {}
Value::Value(std::string string) : storage_(std::move(string)) {}
Value::Value(std::string_view string) : storage_(std::string{string}) {}
Value::Value(const char* string) : storage_(std::string{string}) {}
Value::Value(Array array) : storage_(std::move(array)) {}
Value::Value(SecretString secret) : storage_(std::move(secret)) {}
Value::Value(Value&& other) noexcept = default;
Value& Value::operator=(Value&& other) noexcept = default;
Value::~Value() = default;

Value::Value(Object object) : storage_(std::move(object)) {
  auto& members = std::get<Object>(storage_);
  std::sort(members.begin(), members.end(), precedes);
}

auto Value::parse(std::string_view text) -> std::optional<Value> {
  if (!std::ranges::all_of(text, lexable)) return std::nullopt;
  return Reader{text}.document();
}

auto Value::clone() const -> Value {
  Value copy{std::uint64_t{}};
  copy.storage_ = duplicate(storage_);
  return copy;
}

auto Value::canonical() const -> std::optional<std::string> {
  if (holds_secret()) return std::nullopt;
  std::string output;
  append_to(output);
  return output;
}

auto Value::canonical_secret() const -> SecretString {
  SecretString output;
  append_to(output);
  return output;
}

auto Value::duplicate(const Storage& storage) -> Storage {
  if (const auto* secret = std::get_if<SecretString>(&storage))
    return Storage{secret->clone()};
  if (const auto* number = std::get_if<std::uint64_t>(&storage))
    return Storage{*number};
  if (const auto* text = std::get_if<std::string>(&storage))
    return Storage{*text};
  if (const auto* items = std::get_if<Array>(&storage)) {
    Array copy;
    copy.reserve(items->size());
    for (const auto& item : *items) copy.push_back(item.clone());
    return Storage{std::move(copy)};
  }
  const auto& members = std::get<Object>(storage);
  Object copy;
  copy.reserve(members.size());
  for (const auto& member : members)
    copy.push_back(Member{member.key, member.value.clone()});
  return Storage{std::move(copy)};
}

auto Value::holds_secret() const noexcept -> bool {
  if (secret() != nullptr) return true;
  if (const auto* items = array())
    return std::ranges::any_of(*items, &Value::holds_secret);
  if (const auto* members = object())
    return std::ranges::any_of(*members, [](const Member& member) {
      return member.value.holds_secret();
    });
  return false;
}

auto Value::lexable(char symbol) noexcept -> bool {
  return symbol != '\0' && plain(symbol);
}

auto Value::plain(char symbol) noexcept -> bool {
  return static_cast<unsigned char>(symbol) < 0x80U;
}

auto Value::precedes(const Member& left, const Member& right) noexcept -> bool {
  return left.key < right.key;
}

template <class Output>
void Value::append_integer(Output& output, std::uint64_t value) {
  std::array<char, 20> digits{};
  const auto end =
      std::to_chars(digits.data(), digits.data() + digits.size(), value).ptr;
  output.append(std::string_view{
      digits.data(), static_cast<std::size_t>(end - digits.data())});
}

template <class Output>
void Value::append_to(Output& output) const {
  if (const auto* number = integer()) {
    append_integer(output, *number);
  } else if (const auto* text = string()) {
    append_quoted(output, *text);
  } else if (const auto* hidden = secret()) {
    append_quoted(output, hidden->view());
  } else if (const auto* items = array()) {
    output.push_back('[');
    for (std::size_t index = 0; index < items->size(); ++index) {
      if (index != 0) output.push_back(',');
      (*items)[index].append_to(output);
    }
    output.push_back(']');
  } else if (const auto* members = object()) {
    output.push_back('{');
    for (std::size_t index = 0; index < members->size(); ++index) {
      if (index != 0) output.push_back(',');
      append_quoted(output, (*members)[index].key);
      output.push_back(':');
      (*members)[index].value.append_to(output);
    }
    output.push_back('}');
  }
}

auto ObjectReader::create(std::span<const Member> object) noexcept
    -> std::optional<ObjectReader> {
  if (object.size() > max_members) return std::nullopt;
  for (std::size_t index = 1; index < object.size(); ++index) {
    if (object[index - 1].key >= object[index].key) return std::nullopt;
  }
  return ObjectReader{object};
}

auto ObjectReader::find(std::string_view key) const noexcept
    -> std::optional<std::size_t> {
  const auto position =
      std::ranges::lower_bound(object_, key, std::less<>{}, &Member::key);
  if (position == object_.end() || position->key != key) return std::nullopt;
  return static_cast<std::size_t>(position - object_.begin());
}

template <class Found, class Pick>
auto ObjectReader::take(std::string_view key, Pick pick) noexcept
    -> std::optional<Found> {
  const auto index = find(key);
  if (!index) return std::nullopt;
  const auto* found = std::invoke(pick, object_[*index].value);
  if (found == nullptr) return std::nullopt;
  seen_.set(*index);
  return Found{*found};
}

auto ObjectReader::string(std::string_view key) noexcept
    -> std::optional<std::string_view> {
  return take<std::string_view>(key, &Value::string);
}

auto ObjectReader::integer(std::string_view key) noexcept
    -> std::optional<std::uint64_t> {
  return take<std::uint64_t>(key, &Value::integer);
}

auto ObjectReader::array(std::string_view key) noexcept
    -> std::optional<std::span<const Value>> {
  return take<std::span<const Value>>(key, &Value::array);
}

auto ObjectReader::object(std::string_view key) noexcept
    -> std::optional<std::span<const Member>> {
  return take<std::span<const Member>>(key, &Value::object);
}

auto ObjectReader::value(std::string_view key) noexcept
    -> std::optional<std::reference_wrapper<const Value>> {
  const auto index = find(key);
  if (!index) return std::nullopt;
  seen_.set(*index);
  return std::cref(object_[*index].value);
}

auto ObjectReader::has(std::string_view key) const noexcept -> bool {
  return find(key).has_value();
}

auto ObjectReader::finish() const noexcept -> bool {
  return seen_.count() == object_.size();
}

}  // namespace libchiaking::json::strict
