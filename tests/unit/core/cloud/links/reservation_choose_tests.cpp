#include "src/core/play/adapters/datacenters/datacenter_ranking.hpp"
#include "tests/unit/core/cloud/rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
using Kind = play::LaunchChoice::Kind;
namespace progress = libchiaking::progress;

constexpr std::string_view one_datacenter =
    R"([{"dataCenter":"dc1","publicIp":"192.0.2.1","port":2053,"maxBandwidth":20000}])";
constexpr std::string_view two_datacenters =
    R"([{"dataCenter":"dc1","publicIp":"192.0.2.1","port":2053,"maxBandwidth":20000},)"
    R"({"dataCenter":"dc2","publicIp":"192.0.2.2","port":2053,"maxBandwidth":20000}])";

class ChooseProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    EXPECT_GT(request.headers.size(), std::size_t{4});
    if (request.headers.size() > 4) {
      EXPECT_EQ(request.headers[4].value.view(), expected_key);
    }
    platform::HttpResponse response;
    if (request.url.view().find("/select") == std::string::npos) {
      ++fetches;
      EXPECT_NE(request.url.view().find("/datacenters"), std::string::npos);
      response.status_code = 200;
      for (char character : datacenters)
        response.body.push_back(static_cast<std::byte>(character));
      return response;
    }
    ++calls;
    EXPECT_NE(request.url.view().find("/datacenters/select"),
              std::string::npos);
    if (error) return libchiaking::unexpected(*error);
    response.status_code = status;
    if (rotate)
      response.headers.push_back(
          {"X-Gaikai-Session", libchiaking::SecretString{"new-key"}});
    for (char character : std::string_view{R"({"port":8975})"})
      response.body.push_back(static_cast<std::byte>(character));
    return response;
  }

  std::string expected_key{"synthetic-key"};
  std::string_view datacenters{one_datacenter};
  std::uint16_t status{200};
  std::optional<platform::HttpError> error;
  bool rotate{true};
  unsigned calls{};
  unsigned fetches{};
};

class BodyProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    platform::HttpResponse response;
    response.status_code = 200;
    if (request.url.view().find("/select") == std::string::npos) {
      ++fetches;
      EXPECT_NE(request.url.view().find("/datacenters"), std::string::npos);
      for (char character : two_datacenters)
        response.body.push_back(static_cast<std::byte>(character));
      return response;
    }
    ++calls;
    body.assign(request.body.view());
    for (char character : std::string_view{R"({"port":8975})"})
      response.body.push_back(static_cast<std::byte>(character));
    return response;
  }

  std::string body;
  unsigned calls{};
  unsigned fetches{};
};

class Recorder final : public play::LaunchObserver {
 public:
  void on_progress(const play::LaunchProgress&,
                   libchiaking::net::protocol::remote_play::FailureStage,
                   std::string_view) override {}

  void on_datacenters(const std::vector<gk::Ping>& picker,
                      const std::vector<gk::Ping>& measured) override {
    ++calls;
    picker_rows = picker;
    measured_rows = measured;
  }

  void on_datacenter_selected(const gk::Ping& row, bool forced) override {
    ++selections;
    selected = row;
    selected_forced = forced;
  }

  unsigned calls{};
  unsigned selections{};
  gk::Ping selected;
  bool selected_forced{};
  std::vector<gk::Ping> picker_rows;
  std::vector<gk::Ping> measured_rows;
};

class LaunchPicker final : public play::LaunchObserver {
 public:
  void on_progress(const play::LaunchProgress& reported,
                   libchiaking::net::protocol::remote_play::FailureStage,
                   std::string_view text) override {
    progress.emplace_back(text);
    if (reported.activity() ==
        play::LaunchActivity::waiting_for_datacenter_choice)
      waiting = reported;
    else
      EXPECT_FALSE(reported.wait());
  }

  auto datacenter_choice_wait() const noexcept
      -> std::optional<std::chrono::milliseconds> override {
    return choice_wait;
  }

  void on_datacenters(const std::vector<gk::Ping>& picker,
                      const std::vector<gk::Ping>&) override {
    EXPECT_EQ(offered.size(), std::size_t{2});
    picker_rows = picker;
  }

  void on_datacenter_selected(const gk::Ping& row, bool forced) override {
    ++selections;
    selected = row;
    selected_forced = forced;
  }

