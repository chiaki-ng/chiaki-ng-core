#include "tests/support/clock.hpp"
#include "tests/support/waiter.hpp"
#include "tests/unit/core/cloud/links/ping_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace cloud_test {
namespace {

namespace gk = libchiaking::psn::gk;
namespace play = libchiaking::play;
using Error = play::DatacenterSurvey::Error;
using Progress = std::vector<std::pair<std::size_t, std::size_t>>;

class SurveyDeadline : public ::testing::Test {
 protected:
  auto survey() -> libchiaking::Expected<play::DatacenterSurvey, Error> {
    play::DatacenterSurvey::Jobs jobs;
    jobs.push_back(PingRig::job(counts, 100, PingRig::make_result(30'000)));
    return play::DatacenterSurvey::start(
        clock, {{"dc-a", "203.0.113.1", 2053, 20'000}}, std::move(jobs));
  }

  static constexpr std::uint64_t deadline = 15'000'000;
  static constexpr auto max = std::numeric_limits<std::uint64_t>::max();

  test_support::FakeClock clock{0};
  ScriptedCounts counts;
};

TEST_F(SurveyDeadline, EndsFifteenSecondsAfterItStarts) {
  const auto zero = survey();
  ASSERT_TRUE(zero);
  EXPECT_EQ(zero.value().deadline_us(), 15'000'000U);
  clock.now_us = 1000;
  const auto one = survey();
  ASSERT_TRUE(one);
  EXPECT_EQ(one.value().deadline_us(), 15'001'000U);
}

TEST_F(SurveyDeadline, RefusesADeadlinePastTheClock) {
  clock.now_us = max - 15'000'000;
  const auto latest = survey();
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest.value().deadline_us(), max);
  clock.now_us = max - 15'000'000 + 1;
  const auto overflow = survey();
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error(), Error::deadline_overflow);
}

TEST_F(SurveyDeadline, TimesOutAtTheDeadline) {
  auto zero = survey();
  ASSERT_TRUE(zero);
  clock.now_us = deadline - 1;
  EXPECT_FALSE(zero.value().step());
  clock.now_us = deadline;
  const auto expired = zero.value().step();
  ASSERT_TRUE(expired);
  EXPECT_TRUE(expired->timed_out);
}

TEST_F(SurveyDeadline, TimesOutPastTheDeadline) {
  auto late = survey();
  ASSERT_TRUE(late);
  clock.now_us = deadline + 1;
  const auto after = late.value().step();
  ASSERT_TRUE(after);
  EXPECT_TRUE(after->timed_out);
}

class SurveyRow : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto profile =
        libchiaking::net::protocol::takion::resolve_takion_wire_profile(9);
    ASSERT_TRUE(profile);
    libchiaking::net::protocol::senkusha::RttStatistics rtt;
    rtt.raw_mean = std::chrono::microseconds{25'999};
    rtt.adjusted_mean = std::chrono::microseconds{5'999};
    result.emplace(9, rtt, 1454, 1254, *profile);
  }

  auto survey_row(std::optional<PingRig::Result> measured)
      -> std::optional<gk::Ping> {
    test_support::FakeClock clock{0};
    ScriptedCounts counts;
    play::DatacenterSurvey::Jobs jobs;
    jobs.push_back(PingRig::job(counts, 1, std::move(measured)));
    auto survey =
        play::DatacenterSurvey::start(clock, {datacenter}, std::move(jobs));
    EXPECT_TRUE(survey);
    if (!survey) return std::nullopt;
    const auto finished = survey.value().step();
    EXPECT_TRUE(finished);
    if (!finished) return std::nullopt;
    EXPECT_FALSE(finished->timed_out);
    EXPECT_EQ(finished->rows.size(), std::size_t{1});
    if (finished->rows.size() != 1) return std::nullopt;
    return finished->rows[0];
  }

  const gk::Datacenter datacenter{"us-east", "203.0.113.7", 2053, 20'000};
  std::optional<PingRig::Result> result;
};

TEST_F(SurveyRow, ReportsTheRawRoundTripOfAMeasuredDatacenter) {
  const auto row = survey_row(result);
  ASSERT_TRUE(row);
  EXPECT_EQ(row->name, "us-east");
  EXPECT_EQ(row->public_ip, "203.0.113.7");
  EXPECT_EQ(row->port, 2053);
  EXPECT_EQ(row->max_bandwidth, 20'000);
  EXPECT_TRUE(row->measured);
  EXPECT_EQ(row->rtt_ms, 25);
  EXPECT_EQ(row->rtts_ms, std::vector<int>({25}));
  EXPECT_EQ(row->mtu_in, 1454);
  EXPECT_EQ(row->mtu_out, 1254);
  EXPECT_EQ(row->selected_port, 0);
}

TEST_F(SurveyRow, ReportsAFailedMeasurementAsUnmeasured) {
  const auto row = survey_row(std::nullopt);
  ASSERT_TRUE(row);
  EXPECT_FALSE(row->measured);
  EXPECT_EQ(row->rtt_ms, 999);
  EXPECT_EQ(row->rtts_ms, std::vector<int>({999}));
  EXPECT_EQ(row->mtu_in, 0);
  EXPECT_EQ(row->mtu_out, 0);
  EXPECT_EQ(row->name, "us-east");
  EXPECT_EQ(row->port, 2053);
}

TEST_F(SurveyRow, ReportsAZeroRoundTrip) {
  result->round_trip_time.raw_mean = std::chrono::microseconds{0};
  const auto row = survey_row(result);
  ASSERT_TRUE(row);
  EXPECT_TRUE(row->measured);
  EXPECT_EQ(row->rtt_ms, 0);
}

TEST_F(SurveyRow, ClampsHugeValuesToAnInt) {
  result->round_trip_time.raw_mean = std::chrono::microseconds{
      static_cast<std::int64_t>(std::numeric_limits<int>::max()) * 1000 + 1500};
  result->inbound_mtu = std::numeric_limits<std::uint32_t>::max();
  result->outbound_mtu = std::numeric_limits<std::uint32_t>::max();
  const auto row = survey_row(result);
  ASSERT_TRUE(row);
  EXPECT_EQ(row->rtt_ms, std::numeric_limits<int>::max());
  EXPECT_EQ(row->mtu_in, std::numeric_limits<int>::max());
  EXPECT_EQ(row->mtu_out, std::numeric_limits<int>::max());
}

class SurveyResults : public ::testing::Test {
 protected:
  class Script {
   public:
    int terminal_poll;
    std::optional<PingRig::Result> result;
  };

  auto make_survey(std::vector<Script> scripts) -> play::DatacenterSurvey {
    std::vector<gk::Datacenter> datacenters{
        {"dc-a", "203.0.113.1", 2053, 20'000},
        {"dc-b", "203.0.113.2", 2053, 20'000},
        {"dc-c", "203.0.113.3", 2053, 20'000},
    };
    datacenters.resize(scripts.size());
    counts.assign(scripts.size(), {});
    play::DatacenterSurvey::Jobs jobs;
    for (std::size_t index = 0; index < scripts.size(); ++index)
      jobs.push_back(PingRig::job(counts[index], scripts[index].terminal_poll,
                                  std::move(scripts[index].result)));
    auto survey = play::DatacenterSurvey::start(clock, std::move(datacenters),
                                                std::move(jobs));
    EXPECT_TRUE(survey);
    return std::move(survey.value());
  }

  static auto unmeasured_and_cancelled(const std::vector<gk::Ping>& rows,
                                       const std::vector<ScriptedCounts>& all)
      -> testing::AssertionResult {
    for (const auto& row : rows)
      if (row.measured || row.rtt_ms != 999)
        return testing::AssertionFailure() << row.name << " was measured";
    for (const auto& count : all)
      if (count.polls != 0 || count.cancels != 1)
        return testing::AssertionFailure()
               << "a job was polled " << count.polls << " times and cancelled "
               << count.cancels << " times";
    return testing::AssertionSuccess();
  }

  test_support::FakeClock clock{0};
  std::vector<ScriptedCounts> counts;
  const PingRig::Result first = PingRig::make_result(25'999);
  const PingRig::Result last = PingRig::make_result(40'999);
};

TEST_F(SurveyResults, FinishesWhenEveryJobHasFinished) {
  auto results = make_survey({{2, first}, {3, std::nullopt}, {1, last}});
  clock.now_us = 10;
  EXPECT_FALSE(results.step());
  clock.now_us = 20;
  EXPECT_FALSE(results.step());
  clock.now_us = 30;
  const auto final = results.step();
  ASSERT_TRUE(final);
  EXPECT_FALSE(final->timed_out);
  EXPECT_EQ(counts[2].polls, 1);
  EXPECT_EQ(counts[0].polls, 2);
  EXPECT_EQ(counts[1].polls, 3);
  ASSERT_EQ(final->rows.size(), std::size_t{3});
  EXPECT_EQ(final->rows[0].name, "dc-a");
  EXPECT_EQ(final->rows[0].rtt_ms, 25);
  EXPECT_EQ(final->rows[1].name, "dc-b");
  EXPECT_FALSE(final->rows[1].measured);
  EXPECT_EQ(final->rows[1].rtt_ms, 999);
  EXPECT_EQ(final->rows[2].name, "dc-c");
  EXPECT_EQ(final->rows[2].rtt_ms, 40);
}

TEST_F(SurveyResults, IsSpentOnceFinished) {
  auto results = make_survey({{2, first}, {3, std::nullopt}, {1, last}});
  for (const std::uint64_t now : {10U, 20U, 30U}) {
    clock.now_us = now;
    static_cast<void>(results.step());
  }
  clock.now_us = 60;
  EXPECT_FALSE(results.step());
  EXPECT_FALSE(results.cancel());
  EXPECT_EQ(counts[2].polls, 1);
  EXPECT_EQ(counts[0].polls, 2);
  EXPECT_EQ(counts[1].polls, 3);
}

TEST_F(SurveyResults, CancelsEveryJobAtTheDeadline) {
  auto timeout = make_survey({{1, first}, {1, first}, {1, first}});
  clock.now_us = timeout.deadline_us();
  const auto final = timeout.step();
  ASSERT_TRUE(final);
  EXPECT_TRUE(final->timed_out);
  EXPECT_TRUE(unmeasured_and_cancelled(final->rows, counts));
}

TEST_F(SurveyResults, KeepsTheFinishedRowsWhenCancelled) {
  auto cancelled = make_survey({{1, first}, {100, first}, {100, first}});
  clock.now_us = 5;
  EXPECT_FALSE(cancelled.step());
  clock.now_us = 6;
  const auto rows = cancelled.cancel();
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), std::size_t{3});
  EXPECT_TRUE((*rows)[0].measured);
  EXPECT_FALSE((*rows)[1].measured);
}

TEST_F(SurveyResults, FinishesAtOnceWithoutDatacenters) {
  auto empty = play::DatacenterSurvey::start(clock, {}, {});
  ASSERT_TRUE(empty);
  const auto final = empty.value().step();
  ASSERT_TRUE(final);
  EXPECT_FALSE(final->timed_out);
  EXPECT_TRUE(final->rows.empty());
}

TEST_F(SurveyResults, ReportsEachFinishedMeasurementWhileRunning) {
  auto counted = make_survey({{2, first}, {3, std::nullopt}, {1, last}});
  EXPECT_EQ(counted.reported(), std::size_t{0});
  test_support::FakeWaiter still;
  Progress measured;
  const auto ran =
      counted.run(still, {}, [&](std::size_t done, std::size_t total) {
        measured.emplace_back(done, total);
      });
  ASSERT_TRUE(ran);
  EXPECT_FALSE(ran.value().timed_out);
  EXPECT_EQ(ran.value().rows.size(), std::size_t{3});
  EXPECT_EQ(measured, (Progress{{1, 3}, {2, 3}, {3, 3}}));
  EXPECT_EQ(still.calls(), 2);
}

TEST_F(SurveyResults, StopsReportingAtTheDeadline) {
  auto late = make_survey({{1, first}, {100, first}, {100, first}});
  test_support::FakeWaiter impatient;
  impatient.on_wait = [&] { clock.now_us = late.deadline_us(); };
  Progress partly;
  const auto cut =
      late.run(impatient, {}, [&](std::size_t done, std::size_t total) {
        partly.emplace_back(done, total);
      });
  ASSERT_TRUE(cut);
  EXPECT_TRUE(cut.value().timed_out);
  EXPECT_EQ(partly, (Progress{{1, 3}}));
}

class SurveyOwner : public ::testing::Test {
 protected:
  static constexpr std::uint64_t start = 1'000'000;
  static constexpr std::uint64_t deadline = start + 15'000'000;

  static auto datacenters(std::size_t count) -> std::vector<gk::Datacenter> {
    auto rows = std::vector<gk::Datacenter>{
        {"dc-a", "203.0.113.1", 2053, 20'000},
        {"dc-b", "203.0.113.2", 2053, 20'000},
        {"dc-c", "203.0.113.3", 2053, 20'000},
    };
    rows.resize(count);
    return rows;
  }

  static auto job(ScriptedCounts& counts, int terminal_poll, bool succeeds)
      -> std::unique_ptr<play::PingJob> {
    return PingRig::job(
        counts, terminal_poll,
        succeeds ? std::optional{PingRig::make_result(30'123)} : std::nullopt);
  }

  static auto one_job(ScriptedCounts& counts) -> play::DatacenterSurvey::Jobs {
    play::DatacenterSurvey::Jobs jobs;
    jobs.push_back(job(counts, 100, true));
    return jobs;
  }

  test_support::FakeClock clock{start};
};

TEST_F(SurveyOwner, PollsEachJobUntilItFinishes) {
  ScriptedCounts a;
  ScriptedCounts b;
  ScriptedCounts c;
  play::DatacenterSurvey::Jobs jobs;
  jobs.push_back(job(a, 2, true));
  jobs.push_back(job(b, 1, false));
  jobs.push_back(job(c, 3, true));
  auto made =
      play::DatacenterSurvey::start(clock, datacenters(3), std::move(jobs));
  ASSERT_TRUE(made);
  auto& owner = made.value();
  clock.now_us = start + 1;
  EXPECT_FALSE(owner.step());
  clock.now_us = start + 2;
  EXPECT_FALSE(owner.step());
  clock.now_us = start + 3;
  const auto final = owner.step();
  ASSERT_TRUE(final);
  EXPECT_FALSE(final->timed_out);
  ASSERT_EQ(final->rows.size(), std::size_t{3});
  EXPECT_TRUE(final->rows[0].measured);
  EXPECT_EQ(final->rows[0].rtt_ms, 30);
  EXPECT_FALSE(final->rows[1].measured);
  EXPECT_EQ(final->rows[1].rtt_ms, 999);
  EXPECT_TRUE(final->rows[2].measured);
  EXPECT_EQ(final->rows[2].rtt_ms, 30);
  EXPECT_EQ(a.polls, 2);
  EXPECT_EQ(b.polls, 1);
  EXPECT_EQ(c.polls, 3);
  clock.now_us = start + 4;
  EXPECT_FALSE(owner.step());
  EXPECT_EQ(a.cancels, 0);
  EXPECT_EQ(b.cancels, 0);
  EXPECT_EQ(c.cancels, 0);
}

TEST_F(SurveyOwner, CancelsAnUnfinishedJobAtTheDeadlineOnce) {
  ScriptedCounts t;
  auto timed = play::DatacenterSurvey::start(clock, datacenters(1), one_job(t));
  ASSERT_TRUE(timed);
  clock.now_us = deadline;
  const auto final = timed.value().step();
  ASSERT_TRUE(final);
  EXPECT_TRUE(final->timed_out);
  ASSERT_FALSE(final->rows.empty());
  EXPECT_FALSE(final->rows[0].measured);
  EXPECT_EQ(t.polls, 0);
  EXPECT_EQ(t.cancels, 1);
  clock.now_us = deadline + 1;
  EXPECT_FALSE(timed.value().step());
}

TEST_F(SurveyOwner, CancelsAnUnfinishedJobOnce) {
  ScriptedCounts x;
  auto cancelled =
      play::DatacenterSurvey::start(clock, datacenters(1), one_job(x));
  ASSERT_TRUE(cancelled);
  clock.now_us = start + 1;
  EXPECT_FALSE(cancelled.value().step());
  clock.now_us = start + 2;
  const auto cancelled_rows = cancelled.value().cancel();
  ASSERT_TRUE(cancelled_rows);
  EXPECT_EQ(cancelled_rows->size(), std::size_t{1});
  EXPECT_EQ(x.polls, 1);
  EXPECT_EQ(x.cancels, 1);
  clock.now_us = start + 3;
  EXPECT_FALSE(cancelled.value().cancel());
}

TEST_F(SurveyOwner, CancelsItsJobsWhenDestroyed) {
  ScriptedCounts d;
  {
    auto scoped =
        play::DatacenterSurvey::start(clock, datacenters(1), one_job(d));
    ASSERT_TRUE(scoped);
  }
  EXPECT_EQ(d.cancels, 1);
}

TEST_F(SurveyOwner, HandsItsJobsOverWhenMoved) {
  ScriptedCounts moved_counts;
  {
    auto source = play::DatacenterSurvey::start(clock, datacenters(1),
                                                one_job(moved_counts));
    ASSERT_TRUE(source);
    auto moved = std::move(source.value());
    clock.now_us = start + 1;
    EXPECT_FALSE(source.value().step());
    EXPECT_FALSE(moved.step());
    EXPECT_EQ(moved_counts.polls, 1);
    EXPECT_EQ(moved_counts.cancels, 0);
  }
  EXPECT_EQ(moved_counts.cancels, 1);
}

TEST_F(SurveyOwner, CancelsTheJobsOfAMismatchedSurvey) {
  ScriptedCounts mismatch;
  const auto invalid =
      play::DatacenterSurvey::start(clock, datacenters(2), one_job(mismatch));
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), Error::count_mismatch);
  EXPECT_EQ(mismatch.cancels, 1);
}

TEST_F(SurveyOwner, CancelsTheJobsOfASurveyWithANullJob) {
  ScriptedCounts nonnull;
  auto null_jobs = one_job(nonnull);
  null_jobs.push_back(nullptr);
  const auto invalid_null = play::DatacenterSurvey::start(clock, datacenters(2),
                                                          std::move(null_jobs));
  ASSERT_FALSE(invalid_null);
  EXPECT_EQ(invalid_null.error(), Error::null_job);
  EXPECT_EQ(nonnull.cancels, 1);
}

TEST_F(SurveyOwner, CancelsTheJobsOfASurveyWhoseDeadlineOverflows) {
  ScriptedCounts overflow;
  clock.now_us = std::numeric_limits<std::uint64_t>::max();
  const auto invalid_overflow =
      play::DatacenterSurvey::start(clock, datacenters(1), one_job(overflow));
  ASSERT_FALSE(invalid_overflow);
  EXPECT_EQ(invalid_overflow.error(), Error::deadline_overflow);
  EXPECT_EQ(overflow.cancels, 1);
}

class SurveyRunner : public ::testing::Test {
 protected:
  void SetUp() override {
    waiter.step_us = 10'000;
    waiter.answer = true;
    waiter.stop_state = state.get();
  }

  void TearDown() override {
    EXPECT_TRUE(std::ranges::all_of(
        waiter.waits, [](int milliseconds) { return milliseconds == 1; }));
  }

  auto make_owner(ScriptedCounts& counts, int terminal_poll)
      -> play::DatacenterSurvey {
    play::DatacenterSurvey::Jobs jobs;
    jobs.push_back(
        PingRig::job(counts, terminal_poll, PingRig::make_result(30'000)));
    auto owner = play::DatacenterSurvey::start(
        clock, {{"dc-a", "203.0.113.1", 2053, 20'000}}, std::move(jobs));
    EXPECT_TRUE(owner);
    return std::move(owner.value());
  }

  void request_stop() { waiter.stop_on_wait = 1; }

  void fail_waits() { waiter.answer = false; }

  test_support::FakeClock clock{1'000'000};
  std::shared_ptr<test_support::StopState> state =
      std::make_shared<test_support::StopState>();
  const libchiaking::platform::StopToken stop{state};
  test_support::FakeWaiter waiter{&clock};
};

TEST_F(SurveyRunner, PollsUntilTheJobFinishes) {
  ScriptedCounts success;
  auto owner = make_owner(success, 3);
  const auto result = owner.run(waiter, stop);
  ASSERT_TRUE(result);
  EXPECT_FALSE(result.value().timed_out);
  ASSERT_EQ(result.value().rows.size(), std::size_t{1});
  EXPECT_TRUE(result.value().rows[0].measured);
  EXPECT_EQ(success.polls, 3);
  EXPECT_EQ(success.cancels, 0);
  EXPECT_EQ(waiter.calls(), 2);
}

TEST_F(SurveyRunner, IsCancelledByAStopDuringTheWait) {
  request_stop();
  ScriptedCounts cancelled_counts;
  auto cancelled_owner = make_owner(cancelled_counts, 100);
  const auto result = cancelled_owner.run(waiter, stop);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), Error::cancelled);
  EXPECT_EQ(cancelled_counts.polls, 1);
  EXPECT_EQ(cancelled_counts.cancels, 1);
}

TEST_F(SurveyRunner, IsCancelledByAFailedWait) {
  fail_waits();
  ScriptedCounts wait_failed;
  auto failed_owner = make_owner(wait_failed, 100);
  const auto result = failed_owner.run(waiter, stop);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), Error::cancelled);
  EXPECT_EQ(wait_failed.polls, 1);
  EXPECT_EQ(wait_failed.cancels, 1);
}

TEST_F(SurveyRunner, PollsNothingOnceStopped) {
  fail_waits();
  state->stopped = true;
  ScriptedCounts pre_cancelled;
  auto pre_owner = make_owner(pre_cancelled, 100);
  const auto result = pre_owner.run(waiter, stop);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), Error::cancelled);
  EXPECT_EQ(pre_cancelled.polls, 0);
  EXPECT_EQ(pre_cancelled.cancels, 1);
}

TEST_F(SurveyRunner, TimesOutWhenAWaitPassesTheDeadline) {
  waiter.step_us = 15'000'000;
  ScriptedCounts timed_out;
  auto timeout_owner = make_owner(timed_out, 100);
  const auto result = timeout_owner.run(waiter, stop);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.value().timed_out);
  ASSERT_FALSE(result.value().rows.empty());
  EXPECT_FALSE(result.value().rows[0].measured);
  EXPECT_EQ(timed_out.polls, 1);
  EXPECT_EQ(timed_out.cancels, 1);
}

TEST_F(SurveyRunner, KeepsTheFinishedRowsWhenItTimesOut) {
  waiter.step_us = 15'000'000;
  ScriptedCounts partial_measured;
  ScriptedCounts partial_pending;
  play::DatacenterSurvey::Jobs jobs;
  jobs.push_back(
      PingRig::job(partial_measured, 1, PingRig::make_result(30'000)));
  jobs.push_back(
      PingRig::job(partial_pending, 100, PingRig::make_result(30'000)));
  auto mixed =
      play::DatacenterSurvey::start(clock,
                                    {
                                        {"dc-a", "203.0.113.1", 2053, 20'000},
                                        {"dc-b", "203.0.113.2", 2053, 20'000},
                                    },
                                    std::move(jobs));
  ASSERT_TRUE(mixed);
  const auto result = mixed.value().run(waiter, stop);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.value().timed_out);
  ASSERT_EQ(result.value().rows.size(), std::size_t{2});
  EXPECT_TRUE(result.value().rows[0].measured);
  EXPECT_FALSE(result.value().rows[1].measured);
  EXPECT_EQ(partial_measured.polls, 1);
  EXPECT_EQ(partial_measured.cancels, 0);
  EXPECT_EQ(partial_pending.polls, 1);
  EXPECT_EQ(partial_pending.cancels, 1);
}

}  // namespace
}  // namespace cloud_test
