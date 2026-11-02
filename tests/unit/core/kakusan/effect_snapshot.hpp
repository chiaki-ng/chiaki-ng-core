#pragma once

#include "src/core/net/kakusan/protocol_error.hpp"
#include "src/core/kakusan/model/player_events.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/utils/memory/constant_time.hpp"
#include "src/core/utils/timing/deadline.hpp"
#include "tests/support/kakusan/messages.hpp"

#include <type_traits>
#include <variant>

namespace kakusan_test {

class Effects {
 public:
  template <class Effect>
  static auto snapshot(const Effect& effect) -> Effect {
    return std::visit(
        [](const auto& item) -> Effect {
          if constexpr (std::is_same_v<std::remove_cvref_t<decltype(item)>,
                                       libchiaking::kakusan::DisplayCode>)
            return libchiaking::kakusan::DisplayCode{item.code.clone()};
          else if constexpr (std::is_same_v<std::remove_cvref_t<decltype(item)>,
                                            libchiaking::kakusan::SendControl>)
            return libchiaking::kakusan::SendControl{
                test_support::kakusan::Messages::clone(item.message),
                item.token, item.terminal};
          else if constexpr (std::is_same_v<
                                 std::remove_cvref_t<decltype(item)>,
                                 libchiaking::kakusan::ConfigureReceiver>)
            return Effect{
                std::in_place_type<std::remove_cvref_t<decltype(item)>>,
                item.setup.clone()};
          else
            return item;
        },
        effect);
  }
};

}  // namespace kakusan_test
