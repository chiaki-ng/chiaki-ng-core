#include <libchiaking/play.hpp>
#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <span>
#include <vector>

namespace play_test {
namespace {

namespace catalog = libchiaking::catalog;
namespace play = libchiaking::play;
using libchiaking::Expected;

class DatacenterStore : public ::testing::Test {
 protected:
  class MemoryStore final : public play::DatacenterStore {
   public:
    auto load(catalog::ServiceGeneration service)
        -> Expected<std::vector<play::DatacenterMeasurement>, Error> override {
      if (broken_) return libchiaking::unexpected(Error::unavailable);
      return kept_[service];
    }

    auto save(catalog::ServiceGeneration service,
              std::span<const play::DatacenterMeasurement> measurements)
        -> Expected<void, Error> override {
      if (broken_) return libchiaking::unexpected(Error::unavailable);
      kept_[service].assign(measurements.begin(), measurements.end());
      return {};
    }

    void break_down() { broken_ = true; }

   private:
    std::map<catalog::ServiceGeneration,
             std::vector<play::DatacenterMeasurement>>
        kept_;
    bool broken_{};
  };

  void SetUp() override {
    const auto name = play::DatacenterName::parse("usw2");
    ASSERT_TRUE(name);
    const auto measurement = play::DatacenterMeasurement::create(
        *name, std::chrono::milliseconds{30},
        play::DatacenterMeasurement::Time{});
    ASSERT_TRUE(measurement);
    known_ = play::KnownDatacenters{{*measurement}};
  }

  auto known() const -> const play::KnownDatacenters& { return known_; }

  MemoryStore store;
  play::DatacenterStore& port = store;

 private:
  play::KnownDatacenters known_;
};

TEST_F(DatacenterStore, KeepsMeasurementsPerService) {
  EXPECT_TRUE(port.save(catalog::ServiceGeneration::ps5_cloud, known().all()));
  const auto old = port.load(catalog::ServiceGeneration::old_service);
  const auto ps5 = port.load(catalog::ServiceGeneration::ps5_cloud);
  ASSERT_TRUE(old);
  EXPECT_TRUE(old.value().empty());
  ASSERT_TRUE(ps5);
  EXPECT_EQ(play::KnownDatacenters{ps5.value()}, known());
}

TEST_F(DatacenterStore, ReportsAnUnavailableStore) {
  ASSERT_TRUE(port.save(catalog::ServiceGeneration::ps5_cloud, known().all()));
  store.break_down();
  EXPECT_FALSE(port.load(catalog::ServiceGeneration::ps5_cloud));
  const auto saved = port.save(catalog::ServiceGeneration::ps5_cloud, {});
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error(), play::DatacenterStore::Error::unavailable);
}

}  // namespace
}  // namespace play_test
