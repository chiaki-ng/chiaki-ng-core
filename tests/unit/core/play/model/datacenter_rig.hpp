#pragma once

#include <libchiaking/play_types.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace play_test {

class DatacenterRig : public ::testing::Test {
 protected:
  using Time = libchiaking::play::DatacenterMeasurement::Time;
  using Probe = libchiaking::play::DatacenterMeasurement::Probe;

  static auto name(std::string_view text) -> libchiaking::play::DatacenterName {
    const auto parsed = libchiaking::play::DatacenterName::parse(text);
    EXPECT_TRUE(parsed) << text;
    return parsed.value();
  }

  static auto at(int seconds) -> Time {
    return Time{std::chrono::seconds{seconds}};
  }

  static auto measured(std::string_view where, int round_trip, int when = 0,
                       Probe probe = {})
      -> libchiaking::play::DatacenterMeasurement {
    const auto measurement = libchiaking::play::DatacenterMeasurement::create(
        name(where), std::chrono::milliseconds{round_trip}, at(when), probe);
    EXPECT_TRUE(measurement) << where;
    return measurement.value();
  }

  static auto names(const libchiaking::play::KnownDatacenters& known)
      -> std::vector<std::string> {
    std::vector<std::string> result;
    for (const auto& measurement : known.all())
      result.push_back(measurement.name().text());
    return result;
  }

  static auto names(const libchiaking::play::DatacenterOffer& offer)
      -> std::vector<std::string> {
    std::vector<std::string> result;
    for (const auto& entry : offer.entries())
      result.push_back(entry.name().text());
    return result;
  }
};

}  // namespace play_test
