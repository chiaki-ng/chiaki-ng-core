#include "src/core/catalog/model/library/owned_record.hpp"
#include "tests/support/enum_printing.hpp"

#include <libchiaking/detail/catalog/launch_verdict.hpp>
#include <libchiaking/detail/catalog/release_date.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace catalog_test {
namespace {

using libchiaking::catalog::LaunchRefusal;
using libchiaking::catalog::ReleaseDate;
using State = ReleaseDate::State;
using Time = ReleaseDate::Time;

class ReleaseDateCases : public ::testing::Test {
 protected:
  static constexpr std::int64_t now_ms = 1'791'241'200'000;

  static auto at_ms(std::int64_t value) -> Time {
    return Time{std::chrono::milliseconds{value}};
  }

  static auto instant_ms(std::string_view date) -> std::int64_t {
    const auto parsed = ReleaseDate::parse_time(date);
    EXPECT_TRUE(parsed) << date;
    return parsed.value().time_since_epoch().count();
  }

  static auto scheduled_ms(ReleaseDate release) -> std::int64_t {
    EXPECT_TRUE(release.time());
    return release.time().value().time_since_epoch().count();
  }

  static auto record(std::optional<std::string> active_date, bool preorder)
      -> ReleaseDate {
    libchiaking::catalog::OwnedRecord::Fields fields;
    fields.id = "X";
    fields.active_date = std::move(active_date);
    fields.preorder = preorder;
    return libchiaking::catalog::OwnedRecord{std::move(fields)}.release();
  }

