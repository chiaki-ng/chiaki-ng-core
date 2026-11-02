#include "libchiaking/platform/net/default.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/cloud/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cloud_test {
namespace {

using Reason = gk::Error::Reason;
using Stage = gk::Error::Stage;
using Measured = libchiaking::Expected<std::vector<gk::Ping>, gk::Error>;
using Survey = play::ServerReservation::Survey;

class StageProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    EXPECT_EQ(request.headers.size(), std::size_t{5});
    const auto key = request.headers.size() > 4
                         ? request.headers[4].value.view()
                         : std::string_view{};
    if (calls == 1) {
      EXPECT_NE(request.url.view().find("/datacenters"), std::string::npos);
      EXPECT_EQ(request.url.view().find("/datacenters/select"),
                std::string::npos);
      EXPECT_EQ(key, "initial-key");
    } else {
      EXPECT_EQ(calls, 2U);
      EXPECT_NE(request.url.view().find("/datacenters/select"),
                std::string::npos);
      EXPECT_EQ(key, "rotated-key");
    }
    platform::HttpResponse response;
    response.status_code = calls == 1 ? first_status : second_status;
    if (calls == 1)
      response.headers.push_back(
          {"X-Gaikai-Session", libchiaking::SecretString{"rotated-key"}});
    const std::string_view body =
        calls == 1
            ? R"([{"dataCenter":"dc1","publicIp":"","port":2053,"maxBandwidth":20000}])"
            : R"({"port":8975})";
    for (char character : body)
      response.body.push_back(static_cast<std::byte>(character));
    return response;
  }

  std::uint16_t first_status{200};
  std::uint16_t second_status{200};
  unsigned calls{};
};

class TimingOutPicker final : public play::LaunchObserver {
 public:
  void on_progress(const play::LaunchProgress&,
                   libchiaking::net::protocol::remote_play::FailureStage,
                   std::string_view) override {}

  void on_datacenter_selected(const gk::Ping& row, bool forced) override {
    selected = row.name;
    selected_forced = forced;
  }

  auto selects_datacenter_at_launch() const noexcept -> bool override {
    return true;
  }

  void offer_datacenters(const std::vector<std::string>& names) override {
    offered = names;
  }

  auto await_datacenter_choice(const platform::StopToken&)
      -> play::LaunchChoice override {
    ++waits;
    return {play::LaunchChoice::Kind::timed_out, {}};
  }

  std::vector<std::string> offered;
  std::string selected;
  bool selected_forced{true};
  unsigned waits{};
};

class LockedReservation {
 public:
  LockedReservation(platform::HttpProvider& provider,
                    test_support::FakeWaiter& waiter,
                    play::LaunchObserver* observer = nullptr,
                    CloudService service = CloudService::cloud_stream)
      : harness_{provider, waiter, seed(service),
                 std::make_shared<test_support::StopState>(), observer} {}

  auto session() -> play::ServerReservation& { return harness_.session(); }
  auto key() -> std::string_view {
    return harness_.session().config_key().view();
  }
  void stop() noexcept { harness_.stop(); }

 private:
  static auto seed(CloudService service) -> SessionSeed {
    SessionSeed locked;
    locked.config_key = "initial-key";
    locked.service = service;
    locked.until = SessionSeed::Until::locked;
    return locked;
  }

  SessionHarness harness_;
};

class ReservationDatacenterStage : public ::testing::Test {
 protected:
  void SetUp() override { waiter.answer = false; }

  auto successful_measure() -> Survey {
    return [this](const std::vector<gk::Datacenter>& rows, CloudService service,
                  const libchiaking::SecretString& used_lock) -> Measured {
      ++measures;
      EXPECT_EQ(rows.size(), std::size_t{1});
      if (!rows.empty()) {
        EXPECT_EQ(rows[0].name, "dc1");
      }
      EXPECT_EQ(service, CloudService::cloud_stream);
      EXPECT_EQ(used_lock.view(), "initial-key");
      gk::Ping row;
      row.name = "dc1";
      row.rtt_ms = 20;
      row.mtu_in = 1454;
      row.mtu_out = 1254;
      row.port = 2053;
      row.measured = true;
      return std::vector<gk::Ping>{std::move(row)};
    };
  }