  auto selects_datacenter_at_launch() const noexcept -> bool override {
    return true;
  }

  void offer_datacenters(const std::vector<std::string>& names) override {
    EXPECT_TRUE(picker_rows.empty());
    offered = names;
  }

  auto await_datacenter_choice(const platform::StopToken&)
      -> play::LaunchChoice override {
    ++waits;
    return choice;
  }

  play::LaunchChoice choice;
  std::optional<std::chrono::milliseconds> choice_wait;
  std::optional<play::LaunchProgress> waiting;
  std::vector<std::string> offered;
  std::vector<std::string> progress;
  std::vector<gk::Ping> picker_rows;
  gk::Ping selected;
  bool selected_forced{};
  unsigned selections{};
  unsigned waits{};
};

class Chosen {
 public:
  libchiaking::Expected<play::DatacenterRanking::Chosen, gk::Error> result;
  std::string key;
  unsigned surveys{};
};

class ReservationChoose : public ::testing::Test {
 protected:
  static auto choose(platform::HttpProvider& provider, std::string_view forced,
                     std::vector<gk::Ping> completed,
                     const std::vector<gk::Ping>& prior,
                     play::LaunchObserver* observer = nullptr,
                     bool stopped = false) -> Chosen {
    auto waiter = GkRig::recording_waiter();
    SessionSeed seed;
    seed.config_key = "synthetic-key";
    seed.until = SessionSeed::Until::locked;
    SessionHarness harness{provider, waiter, seed,
                           std::make_shared<test_support::StopState>(),
                           observer};
    if (stopped) harness.stop();
    unsigned surveys{};
    const play::ServerReservation::Survey survey =
        [&](const std::vector<gk::Datacenter>&, CloudService,
            const libchiaking::SecretString& lock_key)
        -> libchiaking::Expected<std::vector<gk::Ping>, gk::Error> {
      ++surveys;
      EXPECT_EQ(lock_key.view(), "synthetic-key");
      return completed;
    };
    auto result = harness.session().choose_datacenter(survey, forced, prior);
    std::string key{harness.session().config_key().view()};
    return Chosen{std::move(result), std::move(key), surveys};
  }

  static auto failed(const Chosen& chosen, Stage stage, Reason reason,
                     std::uint32_t detail) -> testing::AssertionResult {
    return GkRig::failed(chosen.result, stage, reason, detail);
  }

  static auto ping() -> gk::Ping {
    gk::Ping row;
    row.name = "dc1";
    row.public_ip = "192.0.2.1";
    row.port = 2053;
    row.rtt_ms = 30;
    row.rtts_ms = {30};
    row.mtu_in = 1454;
    row.mtu_out = 1254;
    row.measured = true;
    return row;
  }

  static auto measured(std::string name, int rtt) -> gk::Ping {
    auto row = ping();
    row.name = std::move(name);
    row.rtt_ms = rtt;
    row.rtts_ms = {rtt};
    return row;
  }

  static auto prior() -> gk::Ping {
    gk::Ping row;
    row.name = "dc0";
    row.rtt_ms = 44;
    row.rtts_ms = {44};
    row.measured = true;
    return row;
  }

  static auto launch_pick(play::LaunchChoice choice,
                          std::vector<gk::Ping> pings, LaunchPicker& picker,
                          BodyProvider& provider) -> Chosen {
    picker.choice = std::move(choice);
    auto chosen = choose(provider, "dc1", std::move(pings), {prior()}, &picker);
    EXPECT_EQ(provider.fetches, 1U);
    EXPECT_EQ(chosen.surveys, 1U);
    return chosen;
  }

  ChooseProvider provider;
};

