#include "libchiaking/detail/diagnostics/log_queue.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/utils/diagnostics/trace.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <optional>

namespace diagnostics_test {
namespace {

using libchiaking::diagnostics::LogQueue;
using libchiaking::diagnostics::Trace;
using libchiaking::platform::LogSeverity;

class TraceTest : public ::testing::Test {
 protected:
  enum class Error { synthetic = 7 };
  enum class CalibrationFault { invalid_configuration, timeout = 8 };
  enum class OfferWait { idle, awaiting_offer, awaiting_result };

  auto next_line() -> std::optional<LogQueue::Record> { return queue.read(); }

  const libchiaking::platform::SteadyClock monotonic;
  test_support::FakeClock clock{0};
  LogQueue queue{monotonic};
  Trace trace{&queue, clock, libchiaking::diagnostics::Component::cloud};
};

TEST_F(TraceTest, LogsTheBeginningAndAFailedOutcomeOfACall) {
  auto result = trace.call(libchiaking::diagnostics::Operation::cloud_allocate,
                           [this]() -> libchiaking::Expected<int, Error> {
                             clock.now_us += 345;
                             return libchiaking::unexpected(Error::synthetic);
                           });
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), Error::synthetic);
  auto record = next_line();
  ASSERT_TRUE(record);
  EXPECT_TRUE(std::strstr(record->c_str(), "outcome=begin")) << record->c_str();
  record = next_line();
  ASSERT_TRUE(record);
  EXPECT_EQ(record->severity(), LogSeverity::error);
  EXPECT_TRUE(
      std::strstr(record->c_str(), "outcome=failed value=7 duration_us=345"))
      << record->c_str();
  EXPECT_FALSE(next_line());
}

TEST_F(TraceTest, ExplainsARetainedRouteByTheCalibrationFault) {
  trace.decision(Trace::Decision::retained_calibration_route,
                 std::optional{CalibrationFault::timeout});
  const auto record = next_line();
  ASSERT_TRUE(record);
  EXPECT_TRUE(std::strstr(record->c_str(), "because calibration failed"))
      << record->c_str();
  EXPECT_TRUE(std::strstr(record->c_str(), "detail=8 reason=timeout"))
      << record->c_str();
}

TEST_F(TraceTest, NamesAnUnknownCalibrationFault) {
  trace.decision(Trace::Decision::fresh_calibration_route,
                 std::optional<CalibrationFault>{});
  const auto record = next_line();
  ASSERT_TRUE(record);
  EXPECT_TRUE(std::strstr(record->c_str(), "because calibration failed"))
      << record->c_str();
  EXPECT_TRUE(std::strstr(record->c_str(), "reason=unknown"))
      << record->c_str();
}

TEST_F(TraceTest, NamesTheCalibrationFaultOfAFreshRoute) {
  trace.decision(Trace::Decision::fresh_calibration_route,
                 std::optional{CalibrationFault::invalid_configuration});
  const auto record = next_line();
  ASSERT_TRUE(record);
  EXPECT_TRUE(std::strstr(record->c_str(), "reason=invalid_configuration"))
      << record->c_str();
}

TEST_F(TraceTest, LogsADecisionDetail) {
  trace.decision(Trace::Decision::lock_waiting, 3);
  const auto record = next_line();
  ASSERT_TRUE(record);
  EXPECT_TRUE(std::strstr(record->c_str(), "detail=3 reason=described above"))
      << record->c_str();
}

TEST_F(TraceTest, NamesARecordedState) {
  trace.record_state(libchiaking::diagnostics::Operation::offer_state,
                     OfferWait::awaiting_result);
  const auto record = next_line();
  ASSERT_TRUE(record);
  EXPECT_TRUE(std::strstr(record->c_str(), "Holepunch OFFER FSM observed"))
      << record->c_str();
  EXPECT_TRUE(std::strstr(record->c_str(),
                          "value=2 duration_us=0 state=awaiting_result"))
      << record->c_str();
}

}  // namespace
}  // namespace diagnostics_test