  static auto refusal_kind(ReleaseDate release, std::optional<Time> now)
      -> std::optional<LaunchRefusal::Kind> {
    const auto refusal = LaunchRefusal::of_release(release, now);
    return refusal ? std::optional{refusal->kind()} : std::nullopt;
  }
};

class ReleaseDateTimes : public ReleaseDateCases {};
class ReleaseDateRecords : public ReleaseDateCases {};
class ReleaseDateOrder : public ReleaseDateCases {};
class ReleaseDateGate : public ReleaseDateCases {};
class ReleaseDateCountdown : public ReleaseDateCases {};

TEST_F(ReleaseDateTimes, ParsePsDatesToInstants) {
  EXPECT_EQ(instant_ms("1970-01-01T00:00:00Z"), 0);
  EXPECT_EQ(instant_ms("2024-02-29T00:00:00Z"),
            std::int64_t{19782} * 86'400'000);
  EXPECT_EQ(instant_ms("2026-10-05T23:00:00Z"), 1791241200000);
  EXPECT_EQ(instant_ms("2026-10-05T23:00:00.250Z"), 1791241200250);
  EXPECT_EQ(instant_ms("2026-10-06T01:00:00+02:00"), 1791241200000);
}

TEST_F(ReleaseDateTimes, RefuseTextThatIsNotAnInstant) {
  EXPECT_FALSE(ReleaseDate::parse_time("soon"));
  EXPECT_FALSE(ReleaseDate::parse_time(""));
  EXPECT_FALSE(ReleaseDate::parse_time("2026-13-05T23:00:00Z"));
  EXPECT_FALSE(ReleaseDate::parse_time("2026-10-05"));
  EXPECT_FALSE(ReleaseDate::parse_time("2026-10-05T23:00:00.Z"));
}

TEST_F(ReleaseDateTimes, ScaleFractionalSecondsToMilliseconds) {
  const auto whole = instant_ms("2026-10-05T23:00:00Z");
  EXPECT_EQ(instant_ms("2026-10-05T23:00:00.5Z"), whole + 500);
  EXPECT_EQ(instant_ms("2026-10-05T23:00:00.05Z"), whole + 50);
  EXPECT_EQ(instant_ms("2026-10-05T23:00:00.123Z"), whole + 123);
  EXPECT_EQ(instant_ms("2026-10-05T23:00:00.123456Z"), whole + 123);
  EXPECT_EQ(instant_ms("2026-10-05T23:00:00.9999999999Z"), whole + 999);
  EXPECT_EQ(instant_ms("2026-10-06T01:00:00.5+02:00"), whole + 500);
}

TEST_F(ReleaseDateRecords, AreUnrestrictedWithoutADateOrPreorder) {
  EXPECT_TRUE(record(std::nullopt, false).is_unrestricted());
  EXPECT_TRUE(record(std::string{}, false).is_unrestricted());
}

TEST_F(ReleaseDateRecords, AreUnannouncedForAPreorderWithoutADate) {
  EXPECT_TRUE(record(std::nullopt, true).is_unannounced());
  EXPECT_TRUE(record(std::string{}, true).is_unannounced());
}

TEST_F(ReleaseDateRecords, AreUnannouncedForAnUnreadableDate) {
  EXPECT_TRUE(record(std::string{"next tuesday"}, false).is_unannounced());
  EXPECT_TRUE(record(std::string{"1791241200000"}, false).is_unannounced());
  EXPECT_TRUE(
      record(std::string{"1970-01-01T00:00:00Z"}, false).is_unannounced());
}

TEST_F(ReleaseDateRecords, AreScheduledAtTheirDate) {
  EXPECT_EQ(scheduled_ms(record(std::string{"2026-11-20T00:00:00.000Z"}, true)),
            instant_ms("2026-11-20T00:00:00Z"));
  EXPECT_EQ(
      ReleaseDate::of_record(std::string_view{"2026-11-20T00:00:00Z"}, false),
      ReleaseDate::at(at_ms(instant_ms("2026-11-20T00:00:00Z"))));
}

TEST_F(ReleaseDateRecords, TakeAPlayabilityDateBeforeTheActivationDate) {
  const auto early =
      ReleaseDate::of_record(std::string_view{"2023-05-05T04:00:00Z"}, true,
                             std::string_view{"2023-02-07T05:00:00Z"});
  EXPECT_EQ(early, ReleaseDate::at(at_ms(instant_ms("2023-02-07T05:00:00Z"))));
  EXPECT_EQ(ReleaseDate::of_record(std::nullopt, true,
                                   std::string_view{"2023-02-07T05:00:00Z"}),
            ReleaseDate::at(at_ms(instant_ms("2023-02-07T05:00:00Z"))));
}

TEST_F(ReleaseDateRecords,
       FallBackToTheActivationDateForAnUnreadablePlayability) {
  const auto unreadable = ReleaseDate::of_record(
      std::string_view{"2023-05-05T04:00:00Z"}, true, std::string_view{"soon"});
  EXPECT_EQ(unreadable,
            ReleaseDate::at(at_ms(instant_ms("2023-05-05T04:00:00Z"))));
  EXPECT_TRUE(ReleaseDate::of_record(std::nullopt, true, std::string_view{})
                  .is_unannounced());
}

TEST_F(ReleaseDateOrder, PicksTheEarliest) {
  const auto launch =
      ReleaseDate::at(at_ms(instant_ms("2026-11-20T00:00:00Z")));
  const auto later = ReleaseDate::at(at_ms(instant_ms("2026-12-01T00:00:00Z")));
  EXPECT_EQ(ReleaseDate::unannounced().earliest(launch), launch);
  EXPECT_EQ(launch.earliest(ReleaseDate::unrestricted()),
            ReleaseDate::unrestricted());
  EXPECT_EQ(ReleaseDate::unannounced().earliest(ReleaseDate::unannounced()),
            ReleaseDate::unannounced());
  EXPECT_EQ(launch.earliest(later), launch);
  EXPECT_EQ(later.earliest(launch), launch);
}

TEST_F(ReleaseDateOrder, PicksTheLatest) {
  const auto launch =
      ReleaseDate::at(at_ms(instant_ms("2026-11-20T00:00:00Z")));
  const auto later = ReleaseDate::at(at_ms(instant_ms("2026-12-01T00:00:00Z")));
  EXPECT_EQ(launch.latest(later), later);
  EXPECT_EQ(later.latest(launch), later);
  EXPECT_EQ(launch.latest(ReleaseDate::unannounced()),
            ReleaseDate::unannounced());
  EXPECT_EQ(ReleaseDate::unrestricted().latest(launch), launch);
}

TEST_F(ReleaseDateOrder, DefaultsToUnrestricted) {
  EXPECT_EQ(ReleaseDate{}, ReleaseDate::unrestricted());
}

TEST_F(ReleaseDateGate, MatchesTheLaunchRefusal) {
  for (const auto available :
       {std::int64_t{0}, std::int64_t{-5}, now_ms - 1, now_ms, now_ms + 1})
    for (const auto clock :
         {std::optional<Time>{at_ms(now_ms)}, std::optional<Time>{}}) {
      const auto release = ReleaseDate::at(at_ms(available));
      EXPECT_EQ(release.blocks_launch(clock),
                release.state(clock) != State::released)
          << available;
      EXPECT_EQ(release.blocks_launch(clock),
                LaunchRefusal::of_release(release, clock).has_value())
          << available;
    }
}

TEST_F(ReleaseDateGate, BlocksAnUnannouncedRelease) {
  for (const auto clock :
       {std::optional<Time>{at_ms(now_ms)}, std::optional<Time>{}}) {
    EXPECT_TRUE(ReleaseDate::unannounced().blocks_launch(clock));
    EXPECT_EQ(refusal_kind(ReleaseDate::unannounced(), clock),
              LaunchRefusal::Kind::unannounced);
  }
  EXPECT_EQ(ReleaseDate::unannounced().state(at_ms(now_ms)),
            State::unannounced);
}

TEST_F(ReleaseDateGate, RefusesAFutureReleaseWithItsTime) {
  EXPECT_EQ(ReleaseDate::at(at_ms(now_ms + 1)).state(at_ms(now_ms)),
            State::not_released);
  EXPECT_EQ(refusal_kind(ReleaseDate::at(at_ms(now_ms + 1)), at_ms(now_ms)),
            LaunchRefusal::Kind::not_released);
  const auto refusal = LaunchRefusal::of_release(
      ReleaseDate::at(at_ms(now_ms + 1)), at_ms(now_ms));
  ASSERT_TRUE(refusal);
  EXPECT_EQ(refusal->time(), at_ms(now_ms + 1));
}

TEST_F(ReleaseDateGate, NeedsATrustedClockForADatedRelease) {
  EXPECT_EQ(ReleaseDate::at(at_ms(now_ms)).state(std::nullopt),
            State::no_trusted_clock);
  EXPECT_EQ(refusal_kind(ReleaseDate::at(at_ms(now_ms)), std::nullopt),
            LaunchRefusal::Kind::no_trusted_clock);
  EXPECT_EQ(ReleaseDate::at(at_ms(0)).state(std::nullopt), State::released);
  EXPECT_EQ(ReleaseDate::unrestricted().state(std::nullopt), State::released);
}

TEST_F(ReleaseDateGate, KeepsAPreorderWithoutADateLocked) {
  const auto release = ReleaseDate::unannounced();
  EXPECT_TRUE(release.is_unannounced());
  EXPECT_FALSE(release.time());
  for (const auto clock :
       {std::optional<Time>{at_ms(1500)}, std::optional<Time>{}}) {
    EXPECT_EQ(release.state(clock), State::unannounced);
    EXPECT_TRUE(release.blocks_launch(clock));
    EXPECT_EQ(refusal_kind(release, clock), LaunchRefusal::Kind::unannounced);
  }
  EXPECT_FALSE(release.countdown(at_ms(1500)));
}

TEST_F(ReleaseDateGate, CountsDownToTheLaterOfTwoDates) {
  const auto release =
      ReleaseDate::at(at_ms(2000)).latest(ReleaseDate::at(at_ms(1000)));
  EXPECT_EQ(release, ReleaseDate::at(at_ms(2000)));
  EXPECT_EQ(release.state(at_ms(1500)), State::not_released);
  EXPECT_EQ(release.countdown(at_ms(1500)),
            (ReleaseDate::Countdown{0, 0, 0, 1}));
  EXPECT_EQ(refusal_kind(release, at_ms(1500)),
            LaunchRefusal::Kind::not_released);
  const auto refusal = LaunchRefusal::of_release(release, at_ms(1500));
  ASSERT_TRUE(refusal);
  EXPECT_EQ(refusal->time(), at_ms(2000));
}

TEST_F(ReleaseDateGate, StaysLockedAtItsInstant) {
  const auto release = ReleaseDate::at(at_ms(1000));
  EXPECT_EQ(release.state(at_ms(1000)), State::not_released);
  EXPECT_TRUE(release.blocks_launch(at_ms(1000)));
  EXPECT_EQ(release.countdown(at_ms(1000)),
            (ReleaseDate::Countdown{0, 0, 0, 0}));
  EXPECT_EQ(refusal_kind(release, at_ms(1000)),
            LaunchRefusal::Kind::not_released);
}

TEST_F(ReleaseDateGate, ReleasesOneMillisecondAfterItsInstant) {
  const auto release = ReleaseDate::at(at_ms(1000));
  EXPECT_EQ(release.state(at_ms(1001)), State::released);
  EXPECT_FALSE(release.blocks_launch(at_ms(1001)));
  EXPECT_FALSE(release.countdown(at_ms(1001)));
  EXPECT_FALSE(LaunchRefusal::of_release(release, at_ms(1001)));
}

TEST_F(ReleaseDateGate, ReleasesNothingDatedWithoutATrustedClock) {
  const auto future = ReleaseDate::at(at_ms(4'000'000'000'000));
  EXPECT_EQ(future.state(std::nullopt), State::no_trusted_clock);
  EXPECT_TRUE(future.blocks_launch(std::nullopt));
  EXPECT_EQ(refusal_kind(future, std::nullopt),
            LaunchRefusal::Kind::no_trusted_clock);
  EXPECT_EQ(future.time(), at_ms(4'000'000'000'000));
  const auto past = ReleaseDate::at(at_ms(1000));
  EXPECT_EQ(past.state(std::nullopt), State::no_trusted_clock);
  EXPECT_TRUE(past.blocks_launch(std::nullopt));
  EXPECT_EQ(refusal_kind(past, std::nullopt),
            LaunchRefusal::Kind::no_trusted_clock);
  EXPECT_EQ(past.state(at_ms(5000)), State::released);
}

TEST_F(ReleaseDateGate, TreatsADateAtOrBeforeTheEpochAsNoDate) {
  EXPECT_TRUE(ReleaseDate::at(at_ms(-5)).is_unrestricted());
  EXPECT_TRUE(ReleaseDate::at(at_ms(0)).is_unrestricted());
  EXPECT_EQ(ReleaseDate::at(at_ms(-5)).state(std::nullopt), State::released);
  EXPECT_FALSE(
      LaunchRefusal::of_release(ReleaseDate::at(at_ms(-5)), std::nullopt));
  EXPECT_FALSE(ReleaseDate::at(at_ms(1)).is_unrestricted());
}

TEST_F(ReleaseDateCountdown, ShowsOneSecondForOneMillisecondLeft) {
  const auto release = ReleaseDate::at(at_ms(1000));
  EXPECT_EQ(release.state(at_ms(999)), State::not_released);
  EXPECT_EQ(release.countdown(at_ms(999)),
            (ReleaseDate::Countdown{0, 0, 0, 1}));
  EXPECT_EQ(refusal_kind(release, at_ms(999)),
            LaunchRefusal::Kind::not_released);
}

TEST_F(ReleaseDateCountdown, SplitsIntoDaysAndAClock) {
  using namespace std::chrono;
  const auto now = at_ms(now_ms);
  const auto release = ReleaseDate::at(now + days{3} + hours{4} + minutes{5});
  EXPECT_EQ(release.countdown(now), (ReleaseDate::Countdown{3, 4, 5, 0}));
  EXPECT_EQ(ReleaseDate::at(now + hours{23} + minutes{59} + seconds{59} +
                            milliseconds{1})
                .countdown(now),
            (ReleaseDate::Countdown{1, 0, 0, 0}));
  EXPECT_EQ(ReleaseDate::at(now + seconds{61}).countdown(now),
            (ReleaseDate::Countdown{0, 0, 1, 1}));
  EXPECT_FALSE(release.countdown(now + days{4}));
}

}  // namespace
}  // namespace catalog_test
