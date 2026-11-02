#pragma once

#include "src/core/kakusan/model/player_events.hpp"
#include "src/core/net/kakusan/control_message.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <variant>

namespace kakusan_test {

template <class Effects>
class PlayerEffects {
 public:
  using SendControl = libchiaking::kakusan::SendControl;
  using ShowState = libchiaking::kakusan::ShowState;
  using Message = libchiaking::net::kakusan::control::Message;

  template <class T>
  static auto count(const Effects& effects, std::size_t from = 0)
      -> std::size_t {
    std::size_t total = 0;
    for (std::size_t index = from; index < effects.size(); ++index) {
      if (std::holds_alternative<T>(effects[index])) ++total;
    }
    return total;
  }

  template <class T>
  static auto first(const Effects& effects, std::size_t from = 0) -> const T* {
    for (std::size_t index = from; index < effects.size(); ++index) {
      if (const auto* item = std::get_if<T>(&effects[index])) return item;
    }
    return nullptr;
  }

  template <class T>
  static auto the(const Effects& effects, std::size_t from = 0) -> const T& {
    const auto* item = first<T>(effects, from);
    EXPECT_NE(item, nullptr) << "no such effect";
    if (item != nullptr) return *item;
    static const T fallback{};
    return fallback;
  }

  static auto code_shown(const Effects& effects) -> std::string {
    const auto* item = first<libchiaking::kakusan::DisplayCode>(effects);
    EXPECT_NE(item, nullptr) << "no code shown";
    if (item == nullptr) return {};
    return std::string{item->code.view()};
  }

  template <class M>
  static auto sent(const Effects& effects) -> const SendControl* {
    for (const auto& effect : effects) {
      if (const auto* item = std::get_if<SendControl>(&effect)) {
        if (std::holds_alternative<M>(item->message)) return item;
      }
    }
    return nullptr;
  }

  template <class M>
  static auto count_sent(const Effects& effects) -> std::size_t {
    std::size_t total = 0;
    for (const auto& effect : effects) {
      if (const auto* item = std::get_if<SendControl>(&effect)) {
        if (std::holds_alternative<M>(item->message)) ++total;
      }
    }
    return total;
  }

  template <class M>
  static auto message(const Effects& effects) -> const M& {
    const auto* item = sent<M>(effects);
    EXPECT_NE(item, nullptr) << "no such message sent";
    if (item != nullptr) return std::get<M>(item->message);
    static const M fallback{};
    return fallback;
  }

  static auto shown(const Effects& effects) -> ShowState {
    std::optional<ShowState> last;
    for (const auto& effect : effects) {
      if (const auto* item = std::get_if<ShowState>(&effect)) last = *item;
    }
    EXPECT_TRUE(last.has_value()) << "no state shown";
    return last.value_or(ShowState{});
  }

  static auto terminal(const Effects& effects) -> const SendControl* {
    for (const auto& effect : effects) {
      if (const auto* item = std::get_if<SendControl>(&effect);
          item && item->terminal)
        return item;
    }
    return nullptr;
  }

  template <class M>
  static auto closing(const Effects& effects) -> const M& {
    const auto* item = terminal(effects);
    const M* found = item == nullptr ? nullptr : std::get_if<M>(&item->message);
    EXPECT_NE(found, nullptr) << "no such terminal message";
    if (found != nullptr) return *found;
    static const M fallback{};
    return fallback;
  }

  static auto failure_sent(const Effects& effects)
      -> std::optional<libchiaking::net::kakusan::Error> {
    const auto* item = terminal(effects);
    if (item == nullptr) return std::nullopt;
    const auto* failure =
        std::get_if<libchiaking::net::kakusan::control::Failure>(
            &item->message);
    EXPECT_NE(failure, nullptr) << "the terminal message is not a failure";
    if (failure == nullptr) return std::nullopt;
    return failure->code;
  }

  template <class M>
  static auto is(const Message& item) -> bool {
    return std::holds_alternative<M>(item);
  }
};

}  // namespace kakusan_test