  static auto ping_row(std::string name, int rtt, bool measured) -> gk::Ping {
    gk::Ping row;
    row.name = std::move(name);
    row.rtt_ms = rtt;
    row.rtts_ms = {rtt};
    row.measured = measured;
    return row;
  }

  auto unreachable(std::vector<gk::Ping> rows) -> std::optional<int> {
    StageProvider provider;
    LockedReservation session{provider, waiter};
    const Survey measure =
        [&rows](const std::vector<gk::Datacenter>&, CloudService,
                const libchiaking::SecretString&) -> Measured { return rows; };
    const auto chosen =
        session.session().choose_datacenter(measure, "Auto", {});
    EXPECT_TRUE(GkRig::fails_for(chosen, Reason::no_reachable_datacenter));
    if (chosen) return std::nullopt;
    return chosen.error().best_rtt_ms();
  }

  platform::SteadyClock steady;
  platform::DefaultNetworkProvider network{test_support::process_threading(),
                                           steady};
  test_support::FakeClock clock{1'000};
  test_support::FixedRandom random;
  test_support::FakeWaiter waiter;
  const play::ServerReservation::Probes probes{
      network, clock, test_support::process_threading(), random, waiter};
  unsigned measures{};
};

TEST_F(ReservationDatacenterStage, SelectsAForcedDatacenterWithoutMeasuring) {
  unsigned forced_measure_calls{};
  const Survey forced_measure =
      [&](const std::vector<gk::Datacenter>&, CloudService,
          const libchiaking::SecretString&) -> Measured {
    ++forced_measure_calls;
    return std::vector<gk::Ping>{};
  };
  StageProvider forced;
  LockedReservation session{forced, waiter};
  const auto result =
      session.session().choose_datacenter(forced_measure, "dc1", {});
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().selected.name, "dc1");
  EXPECT_EQ(result.value().selected.selected_port, 8975);
  EXPECT_EQ(result.value().picker_rows.size(), std::size_t{1});
  EXPECT_EQ(forced.calls, 2U);
  EXPECT_EQ(waiter.calls(), 0);
  EXPECT_EQ(forced_measure_calls, 0U);
  EXPECT_EQ(session.key(), "rotated-key");
}

TEST_F(ReservationDatacenterStage, MeasuresWithTheLockKeyAndSelectsTheFastest) {
  StageProvider auto_success;
  LockedReservation session{auto_success, waiter};
  const auto result =
      session.session().choose_datacenter(successful_measure(), "Auto", {});
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().selected.name, "dc1");
  EXPECT_EQ(result.value().selected.selected_port, 8975);
  ASSERT_EQ(result.value().picker_rows.size(), std::size_t{1});
  EXPECT_TRUE(result.value().picker_rows[0].measured);
  EXPECT_EQ(auto_success.calls, 2U);
  EXPECT_EQ(measures, 1U);
  EXPECT_EQ(session.key(), "rotated-key");
}

TEST_F(ReservationDatacenterStage, MeasuresForALaunchPickThatTimesOut) {
  StageProvider launch_pick;
  TimingOutPicker picker;
  LockedReservation session{launch_pick, waiter, &picker};
  const auto result = session.session().choose_datacenter(successful_measure(),
                                                          "dc-preset", {});
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().selected.name, "dc1");
  EXPECT_EQ(measures, 1U);
  EXPECT_EQ(picker.waits, 1U);
  EXPECT_EQ(picker.offered, std::vector<std::string>{"dc1"});
  EXPECT_EQ(picker.selected, "dc1");
  EXPECT_FALSE(picker.selected_forced);
}

TEST_F(ReservationDatacenterStage, MeasuresWithoutAPreference) {
  StageProvider empty_auto;
  LockedReservation session{empty_auto, waiter};
  EXPECT_TRUE(
      session.session().choose_datacenter(successful_measure(), "", {}));
  EXPECT_EQ(empty_auto.calls, 2U);
  EXPECT_EQ(measures, 1U);
}

