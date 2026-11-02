#include "src/core/play/adapters/datacenters/datacenters.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/cloud_launch/launching.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace play_test {

using test_support::LiveProviders;
using test_support::play::MemoryDatacenterStore;
namespace {

namespace play = libchiaking::play;
using cloud_launches::Launching;
using Service = libchiaking::catalog::ServiceGeneration;
using Ping = libchiaking::psn::gk::Ping;
using Time = play::DatacenterMeasurement::Time;
using std::chrono::milliseconds;
using std::chrono::minutes;

class CloudDatacenters : public cloud_launches::LaunchTest {
 protected:
  static auto answered(std::string name, int rtt_ms) -> Ping {
    Ping row;
    row.name = std::move(name);
    row.rtt_ms = rtt_ms;
    row.measured = true;
    return row;
  }

  static auto silent(std::string name) -> Ping {
    Ping row;
    row.name = std::move(name);
    return row;
  }

  static auto probed() -> Ping {
    Ping first;
    first.name = "dc1";
    first.rtt_ms = 21;
    first.rtts_ms = {20, 22};
    first.mtu_in = 1454;
    first.mtu_out = 1254;
    first.port = 2053;
    first.public_ip = "192.0.2.1";
    first.max_bandwidth = 20000;
    first.measured = true;
    return first;
  }

