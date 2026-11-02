#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace test_support {

class SetupFailure : public std::runtime_error {
 public:
  explicit SetupFailure(std::string_view what)
      : std::runtime_error(std::string{"test setup failed: "} +
                           std::string{what}) {}
};

class Setup {
 public:
  static void require(bool holds, std::string_view what) {
    if (!holds) throw SetupFailure{what};
  }

  template <class Held>
    requires(!std::is_lvalue_reference_v<Held>)
  static auto value(Held&& held, std::string_view what)
      -> std::remove_cvref_t<decltype(*held)> {
    require(static_cast<bool>(held), what);
    return std::move(*held);
  }
};

}  // namespace test_support
