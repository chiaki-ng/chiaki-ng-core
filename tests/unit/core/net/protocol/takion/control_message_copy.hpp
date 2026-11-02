#pragma once

#include "src/core/net/protocol/takion/detail/control_message.hpp"

#include <variant>

namespace takion_test {

template <class T>
auto copy_of(const T& value) -> T {
  if constexpr (requires { value.clone(); })
    return value.clone();
  else
    return value;
}

inline auto copy_of(
    const libchiaking::net::protocol::takion::ControlMessage& message)
    -> libchiaking::net::protocol::takion::ControlMessage {
  return std::visit(
      [](const auto& value)
          -> libchiaking::net::protocol::takion::ControlMessage {
        return copy_of(value);
      },
      message);
}

}  // namespace takion_test