  auto automatic() const -> play::DatacenterChoice {
    return play::DatacenterChoice::automatic(named("dc1"), milliseconds{21});
  }
};

TEST_F(CloudDatacenters, CacheBandwidthUntilItAgesOrTheRouteChanges) {
  MemoryDatacenterStore store;
  play::Datacenters datacenters{LiveProviders::threading(), store};
  const std::vector fresh{measured("dc1", 21), measured("dc2", 35)};
  EXPECT_TRUE(datacenters.record(Service::ps5_cloud, fresh, automatic()));
  const Time at{std::chrono::hours{1000}};
  EXPECT_FALSE(datacenters.cached_bandwidth(Service::ps5_cloud, "dc1",
                                            milliseconds{21}, at));
  EXPECT_TRUE(datacenters.note_bandwidth(Service::ps5_cloud, "dc1", 55000, at,
                                         milliseconds{21}));
  EXPECT_EQ(store.saves, 2);
  const auto cached = datacenters.cached_bandwidth(
      Service::ps5_cloud, "dc1", milliseconds{25}, at + minutes{10});
  ASSERT_TRUE(cached);
  EXPECT_EQ(cached->kbps, 55000);
  EXPECT_FALSE(datacenters.cached_bandwidth(
      Service::ps5_cloud, "dc1", milliseconds{40}, at + minutes{10}));
  EXPECT_FALSE(datacenters.cached_bandwidth(
      Service::ps5_cloud, "dc1", milliseconds{21}, at + minutes{31}));
  EXPECT_FALSE(datacenters.cached_bandwidth(Service::ps5_cloud, "dc2",
                                            milliseconds{35}, at));
  EXPECT_FALSE(datacenters.cached_bandwidth(Service::ps5_cloud, "dc1",
                                            milliseconds{21}, Time{}));
  EXPECT_FALSE(datacenters.note_bandwidth(Service::ps5_cloud, "dc1", 55000,
                                          Time{}, milliseconds{21}));
  EXPECT_FALSE(datacenters.note_bandwidth(Service::ps5_cloud, "dc9", 55000, at,
                                          milliseconds{21}));
  const std::vector again{measured("dc1", 23, 2000)};
  EXPECT_TRUE(datacenters.record(
      Service::ps5_cloud, again,
      play::DatacenterChoice::automatic(named("dc1"), milliseconds{23})));
  const auto kept_cached = datacenters.cached_bandwidth(
      Service::ps5_cloud, "dc1", milliseconds{23}, at + minutes{5});
  ASSERT_TRUE(kept_cached);
  EXPECT_EQ(kept_cached->kbps, 55000);
  const auto stored = store.held[Service::ps5_cloud];
  const auto kept = std::ranges::find_if(
      stored, [](const play::DatacenterMeasurement& value) {
        return value.name() == "dc1";
      });
  ASSERT_NE(kept, stored.end());
  EXPECT_EQ(kept->probe().bandwidth_kbps, 55000);
  EXPECT_EQ(kept->probe().bandwidth_round_trip, milliseconds{21});
}

TEST_F(CloudDatacenters, RecordOnlyChoicesThatWereNotForcedOrMadeAtLaunch) {
  MemoryDatacenterStore store;
  play::Datacenters datacenters{LiveProviders::threading(), store};
  const std::vector fresh{measured("dc1", 21), measured("dc2", 35)};
  const play::KnownDatacenters known{fresh};

  EXPECT_FALSE(datacenters.record(
      Service::ps5_cloud, fresh,
      play::DatacenterChoice::preferred(named("dc1"), known)));
  EXPECT_FALSE(datacenters.record(Service::ps5_cloud, fresh,
                                  play::DatacenterChoice::picked_at_launch(
                                      named("dc1"), milliseconds{21})));
  EXPECT_EQ(store.saves, 0);
  EXPECT_TRUE(datacenters.known(Service::ps5_cloud).empty());

  EXPECT_TRUE(datacenters.record(Service::ps5_cloud, fresh, automatic()));
  EXPECT_EQ(store.saves, 1);
  EXPECT_EQ(datacenters.known(Service::ps5_cloud).size(), 2U);
  EXPECT_TRUE(datacenters.known(Service::old_service).empty());
  EXPECT_FALSE(datacenters.record(Service::ps5_cloud, fresh,
                                  play::DatacenterChoice::lowest_at_launch(
                                      named("dc1"), milliseconds{21})));
  EXPECT_EQ(store.saves, 1);
  const std::vector slower{measured("dc1", 60, 2000)};
  EXPECT_TRUE(datacenters.record(Service::ps5_cloud, slower,
                                 play::DatacenterChoice::lowest_after_waiting(
                                     named("dc2"), milliseconds{35})));
  EXPECT_EQ(store.saves, 2);
  const auto after_waiting = datacenters.known(Service::ps5_cloud);
  ASSERT_FALSE(after_waiting.all().empty());
  EXPECT_EQ(after_waiting.all().front().name(), "dc2");
  const auto dc1 = after_waiting.find(named("dc1"));
  ASSERT_TRUE(dc1);
  EXPECT_EQ(dc1->round_trip(), milliseconds{60});
  EXPECT_FALSE(datacenters.record(
      Service::ps5_cloud, {},
      play::DatacenterChoice::automatic(named("dc1"), std::nullopt)));
}

TEST_F(CloudDatacenters, AreNotRecordedByALaunchWithAForcedChoice) {
  MemoryDatacenterStore launch_store;
  play::Datacenters launch_datacenters{LiveProviders::threading(), launch_store};
  Launching forced{ps5_case(), &launch_datacenters};
  EXPECT_TRUE(forced.core().run());
  const auto chosen = forced.core().chosen();
  ASSERT_TRUE(chosen);
  EXPECT_EQ(chosen->name(), "dc1");
  EXPECT_TRUE(chosen->was_forced());
  EXPECT_EQ(chosen->how(), play::DatacenterChoice::How::preferred);
  EXPECT_FALSE(chosen->was_made_at_launch());
  EXPECT_EQ(launch_store.saves, 0);
  EXPECT_TRUE(launch_store.held[Service::ps5_cloud].empty());
  EXPECT_TRUE(forced.core().measurements().empty());
  EXPECT_FALSE(forced.core().offer());
  EXPECT_TRUE(launch_datacenters.known(Service::ps5_cloud).empty());
  ASSERT_EQ(forced.listener().chosen.size(), 1U);
  EXPECT_TRUE(forced.listener().chosen.front().forced);
  EXPECT_EQ(forced.listener().chosen.front().reported_rtt_ms, 0);
  EXPECT_TRUE(forced.listener().offers.empty());
  EXPECT_EQ(forced.core().choose_datacenter(named("dc1")).error(),
            play::DatacenterRefusal::launch_over);
  EXPECT_EQ(forced.core().choose_lowest_latency().error(),
            play::DatacenterRefusal::launch_over);
}

TEST_F(CloudDatacenters, CannotBeChosenBeforeTheLaunchWaitsForAChoice) {
  Launching idle{ps5_case()};
  EXPECT_EQ(idle.core().choose_datacenter(named("dc1")).error(),
            play::DatacenterRefusal::not_waiting);
  EXPECT_EQ(idle.core().choose_lowest_latency().error(),
            play::DatacenterRefusal::not_waiting);
  EXPECT_FALSE(idle.core().chosen());
  EXPECT_FALSE(idle.core().refusal());
}

TEST_F(CloudDatacenters, RoundTripThroughTheStore) {
  MemoryDatacenterStore store;
  {
    play::Datacenters datacenters{LiveProviders::threading(), store};
    Ping unanswered;
    unanswered.name = "dc9";
    unanswered.rtt_ms = Ping::unmeasured_rtt_ms;
    Ping unnamed = probed();
    unnamed.name = "Auto";
    const std::vector rows{probed(), unanswered, unnamed};
    const auto fresh =
        play::Datacenters::measured(rows, Time{milliseconds{5000}});
    ASSERT_EQ(fresh.size(), 1U);
    EXPECT_EQ(fresh.front().name(), "dc1");
    EXPECT_EQ(fresh.front().round_trip(), milliseconds{21});
    EXPECT_EQ(fresh.front().probe().samples.size(), 2U);
    EXPECT_EQ(fresh.front().probe().mtu_in, 1454);
    EXPECT_TRUE(datacenters.record(Service::old_service, fresh, automatic()));
    EXPECT_EQ(store.loads, 1);
    EXPECT_EQ(store.saves, 1);
    EXPECT_EQ(datacenters.known(Service::old_service).size(), 1U);
    EXPECT_EQ(store.loads, 1);
  }
  play::Datacenters reopened{LiveProviders::threading(), store};
  const auto known = reopened.known(Service::old_service);
  ASSERT_EQ(known.size(), 1U);
  EXPECT_EQ(known.all().front().name(), "dc1");
  EXPECT_EQ(known.all().front().measured_at(), Time{milliseconds{5000}});
  EXPECT_TRUE(reopened.known(Service::ps5_cloud).empty());
  const auto prior = play::Datacenters::prior_rows(known);
  ASSERT_EQ(prior.size(), 1U);
  EXPECT_EQ(prior.front().name, "dc1");
  EXPECT_EQ(prior.front().rtt_ms, 21);
  EXPECT_TRUE(prior.front().measured);
  EXPECT_EQ(prior.front().rtts_ms, std::vector<int>({20, 22}));
  EXPECT_EQ(prior.front().public_ip, "192.0.2.1");
  EXPECT_EQ(prior.front().port, 2053);

  std::vector<play::DatacenterMeasurement> many;
  for (int index = 0; index < 70; ++index)
    many.push_back(measured("zone" + std::to_string(100 + index), 30 + index));
  EXPECT_TRUE(reopened.record(Service::old_service, many, automatic()));
  EXPECT_EQ(reopened.known(Service::old_service).size(),
            play::KnownDatacenters::limit);
  EXPECT_EQ(store.held[Service::old_service].size(),
            play::KnownDatacenters::limit);
  EXPECT_TRUE(reopened.forget(Service::old_service));
  EXPECT_TRUE(reopened.known(Service::old_service).empty());
  EXPECT_TRUE(store.held[Service::old_service].empty());
}

TEST_F(CloudDatacenters, StayKnownForTheSessionWhenTheStoreFails) {
  MemoryDatacenterStore broken;
  broken.failing = true;
  play::Datacenters datacenters{LiveProviders::threading(), broken};
  EXPECT_TRUE(datacenters.known(Service::ps5_cloud).empty());
  const std::vector fresh{measured("dc1", 21)};
  EXPECT_FALSE(datacenters.record(Service::ps5_cloud, fresh, automatic()));
  EXPECT_EQ(datacenters.known(Service::ps5_cloud).size(), 1U);
}

TEST_F(CloudDatacenters, AreOfferedWithTheLowestLatencyNamed) {
  const std::vector<Ping> picker{answered("dc2", 115), answered("dc1", 20),
                                 silent("dc3")};
  const auto offer =
      play::Datacenters::offer_from(picker, std::chrono::seconds{20});
  EXPECT_EQ(offer.entries().size(), 3U);
  ASSERT_TRUE(offer.lowest());
  EXPECT_EQ(offer.lowest()->name(), "dc1");
  EXPECT_EQ(offer.wait(), std::chrono::seconds{20});
  ASSERT_TRUE(offer.find(named("dc1")));
  ASSERT_TRUE(offer.find(named("dc2")));
  ASSERT_TRUE(offer.find(named("dc3")));
  EXPECT_FALSE(offer.find(named("dc1"))->is_high_latency());
  EXPECT_TRUE(offer.find(named("dc2"))->is_high_latency());
  EXPECT_FALSE(offer.find(named("dc3"))->answered());
}

}  // namespace
}  // namespace play_test