TEST_F(ReservationDatacenterStage, ReportsAFailedMeasurementAtThePingStage) {
  StageProvider measure_failure;
  LockedReservation session{measure_failure, waiter};
  const Survey failing_measure =
      [](const std::vector<gk::Datacenter>&, CloudService,
         const libchiaking::SecretString&) -> Measured {
    return libchiaking::unexpected(
        gk::Error{Stage::ping, Reason::probe_failed});
  };
  EXPECT_TRUE(GkRig::failed(
      session.session().choose_datacenter(failing_measure, "Auto", {}),
      Stage::ping, Reason::probe_failed, 2));
  EXPECT_EQ(measure_failure.calls, 1U);
}

TEST_F(ReservationDatacenterStage, ProbesNothingReachableForAnUnresolvedHost) {
  StageProvider auto_rejected;
  LockedReservation session{auto_rejected, waiter, nullptr,
                            CloudService::legacy_cloud_stream};
  EXPECT_TRUE(
      GkRig::failed(session.session().choose_datacenter(probes, "Auto", {}),
                    Stage::select, Reason::no_reachable_datacenter, 4));
  EXPECT_EQ(auto_rejected.calls, 1U);
  EXPECT_EQ(waiter.calls(), 0);
  EXPECT_EQ(session.key(), "rotated-key");
}

TEST_F(ReservationDatacenterStage, RefusesAnUnlistedForcedDatacenter) {
  StageProvider unavailable;
  LockedReservation session{unavailable, waiter};
  EXPECT_TRUE(
      GkRig::failed(session.session().choose_datacenter(probes, "missing", {}),
                    Stage::select, Reason::datacenter_unavailable, 3));
  EXPECT_EQ(unavailable.calls, 1U);
  EXPECT_EQ(waiter.calls(), 0);
}

TEST_F(ReservationDatacenterStage, KeepsTheKeyWhenTheListIsRefused) {
  StageProvider failed_fetch;
  failed_fetch.first_status = 500;
  LockedReservation session{failed_fetch, waiter};
  EXPECT_TRUE(
      GkRig::failed(session.session().choose_datacenter(probes, "dc1", {}),
                    Stage::datacenters, Reason::rejected, 1));
  EXPECT_EQ(failed_fetch.calls, 1U);
  EXPECT_EQ(session.key(), "initial-key");
}

TEST_F(ReservationDatacenterStage, KeepsTheListsKeyWhenTheSelectionIsRefused) {
  StageProvider failed_select;
  failed_select.second_status = 500;
  LockedReservation session{failed_select, waiter};
  EXPECT_TRUE(
      GkRig::failed(session.session().choose_datacenter(probes, "dc1", {}),
                    Stage::select, Reason::rejected, 5));
  EXPECT_EQ(failed_select.calls, 2U);
  EXPECT_EQ(session.key(), "rotated-key");
}

TEST_F(ReservationDatacenterStage, FetchesNothingOnceStopped) {
  StageProvider pre_cancel;
  LockedReservation session{pre_cancel, waiter};
  session.stop();
  const auto result = session.session().choose_datacenter(probes, "dc1", {});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().stage(), Stage::datacenters);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(result.error().detail(), 0U);
  EXPECT_EQ(pre_cancel.calls, 0U);
}

TEST_F(ReservationDatacenterStage, ReportsTheBestRoundTripWhenTooSlow) {
  StageProvider slow_auto;
  LockedReservation session{slow_auto, waiter};
  const Survey slow_measure = [](const std::vector<gk::Datacenter>&,
                                 CloudService,
                                 const libchiaking::SecretString&) -> Measured {
    gk::Ping slow;
    slow.name = "dc1";
    slow.rtt_ms = 120;
    slow.rtts_ms = {120};
    slow.measured = true;
    return std::vector<gk::Ping>{std::move(slow)};
  };
  const auto result =
      session.session().choose_datacenter(slow_measure, "Auto", {});
  EXPECT_TRUE(
      GkRig::failed(result, Stage::select, Reason::no_reachable_datacenter, 4));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().best_rtt_ms(), std::optional<int>{120});
}

TEST_F(ReservationDatacenterStage, ReportsTheBestMeasuredRoundTripOnly) {
  EXPECT_FALSE(unreachable({ping_row("dc1", 999, false)}));
  EXPECT_EQ(
      unreachable({ping_row("dc1", 999, false), ping_row("dc3", 125, true),
                   ping_row("dc2", 101, true)}),
      std::optional<int>{101});
}

}  // namespace
}  // namespace cloud_test
