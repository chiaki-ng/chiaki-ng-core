#pragma once

#include "src/core/play/adapters/links/link_failure.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace play_test::link_failures {

class Failures {
 public:
  static auto contains(std::string_view text, std::string_view needle)
      -> testing::AssertionResult {
    if (text.find(needle) != std::string_view::npos)
      return testing::AssertionSuccess()
             << '"' << text << "\" holds \"" << needle << '"';
    return testing::AssertionFailure()
           << '"' << text << "\" lacks \"" << needle << '"';
  }

  static auto at(libchiaking::play::Venue venue,
                 libchiaking::play::LinkFailure failure)
      -> libchiaking::play::LinkFailure {
    failure.reached_through(venue);
    return failure;
  }

  static auto legacy_cloud_stream() -> libchiaking::play::Venue {
    return libchiaking::play::Venue::cloud(
        libchiaking::catalog::ServiceGeneration::old_service);
  }

  static auto cloud_stream() -> libchiaking::play::Venue {
    return libchiaking::play::Venue::cloud(
        libchiaking::catalog::ServiceGeneration::ps5_cloud);
  }
};

}  // namespace play_test::link_failures