TEST_F(ReservationChoose, ReportsTheRowsButSelectsNothingTooSlow) {
  provider.datacenters = two_datacenters;
  auto slow = ping();
  slow.rtt_ms = 120;
  slow.rtts_ms = {120};
  Recorder rejected;
  const auto selected = choose(provider, "Auto", {slow}, {prior()}, &rejected);
  EXPECT_TRUE(
      failed(selected, Stage::select, Reason::no_reachable_datacenter, 4));
  EXPECT_EQ(rejected.calls, 1U);
  EXPECT_EQ(provider.calls, 0U);
  EXPECT_EQ(rejected.selections, 0U);
  EXPECT_EQ(provider.fetches, 1U);
  EXPECT_EQ(selected.surveys, 1U);
  ASSERT_EQ(rejected.measured_rows.size(), std::size_t{1});
  EXPECT_EQ(rejected.measured_rows[0].rtt_ms, 120);
  ASSERT_EQ(rejected.picker_rows.size(), std::size_t{3});
  EXPECT_EQ(rejected.picker_rows[0].name, "dc1");
  EXPECT_EQ(rejected.picker_rows[0].rtt_ms, 120);
  EXPECT_EQ(rejected.picker_rows[1].name, "dc2");
  EXPECT_FALSE(rejected.picker_rows[1].measured);
  EXPECT_EQ(rejected.picker_rows[1].rtt_ms, 0);
  EXPECT_EQ(rejected.picker_rows[2].name, "dc0");
  EXPECT_EQ(rejected.picker_rows[2].rtt_ms, 44);
}

TEST_F(ReservationChoose, ReportsNothingForAnUnlistedForcedName) {
  provider.datacenters = two_datacenters;
  Recorder missing;
  const auto selected = choose(provider, "nowhere", {}, {prior()}, &missing);
  EXPECT_FALSE(selected.result);
  EXPECT_EQ(missing.calls, 0U);
  EXPECT_EQ(missing.selections, 0U);
  EXPECT_EQ(provider.fetches, 1U);
  EXPECT_EQ(selected.surveys, 0U);
}

TEST_F(ReservationChoose, ReportsTheRowsAndTheAutomaticChoice) {
  provider.datacenters = two_datacenters;
  Recorder chosen;
  const auto selected = choose(provider, "Auto", {ping()}, {prior()}, &chosen);
  ASSERT_TRUE(selected.result);
  EXPECT_EQ(chosen.calls, 1U);
  EXPECT_EQ(chosen.picker_rows.size(), std::size_t{3});
  EXPECT_EQ(chosen.picker_rows, selected.result.value().picker_rows);
  EXPECT_EQ(chosen.selections, 1U);
  EXPECT_EQ(chosen.selected.name, "dc1");
  EXPECT_EQ(chosen.selected.rtt_ms, 30);
  EXPECT_FALSE(chosen.selected_forced);
  EXPECT_EQ(provider.fetches, 1U);
  EXPECT_EQ(selected.surveys, 1U);
}

TEST_F(ReservationChoose, ReportsAForcedChoiceWithoutMeasuring) {
  provider.datacenters = two_datacenters;
  Recorder forced;
  const auto selected = choose(provider, "dc2", {}, {prior()}, &forced);
  ASSERT_TRUE(selected.result);
  EXPECT_EQ(forced.selections, 1U);
  EXPECT_EQ(forced.selected.name, "dc2");
  EXPECT_TRUE(forced.selected_forced);
  EXPECT_EQ(provider.fetches, 1U);
  EXPECT_EQ(selected.surveys, 0U);
}

TEST_F(ReservationChoose, AnnouncesTheTimedWaitForTheLaunchPick) {
  LaunchPicker timed;
  timed.choice_wait = std::chrono::seconds{20};
  BodyProvider timed_provider;
  const auto asked = launch_pick({Kind::named, "dc2"},
                                 {measured("dc1", 18), measured("dc2", 64)},
                                 timed, timed_provider);
  EXPECT_TRUE(asked.result);
  ASSERT_TRUE(timed.waiting);
  EXPECT_EQ(timed.waiting->wait(),
            (progress::Wait{progress::WaitReason::datacenter_choice,
                            std::chrono::seconds{20}}));
  EXPECT_EQ(timed.waiting->step(), (progress::Position{9, 10}));
  EXPECT_FALSE(timed.waiting->attempt());
}

