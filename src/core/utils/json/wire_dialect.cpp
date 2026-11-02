#include "src/core/utils/json/wire_dialect.hpp"

#include "src/core/utils/json/escape.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <new>
#include <string>
#include <unordered_map>

namespace libchiaking::json {

WireDialect::Value::Value(Kind kind, SecretString scalar) noexcept
    : kind_(kind), scalar_(std::move(scalar)) {}
WireDialect::Value::Value(Object members) noexcept
    : kind_(Kind::object), members_(std::move(members)) {}
WireDialect::Value::Value(Array elements) noexcept
    : kind_(Kind::array), elements_(std::move(elements)) {}
WireDialect::Value::Value(Value&&) noexcept = default;
auto WireDialect::Value::operator=(Value&&) noexcept -> Value& = default;
WireDialect::Value::~Value() = default;

WireDialect::Member::Member(SecretString name, Value value) noexcept
    : name_(std::move(name)), value_(std::move(value)) {}

auto WireDialect::Value::representable() const -> bool {
  switch (kind_) {
    case Kind::number:
      return std::isfinite(
          std::strtod(std::string{scalar_.view()}.c_str(), nullptr));
    case Kind::array:
      return std::ranges::all_of(elements_, [](const Value& element) {
        return element.representable();
      });
    case Kind::object:
      return std::ranges::all_of(members_, [](const Member& member) {
        return member.value().representable();
      });
    case Kind::omitted:
    case Kind::null:
    case Kind::boolean:
    case Kind::string:
      return true;
  }
  return false;
}

auto WireDialect::Value::canonical(std::size_t capacity,
                                   const Concealment* concealment) const
    -> SecretString {
  SecretString text;
  text.reserve(capacity);
  append_canonical(text, concealment);
  return text;
}

void WireDialect::Value::append_canonical(
    SecretString& output, const Concealment* concealment) const {
  switch (kind_) {
    case Kind::omitted:
      output.append("{}");
      return;
    case Kind::null:
      output.append("null");
      return;
    case Kind::boolean:
    case Kind::number:
      output.append(scalar_.view());
      return;
    case Kind::string:
      append_quoted(output, scalar_.view());
      return;
    case Kind::array:
      output.push_back('[');
      for (const auto& element : elements_) {
        if (&element != &elements_.front()) output.push_back(',');
        element.append_canonical(output, concealment);
      }
      output.push_back(']');
      return;
    case Kind::object:
      output.push_back('{');
      for (const auto& member : members_) {
        if (&member != &members_.front()) output.push_back(',');
        append_quoted(output, member.name());
        output.push_back(':');
        if (concealment != nullptr && member.value().kind_ == Kind::string &&
            concealment->conceals(member.name()))
          append_quoted(output, concealment->placeholder);
        else
          member.value().append_canonical(output, concealment);
      }
      output.push_back('}');
      return;
  }
}

void WireDialect::append_indent(Marked& output, std::size_t depth) {
  output.text.push_back('\n');
  output.text.append(std::string(depth * indent_width, ' '));
}

void WireDialect::Value::append_marked(Marked& output, const Conceals& conceals,
                                       std::size_t depth) const {
  if (kind_ != Kind::object && kind_ != Kind::array) {
    append_canonical(output.text, nullptr);
    return;
  }
  const bool object = kind_ == Kind::object;
  const auto count = object ? members_.size() : elements_.size();
  output.text.push_back(object ? '{' : '[');
  for (std::size_t index = 0; index < count; ++index) {
    if (index != 0) output.text.push_back(',');
    append_indent(output, depth + 1);
    if (!object) {
      elements_[index].append_marked(output, conceals, depth + 1);
      continue;
    }
    const auto& member = members_[index];
    append_quoted(output.text, member.name());
    output.text.append(": ");
    if (member.value().kind_ == Kind::string && conceals(member.name())) {
      const auto start = output.text.size() + 1;
      member.value().append_canonical(output.text, nullptr);
      output.concealed.emplace_back(start, output.text.size() - 1 - start);
    } else {
      member.value().append_marked(output, conceals, depth + 1);
    }
  }
  if (count != 0) append_indent(output, depth);
  output.text.push_back(object ? '}' : ']');
}

class WireDialect::Path {
 public:
  auto child(std::string_view name) const -> Path {
    Path next;
    next.text_ = text_.clone();
    next.text_.push_back('/');
    for (const char byte : name) {
      if (byte == '~')
        next.text_.append("~0");
      else if (byte == '/')
        next.text_.append("~1");
      else
        next.text_.push_back(byte);
    }
    return next;
  }
  auto index(std::size_t value) const -> Path {
    std::array<char, 32> digits{};
    const auto converted =
        std::to_chars(digits.data(), digits.data() + digits.size(), value);
    return child({digits.data(),
                  static_cast<std::size_t>(converted.ptr - digits.data())});
  }
  auto may_omit_value() const noexcept -> bool {
    return text_.view() == omitted_value_path;
  }

