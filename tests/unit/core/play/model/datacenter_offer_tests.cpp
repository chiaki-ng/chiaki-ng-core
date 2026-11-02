#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/model/datacenter_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace play_test {
namespace {

using libchiaking::play::DatacenterOffer;
using std::chrono::milliseconds;

class DatacenterOffers : public DatacenterRig {
 protected:
  static auto mixed() -> DatacenterOffer {
    const std::vector<DatacenterOffer::Probe> probes{
        {"silent", milliseconds{0}, true},
        {"far", milliseconds{140}, true},
        {"unmeasured", milliseconds{12}, false},
        {"near", milliseconds{18}, true},
        {"Auto", milliseconds{1}, true},
        {"", milliseconds{1}, true},
        {"bad\nname", milliseconds{1}, true},
        {"near", milliseconds{2}, true},
        {"tie", milliseconds{18}, true},
        {"edge", milliseconds{100}, true},
        {"over", milliseconds{101}, true},
        {"negative", milliseconds{-3}, true},
    };
    return DatacenterOffer::from(probes);
  }

  static auto unanswered() -> std::vector<DatacenterOffer::Probe> {
    return {{"b", milliseconds{0}, false}, {"a", milliseconds{0}, false}};
  }
};

TEST_F(DatacenterOffers, AreOrderedByAnswerThenLatency) {
  const auto offer = mixed();
  EXPECT_EQ(names(offer),
            (std::vector<std::string>{"near", "tie", "edge", "over", "far",
                                      "silent", "unmeasured", "negative"}));
  EXPECT_EQ(offer.wait(), DatacenterOffer::default_wait);
  EXPECT_FALSE(offer.empty());
}

TEST_F(DatacenterOffers, NameTheFirstFastestAsLowest) {
  const auto offer = mixed();
  const auto* lowest = offer.lowest();
  ASSERT_NE(lowest, nullptr);
  EXPECT_EQ(lowest->name(), "near");
  EXPECT_EQ(lowest->round_trip(), milliseconds{18});
  EXPECT_TRUE(offer.is_lowest(*lowest));
  const auto* tie = offer.find(name("tie"));
  ASSERT_NE(tie, nullptr);
  EXPECT_FALSE(offer.is_lowest(*tie));
}

TEST_F(DatacenterOffers, MarkHighLatencyAbove100Milliseconds) {
  const auto offer = mixed();
  const auto* near = offer.find(name("near"));
  const auto* edge = offer.find(name("edge"));
  const auto* over = offer.find(name("over"));
  const auto* far = offer.find(name("far"));
  ASSERT_NE(near, nullptr);
  ASSERT_NE(edge, nullptr);
  ASSERT_NE(over, nullptr);
  ASSERT_NE(far, nullptr);
  EXPECT_FALSE(near->is_high_latency());
  EXPECT_TRUE(near->answered());
  EXPECT_FALSE(edge->is_high_latency());
  EXPECT_TRUE(over->is_high_latency());
  EXPECT_TRUE(far->is_high_latency());
  EXPECT_EQ(DatacenterOffer::high_latency_threshold, milliseconds{100});
}

TEST_F(DatacenterOffers, TreatAMissingOrBadRoundTripAsNoAnswer) {
  const auto offer = mixed();
  const auto* silent = offer.find(name("silent"));
  ASSERT_NE(silent, nullptr);
  EXPECT_FALSE(silent->answered());
  EXPECT_FALSE(silent->round_trip());
  EXPECT_TRUE(silent->is_high_latency());
  const auto* unmeasured = offer.find(name("unmeasured"));
  const auto* negative = offer.find(name("negative"));
  ASSERT_NE(unmeasured, nullptr);
  ASSERT_NE(negative, nullptr);
  EXPECT_FALSE(unmeasured->answered());
  EXPECT_FALSE(negative->answered());
}

TEST_F(DatacenterOffers, OfferOnlyTheNamesTheyHold) {
  const auto offer = mixed();
  EXPECT_TRUE(offer.offers(name("far")));
  EXPECT_FALSE(offer.offers(name("elsewhere")));
}

TEST_F(DatacenterOffers, HaveNoLowestWithoutAnswers) {
  const auto offer = DatacenterOffer::from(unanswered(), milliseconds{30000});
  EXPECT_EQ(names(offer), (std::vector<std::string>{"b", "a"}));
  EXPECT_EQ(offer.lowest(), nullptr);
  ASSERT_FALSE(offer.entries().empty());
  EXPECT_FALSE(offer.is_lowest(offer.entries().front()));
  EXPECT_EQ(offer.wait(), milliseconds{30000});
}

TEST_F(DatacenterOffers, WaitAtLeastASecond) {
  EXPECT_EQ(DatacenterOffer::from(unanswered(), milliseconds{0}).wait(),
            DatacenterOffer::minimum_wait);
  EXPECT_EQ(DatacenterOffer::from(unanswered(), milliseconds{-5}).wait(),
            milliseconds{1000});
}

TEST_F(DatacenterOffers, AreEmptyWithoutProbes) {
  EXPECT_TRUE(DatacenterOffer{}.empty());
  EXPECT_EQ(DatacenterOffer{}.lowest(), nullptr);
  EXPECT_EQ(DatacenterOffer::from({}), DatacenterOffer{});
}

}  // namespace
}  // namespace play_test
