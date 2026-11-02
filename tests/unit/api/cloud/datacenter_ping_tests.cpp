#include "src/core/psn/gk/datacenter_ping.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace cloud_api_test {
namespace {

using libchiaking::cloud::DatacenterPing;
using namespace std::string_view_literals;

static_assert(DatacenterPing::prior_report_limit == 64U * 1024U);
static_assert(DatacenterPing::report_limit == 12U * 1024U);

class DatacenterPingReport : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto made = DatacenterPing::report(one);
    ASSERT_TRUE(made);
    report = *made;
  }

  const std::vector<DatacenterPing> pings{
      {"sjc", 21, {20, 22}, 1454, 1254, 9296, "203.0.113.7", 30000, true},
      {"fra", 999, {}, 0, 0, 9296, "203.0.113.9", 0, false},
  };
  const std::vector<DatacenterPing> one{
      {"sjc", 21, {20, 22}, 1454, 1254, 9296, "203.0.113.7", 30000, true}};
  std::string report;
};

TEST_F(DatacenterPingReport, WritesEveryMeasuredField) {
  const auto written = DatacenterPing::report(pings);
  ASSERT_TRUE(written);
  EXPECT_NE(
      written->find(
          R"("dataCenter":"sjc","rtt":21,"rtts":[20,22],"mtu_in":1454,"mtu_out":1254,"port":9296,"publicIp":"203.0.113.7","maxBandwidth":30000,"measured":true)"),
      std::string::npos)
      << *written;
  EXPECT_NE(
      written->find(
          R"({"dataCenter":"fra","rtt":999,"rtts":[],"mtu_in":0,"mtu_out":0,"port":9296,"publicIp":"203.0.113.9","maxBandwidth":0,"measured":false})"),
      std::string::npos)
      << *written;
}

TEST_F(DatacenterPingReport, RoundTripsThroughItsParser) {
  const auto written = DatacenterPing::report(pings);
  ASSERT_TRUE(written);
  EXPECT_EQ(DatacenterPing::parse_report(*written), pings);
}

TEST_F(DatacenterPingReport, WritesAnUnmeasuredPingLiterally) {
  const std::vector<DatacenterPing> unmeasured{
      {"fra", 999, {}, 0, 0, 9296, "", 0, false}};
  const auto literal = DatacenterPing::report(unmeasured);
  ASSERT_TRUE(literal);
  EXPECT_EQ(
      *literal,
      R"([{"dataCenter":"fra","rtt":999,"rtts":[],"mtu_in":0,"mtu_out":0,"port":9296,"publicIp":"","maxBandwidth":0,"measured":false}])");
}

TEST_F(DatacenterPingReport, IsNotWrittenEmptyOrPastItsLimit) {
  EXPECT_FALSE(DatacenterPing::report({}));
  EXPECT_FALSE(DatacenterPing::report(pings, 16));
}

TEST_F(DatacenterPingReport, ParsesNothingFromBrokenOrIncompleteJson) {
  EXPECT_TRUE(DatacenterPing::parse_report("{not json").empty());
  EXPECT_TRUE(DatacenterPing::parse_report(R"({"dataCenter":"sjc"})").empty());
}

TEST_F(DatacenterPingReport, AcceptsAnyPriorReportWithinItsBound) {
  EXPECT_TRUE(DatacenterPing::accepts_report(report));
  EXPECT_TRUE(DatacenterPing::accepts_report(""));
  EXPECT_TRUE(DatacenterPing::accepts_report("not json"));
  EXPECT_TRUE(DatacenterPing::accepts_report("[]"sv));
  EXPECT_TRUE(DatacenterPing::parse_report("[]"sv).empty());
}

TEST_F(DatacenterPingReport, RefusesAPriorReportWithAnEmbeddedNul) {
  EXPECT_FALSE(DatacenterPing::accepts_report("[]\0[]"sv));
  EXPECT_TRUE(DatacenterPing::parse_report("[]\0[]"sv).empty());
  auto trailing = report;
  trailing.push_back('\0');
  EXPECT_FALSE(DatacenterPing::accepts_report(trailing));
  EXPECT_TRUE(DatacenterPing::parse_report(trailing).empty());
  auto leading = report;
  leading.insert(leading.begin() + 1, '\0');
  EXPECT_FALSE(DatacenterPing::accepts_report(leading));
  EXPECT_TRUE(DatacenterPing::parse_report(leading).empty());
}

TEST_F(DatacenterPingReport, AcceptsAPriorReportUpToSixtyFourKibibytes) {
  auto padded = report;
  padded.append(DatacenterPing::prior_report_limit - padded.size(), ' ');
  ASSERT_EQ(padded.size(), 65536U);
  EXPECT_TRUE(DatacenterPing::accepts_report(padded));
  EXPECT_EQ(DatacenterPing::parse_report(padded), one);
  padded.push_back(' ');
  ASSERT_EQ(padded.size(), 65537U);
  EXPECT_FALSE(DatacenterPing::accepts_report(padded));
  EXPECT_TRUE(DatacenterPing::parse_report(padded).empty());
  EXPECT_FALSE(
      DatacenterPing::accepts_report(std::string(64U * 1024U + 1U, 'x')));
  EXPECT_TRUE(DatacenterPing::accepts_report(std::string(64U * 1024U, 'x')));
}

}  // namespace
}  // namespace cloud_api_test
