#pragma once

#include <glaze/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace test_support {

class JsonTree {
 public:
  using Tree = glz::generic_u64;

  static auto parse(std::string_view text) -> std::optional<Tree> {
    Tree tree{};
    if (glz::read_json(tree, text)) return std::nullopt;
    return tree;
  }

  static auto object() -> Tree {
    Tree tree{};
    tree.data = Tree::object_t{};
    return tree;
  }

  static auto array() -> Tree {
    Tree tree{};
    tree.data = Tree::array_t{};
    return tree;
  }

  static auto elements(Tree& tree) -> Tree::array_t& {
    return tree.get<Tree::array_t>();
  }

  static void erase(Tree& tree, std::string_view key) {
    tree.get<Tree::object_t>().erase(key);
  }

  static auto member(const Tree& tree, std::string_view key) -> const Tree* {
    const auto* members = tree.get_if<Tree::object_t>();
    if (members == nullptr) return nullptr;
    const auto found = members->find(key);
    return found == members->end() ? nullptr : &found->second;
  }

  static auto text(const Tree& tree) -> std::string_view {
    const auto* value = tree.get_if<std::string>();
    return value != nullptr ? std::string_view{*value} : std::string_view{};
  }

  static auto text_at(const Tree& tree, std::string_view key)
      -> std::string_view {
    const auto* value = member(tree, key);
    return value != nullptr ? text(*value) : std::string_view{};
  }

  static auto integer_at(const Tree& tree, std::string_view key)
      -> std::optional<std::int64_t> {
    const auto* value = member(tree, key);
    if (value == nullptr) return std::nullopt;
    if (const auto* number = value->get_if<std::uint64_t>())
      return static_cast<std::int64_t>(*number);
    if (const auto* number = value->get_if<std::int64_t>()) return *number;
    return std::nullopt;
  }

  static auto flag_at(const Tree& tree, std::string_view key)
      -> std::optional<bool> {
    const auto* value = member(tree, key);
    if (value == nullptr || value->get_if<bool>() == nullptr)
      return std::nullopt;
    return *value->get_if<bool>();
  }

  static auto number(const Tree& tree) -> std::int64_t {
    if (const auto* value = tree.get_if<std::uint64_t>())
      return static_cast<std::int64_t>(*value);
    if (const auto* value = tree.get_if<std::int64_t>()) return *value;
    return 0;
  }

  static auto count(const Tree& tree) -> std::size_t {
    const auto* items = tree.get_if<Tree::array_t>();
    return items != nullptr ? items->size() : 0;
  }

  static auto dump(const Tree& tree) -> std::string {
    return tree.dump().value_or(std::string{});
  }
};

}  // namespace test_support
