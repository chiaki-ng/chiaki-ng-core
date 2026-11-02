#pragma once

#include "src/core/kakusan/model/player_events.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace kakusan_test {

template <class Rig, class Effects>
class TransitionTable {
 public:
  struct Row {
    std::string_view name;
    std::function<void(Rig&)> drive;
  };

  struct Column {
    std::string_view name;
    std::function<Effects(Rig&)> apply;
  };

  struct Pin {
    std::string_view row;
    std::string_view column;
    std::string_view trace;
  };

  static void check(const std::vector<Row>& rows,
                    const std::vector<Column>& columns,
                    const std::vector<Pin>& pins) {
    std::size_t matched = 0;
    for (const auto& row : rows) {
      for (const auto& column : columns) {
        Rig rig;
        row.drive(rig);
        const auto before = rig.spoke.state();
        const auto deadline = rig.spoke.next_deadline_us();
        const auto effects = column.apply(rig);
        const auto actual = render(effects, before, rig.spoke.state(), deadline,
                                   rig.spoke.next_deadline_us(), rig.now);
        const auto expected = lookup(pins, row.name, column.name);
        if (expected) ++matched;
        EXPECT_EQ(actual, expected.value_or(std::string_view{}))
            << "{\"" << row.name << "\", \"" << column.name << "\", \""
            << actual << "\"},";
      }
    }
    EXPECT_EQ(matched, pins.size()) << "a pin names no row and column";
  }

 private:
  static auto lookup(const std::vector<Pin>& pins, std::string_view row,
                     std::string_view column)
      -> std::optional<std::string_view> {
    for (const auto& pin : pins) {
      if (pin.row == row && pin.column == column) return pin.trace;
    }
    return std::nullopt;
  }

  static auto render(const Effects& effects,
                     libchiaking::kakusan::Progress before,
                     libchiaking::kakusan::Progress after,
                     std::optional<std::uint64_t> deadline_before,
                     std::optional<std::uint64_t> deadline_after,
                     std::uint64_t now) -> std::string {
    std::string trace;
    for (const auto& effect : effects) {
      if (!trace.empty()) trace += ' ';
      trace += token(effect);
    }
    if (before != after) trace += std::format(" ->{}", static_cast<int>(after));
    if (deadline_before != deadline_after) {
      if (deadline_after)
        trace +=
            std::format(" @{}", static_cast<std::int64_t>(*deadline_after) -
                                    static_cast<std::int64_t>(now));
      else
        trace += " @none";
    }
    if (!trace.empty() && trace.front() == ' ') trace.erase(0, 1);
    return trace;
  }

  template <class Effect>
  static auto token(const Effect& effect) -> std::string {
    return std::visit(
        [&](const auto& item) {
          using Item = std::remove_cvref_t<decltype(item)>;
          auto text = std::to_string(effect.index());
          if constexpr (std::is_same_v<Item, libchiaking::kakusan::SendControl>)
            text += std::format(":m{}#{}{}", item.message.index(), item.token,
                                item.terminal ? "!" : "");
          else if constexpr (std::is_same_v<Item,
                                            libchiaking::kakusan::ShowState>)
            text +=
                std::format(":p{}e{}r{}{}", static_cast<int>(item.state),
                            item.error ? static_cast<int>(*item.error) : -1,
                            item.reason ? static_cast<int>(*item.reason) : -1,
                            item.by_peer ? "b" : "");
          else if constexpr (requires { item.binding.generation; })
            text += std::format(":{}.{}", item.binding.seat,
                                item.binding.generation);
          return text;
        },
        effect);
  }
};

}  // namespace kakusan_test