 private:
  SecretString text_;
};

class WireDialect::Reader {
 public:
  explicit Reader(std::string_view wire) noexcept : wire_(wire) {}

  auto read() -> Expected<Value, ParseError> {
    if (wire_.size() > maximum_bytes) return unexpected(ParseError::too_large);
    if (wire_.starts_with(byte_order_mark)) position_ = byte_order_mark.size();
    return value(Path{}, 0);
  }

 private:
  static constexpr std::string_view byte_order_mark = "\xEF\xBB\xBF";

  static auto invalid() -> Unexpected<ParseError> {
    return unexpected(ParseError::invalid_json);
  }
  static auto ends_literal(char next) noexcept -> bool {
    return next == ',' || next == '}' || next == ']' || text::json_space(next);
  }
  static auto number_character(char next) noexcept -> bool {
    return text::ascii_digit(next) || next == '.' || next == 'e' ||
           next == 'E' || next == '+' || next == '-';
  }
  static auto valid_number(std::string_view value) noexcept -> bool {
    std::size_t index = 0;
    const auto digits = [&] {
      const auto start = index;
      while (index < value.size() && text::ascii_digit(value[index])) ++index;
      return index != start;
    };
    if (index < value.size() && value[index] == '-') ++index;
    if (!digits()) return false;
    if (index < value.size() && value[index] == '.') {
      ++index;
      if (!digits()) return false;
    }
    if (index < value.size() && (value[index] == 'e' || value[index] == 'E')) {
      ++index;
      if (index < value.size() && (value[index] == '+' || value[index] == '-'))
        ++index;
      if (!digits()) return false;
    }
    return index == value.size();
  }
  static auto without_leading_zeros(std::string_view value) -> SecretString {
    SecretString result;
    if (value.starts_with('-')) {
      result.push_back('-');
      value.remove_prefix(1);
    }
    while (value.size() > 1 && value.front() == '0' &&
           text::ascii_digit(value[1]))
      value.remove_prefix(1);
    result.append(value);
    return result;
  }

