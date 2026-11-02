#include "src/core/consoles/adapters/search/probe_diagnostics.hpp"
#include "tests/unit/core/consoles/adapters/search/rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::consoles::Beacon;
using libchiaking::consoles::ProbeDiagnostics;

class ProbeDiagnosticsLog : public ::testing::Test {
 protected:
  void fail_two_hundred_probes() {
    probes.start(0, 1, 1024, 987, 9302);
    for (unsigned index = 0; index < 200; ++index)
      probes.sent(
          {SearchRig::ip(10, 0, 0, static_cast<std::uint8_t>(index)), 987},
          Beacon::ProbeFailure::network);
  }

  test_support::FakeLog output;
  ProbeDiagnostics probes{&output};
};

TEST_F(ProbeDiagnosticsLog, AnnouncesTheScanOnStart) {
  EXPECT_FALSE(probes.started());
  probes.start(0, 1, 1024, 987, 9302);
  EXPECT_TRUE(probes.started());
  ASSERT_EQ(output.messages.size(), 1U);
  EXPECT_EQ(
      output.messages.front(),
      "Scanning 1 IPv4 interfaces; 1024 unicast targets; PS4 UDP 987, PS5 UDP "
      "9302; "
      "broadcast every 1 s, unicast batches of 64 every 200 ms then every 15 "
      "s");
}

TEST_F(ProbeDiagnosticsLog, BoundsDistinctProbeErrors) {
  fail_two_hundred_probes();
  ASSERT_EQ(output.messages.size(), 66U);
  EXPECT_EQ(output.messages[1],
            "Probe to 10.0.0.0:987 failed: network failure");
  EXPECT_EQ(output.messages.back(),
            "Further distinct probe errors are counted in silence summaries");
}

TEST_F(ProbeDiagnosticsLog, LogsNothingForRepeatsSuccessesOrReceipts) {
  fail_two_hundred_probes();
  probes.sent({SearchRig::ip(10, 0, 0, 0), 987}, Beacon::ProbeFailure::network);
  probes.sent({SearchRig::ip(10, 0, 0, 0), 987}, std::nullopt);
  EXPECT_EQ(output.messages.size(), 66U);
  probes.received(false);
  probes.received(true);
  probes.silence(9'999'999, 0);
  EXPECT_EQ(output.messages.size(), 66U);
}

TEST_F(ProbeDiagnosticsLog, SummarisesSilenceAndThrottlesTheSummaries) {
  fail_two_hundred_probes();
  probes.sent({SearchRig::ip(10, 0, 0, 0), 987}, Beacon::ProbeFailure::network);
  probes.sent({SearchRig::ip(10, 0, 0, 0), 987}, std::nullopt);
  probes.received(false);
  probes.received(true);
  probes.silence(9'999'999, 0);
  probes.silence(10'000'000, 0);
  EXPECT_EQ(
      output.messages.back(),
      "No console has answered after 10 s; sent 202 probes; 201 failed sends; "
      "received 2 packets, 1 unparsed");
  probes.silence(59'999'999, 0);
  EXPECT_EQ(output.messages.size(), 67U);
  probes.silence(60'000'000, 0);
  EXPECT_EQ(output.messages.size(), 68U);
  probes.silence(360'000'000, 1);
  EXPECT_EQ(output.messages.size(), 68U);
  probes.silence(660'000'000, 0);
  EXPECT_EQ(output.messages.size(), 69U);
}

TEST_F(ProbeDiagnosticsLog, NamesEachFailureReason) {
  probes.sent({SearchRig::ip(10, 0, 0, 1), 9302},
              Beacon::ProbeFailure::permission_denied);
  probes.sent({SearchRig::ip(10, 0, 0, 1), 9302},
              Beacon::ProbeFailure::short_send);
  EXPECT_EQ(output.messages,
            (std::vector<std::string>{
                "Probe to 10.0.0.1:9302 failed: permission denied",
                "Probe to 10.0.0.1:9302 failed: short send"}));
}

}  // namespace
}  // namespace consoles_test