TEST_F(ReservationChoose, LetsTheUserPickAMeasuredDatacenter) {
  LaunchPicker picker;
  BodyProvider body_provider;
  const auto chosen = launch_pick({Kind::named, "dc2"},
                                  {measured("dc1", 18), measured("dc2", 64)},
                                  picker, body_provider);
  const auto& result = chosen.result;
  ASSERT_TRUE(result);
  EXPECT_EQ(picker.waits, 1U);
  EXPECT_EQ(body_provider.calls, 1U);
  ASSERT_TRUE(picker.waiting);
  EXPECT_EQ(
      picker.waiting->wait(),
      (progress::Wait{progress::WaitReason::datacenter_choice, std::nullopt}));
  EXPECT_EQ(picker.offered, (std::vector<std::string>{"dc1", "dc2"}));
  ASSERT_EQ(picker.picker_rows.size(), std::size_t{2});
  EXPECT_EQ(picker.picker_rows[0].name, "dc1");
  EXPECT_EQ(picker.picker_rows[0].rtt_ms, 18);
  EXPECT_EQ(picker.picker_rows[1].name, "dc2");
  EXPECT_EQ(picker.picker_rows[1].rtt_ms, 64);
  EXPECT_NE(
      std::ranges::find(picker.progress,
                        "Waiting for Datacenter Selection - Step 9 of 10"),
      picker.progress.end());
  EXPECT_EQ(picker.selections, 1U);
  EXPECT_TRUE(picker.selected_forced);
  EXPECT_EQ(picker.selected.name, "dc2");
  EXPECT_EQ(picker.selected.rtt_ms, 64);
  EXPECT_EQ(result.value().selected.name, "dc2");
  EXPECT_NE(body_provider.body.find("\"dc2\""), std::string::npos);
  EXPECT_EQ(body_provider.body.find("\"dc1\""), std::string::npos);
  EXPECT_EQ(result.value().picker_rows.size(), std::size_t{3});
}

TEST_F(ReservationChoose, PicksTheFastestWhenTheUserDoesNotChoose) {
  for (const auto kind : {Kind::timed_out, Kind::automatic}) {
    SCOPED_TRACE(testing::PrintToString(kind));
    LaunchPicker picker;
    BodyProvider body_provider;
    const auto chosen =
        launch_pick({kind, {}}, {measured("dc2", 64), measured("dc1", 18)},
                    picker, body_provider);
    EXPECT_TRUE(chosen.result);
    EXPECT_EQ(picker.selections, 1U);
    EXPECT_FALSE(picker.selected_forced);
    EXPECT_EQ(picker.selected.name, "dc1");
    EXPECT_EQ(picker.selected.rtt_ms, 18);
    EXPECT_NE(body_provider.body.find("\"dc2\""), std::string::npos);
  }
}

TEST_F(ReservationChoose, AssumesTheRoundTripOfAnUnmeasuredPick) {
  LaunchPicker picker;
  BodyProvider body_provider;
  const auto chosen = launch_pick({Kind::named, "dc2"}, {measured("dc1", 18)},
                                  picker, body_provider);
  EXPECT_TRUE(chosen.result);
  EXPECT_TRUE(picker.selected_forced);
  EXPECT_EQ(picker.selected.name, "dc2");
  EXPECT_EQ(picker.selected.rtt_ms, 40);
  EXPECT_EQ(picker.selected.public_ip, "192.0.2.2");
}

TEST_F(ReservationChoose, LetsTheUserPickASlowDatacenter) {
  LaunchPicker picker;
  BodyProvider body_provider;
  const auto chosen = launch_pick({Kind::named, "dc1"},
                                  {measured("dc1", 140), measured("dc2", 150)},
                                  picker, body_provider);
  EXPECT_TRUE(chosen.result);
  EXPECT_TRUE(picker.selected_forced);
  EXPECT_EQ(picker.selected.rtt_ms, 140);
}

TEST_F(ReservationChoose, SelectsNothingTooSlowWhenThePickTimesOut) {
  LaunchPicker picker;
  BodyProvider body_provider;
  const auto chosen = launch_pick({Kind::timed_out, {}},
                                  {measured("dc1", 140), measured("dc2", 150)},
                                  picker, body_provider);
  EXPECT_TRUE(
      failed(chosen, Stage::select, Reason::no_reachable_datacenter, 4));
  EXPECT_EQ(picker.selections, 0U);
  EXPECT_EQ(body_provider.calls, 0U);
}

TEST_F(ReservationChoose, IsCancelledWithThePick) {
  LaunchPicker picker;
  BodyProvider body_provider;
  const auto chosen = launch_pick({Kind::cancelled, {}}, {measured("dc1", 18)},
                                  picker, body_provider);
  EXPECT_TRUE(failed(chosen, Stage::select, Reason::cancelled, 0));
  EXPECT_EQ(picker.selections, 0U);
  EXPECT_EQ(body_provider.calls, 0U);
}