  void skip_whitespace() noexcept {
    while (position_ < wire_.size() && text::json_space(wire_[position_]))
      ++position_;
  }
  auto consume(char byte) noexcept -> bool {
    skip_whitespace();
    if (position_ == wire_.size() || wire_[position_] != byte) return false;
    ++position_;
    return true;
  }
  auto next_is(char byte) const noexcept -> bool {
    return position_ < wire_.size() && wire_[position_] == byte;
  }
  auto string() -> Expected<SecretString, ParseError> {
    if (!consume('"')) return invalid();
    const auto start = position_;
    while (position_ < wire_.size() && wire_[position_] != '"')
      position_ += wire_[position_] == '\\' ? 2U : 1U;
    if (position_ >= wire_.size()) return invalid();
    SecretString decoded;
    if (!append_unescaped(decoded, wire_.substr(start, position_ - start)))
      return invalid();
    ++position_;
    return decoded;
  }
  auto number() -> Expected<Value, ParseError> {
    const auto start = position_++;
    while (position_ < wire_.size() && number_character(wire_[position_]))
      ++position_;
    const auto text = wire_.substr(start, position_ - start);
    if (!valid_number(text)) return invalid();
    return Value{Kind::number, without_leading_zeros(text)};
  }
  auto literal(std::size_t depth) -> Expected<Value, ParseError> {
    for (const auto& [spelling, kind] :
         {std::pair{std::string_view{"true"}, Kind::boolean},
          std::pair{std::string_view{"false"}, Kind::boolean},
          std::pair{std::string_view{"null"}, Kind::null}}) {
      if (!wire_.substr(position_).starts_with(spelling)) continue;
      position_ += spelling.size();
      if (depth != 0 && position_ < wire_.size() &&
          !ends_literal(wire_[position_]))
        return invalid();
      return Value{
          kind, kind == Kind::null ? SecretString{} : SecretString{spelling}};
    }
    return invalid();
  }
  auto value(const Path& path, std::size_t depth)
      -> Expected<Value, ParseError> {
    skip_whitespace();
    if (position_ == wire_.size()) return invalid();
    const char byte = wire_[position_];
    if (byte == '{' || byte == '[') {
      if (depth >= maximum_depth) return unexpected(ParseError::too_deep);
      return byte == '{' ? object(path, depth + 1) : array(path, depth + 1);
    }
    if (byte == '"') {
      auto decoded = string();
      if (!decoded) return unexpected(decoded.error());
      return Value{Kind::string, std::move(decoded.value())};
    }
    if (byte == '-' || text::ascii_digit(byte)) return number();
    return literal(depth);
  }
  auto member_value(const Path& child, std::size_t depth)
      -> Expected<Value, ParseError> {
    skip_whitespace();
    if (!next_is(',') && !next_is('}')) return value(child, depth);
    if (!child.may_omit_value()) return invalid();
    return Value{Kind::omitted};
  }
  auto object(const Path& path, std::size_t depth)
      -> Expected<Value, ParseError> {
    ++position_;
    Value::Object members;
    std::unordered_map<std::string_view, std::size_t> names;
    if (consume('}')) return Value{std::move(members)};
    do {
      auto key = string();
      if (!key) return unexpected(key.error());
      if (!consume(':')) return invalid();
      auto member = member_value(path.child(key.value().view()), depth);
      if (!member) return unexpected(member.error());
      if (const auto existing = names.find(key.value().view());
          existing != names.end()) {
        members[existing->second].replace(std::move(member.value()));
      } else {
        members.emplace_back(std::move(key.value()), std::move(member.value()));
        names.emplace(members.back().name(), members.size() - 1);
      }
      if (consume('}')) return Value{std::move(members)};
    } while (consume(','));
    return invalid();
  }
  auto array(const Path& path, std::size_t depth)
      -> Expected<Value, ParseError> {
    ++position_;
    Value::Array elements;
    if (consume(']')) return Value{std::move(elements)};
    do {
      auto element = value(path.index(elements.size()), depth);
      if (!element) return unexpected(element.error());
      elements.push_back(std::move(element.value()));
      if (consume(']')) return Value{std::move(elements)};
    } while (consume(','));
    return invalid();
  }

  std::string_view wire_;
  std::size_t position_{};
};

auto WireDialect::tree(std::string_view wire) -> Expected<Value, ParseError> {
  return Reader{wire}.read();
}

auto WireDialect::canonical_text(std::string_view wire)
    -> Expected<SecretString, ParseError> {
  return tree(wire).transform(
      [&wire](const Value& root) { return root.canonical(wire.size()); });
}

auto WireDialect::concealed_text(std::string_view wire,
                                 const Conceals& conceals,
                                 std::string_view placeholder)
    -> Expected<SecretString, ParseError> {
  const Concealment concealment{conceals, placeholder};
  return tree(wire).transform([&](const Value& root) {
    return root.canonical(wire.size(), &concealment);
  });
}

auto WireDialect::marked_text(std::string_view wire, const Conceals& conceals)
    -> Expected<Marked, ParseError> {
  return tree(wire).transform([&](const Value& root) {
    Marked marked;
    marked.text.reserve(wire.size() * 2);
    root.append_marked(marked, conceals, 0);
    return marked;
  });
}

auto WireDialect::representable_text(std::string_view wire)
    -> Expected<SecretString, ParseError> {
  auto root = tree(wire);
  if (!root) return unexpected(root.error());
  if (!root.value().representable())
    return unexpected(ParseError::invalid_json);
  return root.value().canonical(wire.size());
}

}  // namespace libchiaking::json
