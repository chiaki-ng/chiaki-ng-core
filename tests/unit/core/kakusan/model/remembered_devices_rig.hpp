#pragma once

#include "libchiaking/kakusan_types.hpp"
#include "libchiaking/testing/service_builder.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/kakusan/fakes.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

namespace kakusan_test {

class RememberedDevicesTest : public ::testing::Test {
 protected:
  class Book {
   public:
    auto open() -> libchiaking::kakusan::Kakusan {
      return libchiaking::testing::ServiceBuilder::on<
          libchiaking::kakusan::Kakusan>(
          Neighbourhood::surroundings(machine, random, clock.source(), store));
    }

    Lan lan;
    Machine machine{lan, 1};
    test_support::SeededRandom random{9};
    VirtualClock clock;
    MemoryStore store;
  };

  static auto key_of(std::uint8_t tag) -> libchiaking::kakusan::IdentityKey {
    return TestPlatform::identity(tag).key();
  }

  static auto at(int seconds) -> std::chrono::system_clock::time_point {
    return std::chrono::system_clock::time_point{std::chrono::seconds{seconds}};
  }

  static auto saved(std::uint8_t tag, std::string name)
      -> libchiaking::kakusan::SavedDevice {
    return libchiaking::kakusan::SavedDevice{key_of(tag), std::move(name),
                                             at(10), at(20)};
  }
};

}  // namespace kakusan_test
