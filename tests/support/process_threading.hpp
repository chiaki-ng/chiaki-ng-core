#pragma once

#include <libchiaking/platform/runtime/threading.hpp>

namespace test_support {

inline auto process_threading()
    -> libchiaking::platform::DefaultThreadingProvider& {
  static auto* const threading =
      new libchiaking::platform::DefaultThreadingProvider;
  return *threading;
}

}  // namespace test_support
