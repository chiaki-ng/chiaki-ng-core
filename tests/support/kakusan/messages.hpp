#pragma once

#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "tests/support/bytes.hpp"

#include <cstddef>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace test_support::kakusan {

class Messages {
 public:
  using Message = libchiaking::net::kakusan::control::Message;

  static auto clone(const Message& message) -> Message {
    return std::visit(
        [](const auto& item) -> Message {
          if constexpr (std::is_same_v<
                            std::remove_cvref_t<decltype(item)>,
                            libchiaking::net::kakusan::control::SourceSetup>)
            return item.clone();
          else
            return item;
        },
        message);
  }

  static void append_label(std::vector<std::byte>& output,
                           std::string_view label) {
    Bytes::append(output, libchiaking::net::kakusan::Hash::bytes_of(label));
    output.push_back(std::byte{});
  }
};

}  // namespace test_support::kakusan