TEST_F(ReservationChoose, RefusesAPickOfAnUnlistedDatacenter) {
  LaunchPicker picker;
  BodyProvider body_provider;
  const auto chosen = launch_pick({Kind::named, "nowhere"},
                                  {measured("dc1", 18)}, picker, body_provider);
  EXPECT_TRUE(failed(chosen, Stage::select, Reason::datacenter_unavailable, 3));
  EXPECT_EQ(body_provider.calls, 0U);
}

TEST_F(ReservationChoose, SelectsTheFastestAndTakesTheRotatedKey) {
  const auto selected = choose(provider, "Auto", {ping()}, {});
  ASSERT_TRUE(selected.result);
  EXPECT_EQ(selected.result.value().selected.name, "dc1");
  EXPECT_EQ(selected.result.value().selected.selected_port, 8975);
  EXPECT_EQ(selected.result.value().picker_rows.size(), std::size_t{1});
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_EQ(selected.key, "new-key");
  EXPECT_EQ(provider.fetches, 1U);
  EXPECT_EQ(selected.surveys, 1U);
}

TEST_F(ReservationChoose, SelectsAForcedDatacenterWithoutMeasuring) {
  provider.rotate = false;
  const auto selected = choose(provider, "dc1", {}, {});
  ASSERT_TRUE(selected.result);
  EXPECT_EQ(selected.result.value().selected.rtt_ms, 40);
  EXPECT_EQ(selected.result.value().selected.selected_port, 8975);
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_EQ(selected.key, "synthetic-key");
  EXPECT_EQ(provider.fetches, 1U);
  EXPECT_EQ(selected.surveys, 0U);
}

TEST_F(ReservationChoose, RefusesAnUnlistedForcedDatacenter) {
  provider.rotate = false;
  const auto selected = choose(provider, "missing", {}, {});
  EXPECT_TRUE(
      failed(selected, Stage::select, Reason::datacenter_unavailable, 3));
  EXPECT_EQ(provider.calls, 0U);
  EXPECT_EQ(provider.fetches, 1U);
  EXPECT_EQ(selected.surveys, 0U);
}

TEST_F(ReservationChoose, SelectsNothingWithoutMeasurements) {
  provider.rotate = false;
  const auto selected = choose(provider, "Auto", {}, {});
  EXPECT_TRUE(
      failed(selected, Stage::select, Reason::no_reachable_datacenter, 4));
  EXPECT_EQ(provider.calls, 0U);
  EXPECT_EQ(provider.fetches, 1U);
  EXPECT_EQ(selected.surveys, 1U);
}

TEST_F(ReservationChoose, KeepsTheKeyWhenTheSelectionIsRejected) {
  provider.status = 500;
  const auto selected = choose(provider, "dc1", {}, {});
  EXPECT_TRUE(failed(selected, Stage::select, Reason::rejected, 5));
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_EQ(selected.key, "synthetic-key");
  EXPECT_EQ(provider.fetches, 1U);
}

TEST_F(ReservationChoose, KeepsTheKeyWhenTheSelectionIsCancelled) {
  provider.error = platform::HttpError::cancelled;
  const auto selected = choose(provider, "dc1", {}, {});
  EXPECT_TRUE(failed(selected, Stage::select, Reason::cancelled, 0));
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_EQ(selected.key, "synthetic-key");
  EXPECT_EQ(provider.fetches, 1U);
}

TEST_F(ReservationChoose, KeepsTheKeyWhenTheSelectionCannotBeSent) {
  provider.error = platform::HttpError::backend_failure;
  const auto selected = choose(provider, "dc1", {}, {});
  EXPECT_TRUE(failed(selected, Stage::select, Reason::transport, 5));
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_EQ(selected.key, "synthetic-key");
  EXPECT_EQ(provider.fetches, 1U);
}

TEST_F(ReservationChoose, FetchesNothingOnceStopped) {
  provider.error = platform::HttpError::backend_failure;
  const auto selected = choose(provider, "dc1", {}, {}, nullptr, true);
  EXPECT_TRUE(failed(selected, Stage::datacenters, Reason::cancelled, 0));
  EXPECT_EQ(provider.calls, 0U);
  EXPECT_EQ(provider.fetches, 0U);
  EXPECT_EQ(selected.surveys, 0U);
}

}  // namespace
}  // namespace cloud_test
