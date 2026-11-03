#include "tests/support/enum_printing.hpp"
#include "tests/support/live_providers.hpp"
#include "tests/unit/core/play/adapters/links/internet/internet_start_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace play_test {

using test_support::LiveProviders;
namespace {

using internet_starts::HeardRoutes;
using internet_starts::PlayedStart;
using Route = libchiaking::progress::InternetRoute;
namespace holepunch = libchiaking::net::nat::holepunch;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;

class InternetRouteProgress : public ::testing::Test {
 protected:
  static auto offered(holepunch::CandidateType type) -> holepunch::Candidate {
    return {type, "192.0.2.1", "192.0.2.1", 9303, 9303};
  }

  static auto plain() -> std::vector<holepunch::Candidate> {
    using Type = holepunch::CandidateType;
    return {offered(Type::local), offered(Type::stun), offered(Type::static_)};
  }

  static auto guessed() -> std::vector<holepunch::Candidate> {
    using Type = holepunch::CandidateType;
    std::vector guessed{offered(Type::local), offered(Type::local)};
    for (int guess = 0; guess < 75; ++guess)
      guessed.push_back(offered(Type::stun));
    guessed.push_back(offered(Type::static_));
    return guessed;
  }

  static auto chosen(const holepunch::ConnRequest& console, std::size_t index,
                     bool derived = false) -> Route::Kind {
    holepunch::SelectedCandidate selected;
    selected.candidate_index = index;
    selected.derived = derived;
    return play::InternetStart::kind_of(console, selected);
  }
};

TEST_F(InternetRouteProgress, ReportsEveryRouteSubStepInOrder) {
  using FailureStage = remote_play::FailureStage;
  using Step = Route::Step;
  using Purpose = Route::Purpose;
  PlayedStart start;
  ASSERT_NO_FATAL_FAILURE(start.play());
  const auto routes = start.routes();
  const std::vector<std::pair<FailureStage, std::pair<Step, Purpose>>> expected{
      {FailureStage::psn_open,
       {Step::opening_account_session, Purpose::control}},
      {FailureStage::control_holepunch,
       {Step::gathering_candidates, Purpose::control}},
      {FailureStage::psn_start, {Step::waiting_for_console, Purpose::control}},
      {FailureStage::control_holepunch,
       {Step::exchanging_offers, Purpose::control}},
      {FailureStage::control_holepunch,
       {Step::testing_candidates, Purpose::control}},
      {FailureStage::control_holepunch, {Step::route_chosen, Purpose::control}},
      {FailureStage::control_holepunch,
       {Step::confirming_route, Purpose::control}},
      {FailureStage::data_holepunch,
       {Step::gathering_candidates, Purpose::stream}},
      {FailureStage::data_holepunch,
       {Step::exchanging_offers, Purpose::stream}},
      {FailureStage::data_holepunch,
       {Step::testing_candidates, Purpose::stream}},
      {FailureStage::data_holepunch, {Step::route_chosen, Purpose::stream}},
      {FailureStage::data_holepunch, {Step::confirming_route, Purpose::stream}},
      {FailureStage::route_switch, {Step::switching_control, Purpose::stream}}};
  ASSERT_EQ(routes.size(), expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    SCOPED_TRACE(index);
    const auto& told = routes[index];
    ASSERT_TRUE(told.route);
    EXPECT_EQ(told.stage, expected[index].first);
    EXPECT_EQ(told.route->step, expected[index].second.first);
    EXPECT_EQ(told.route->purpose, expected[index].second.second);
    const bool counted = told.route->step == Step::exchanging_offers ||
                         told.route->step == Step::testing_candidates;
    const bool decided = told.route->step == Step::route_chosen ||
                         told.route->step == Step::confirming_route;
    EXPECT_EQ(told.route->candidates.has_value(), counted);
    EXPECT_EQ(told.route->chosen.has_value(), decided);
    if (counted && told.route->candidates) {
      EXPECT_GE(told.route->candidates->total(), 1U);
    }
  }
  EXPECT_EQ(routes[5].route->chosen, routes[6].route->chosen);
  EXPECT_EQ(routes[10].route->chosen, routes[11].route->chosen);
}

TEST_F(InternetRouteProgress, CountsCandidatesByTheKindAnAppCanName) {
  EXPECT_EQ(play::InternetStart::candidates_of(plain()),
            (Route::Candidates{1, 2, 0}));
  EXPECT_EQ(play::InternetStart::candidates_of({}), Route::Candidates{});
  const auto many = guessed();
  EXPECT_EQ(play::InternetStart::candidates_of(many),
            (Route::Candidates{2, 1, 75}));
  EXPECT_EQ(play::InternetStart::candidates_of(many).total(), many.size());
}

TEST_F(InternetRouteProgress, NamesTheKindOfTheChosenCandidate) {
  using Kind = Route::Kind;
  holepunch::ConnRequest console;
  console.candidates = plain();
  EXPECT_EQ(chosen(console, 0), Kind::local);
  EXPECT_EQ(chosen(console, 1), Kind::public_address);
  EXPECT_EQ(chosen(console, 2), Kind::public_address);
  EXPECT_EQ(chosen(console, 0, true), Kind::discovered);
  EXPECT_EQ(chosen(console, 3), Kind::discovered);
  EXPECT_EQ(chosen(console, SIZE_MAX), Kind::discovered);
  console.candidates = guessed();
  EXPECT_EQ(chosen(console, 1), Kind::local);
  EXPECT_EQ(chosen(console, 2), Kind::port_guessed);
  EXPECT_EQ(chosen(console, 76), Kind::port_guessed);
  EXPECT_EQ(chosen(console, 77), Kind::public_address);
  console.candidates = {offered(holepunch::CandidateType::derived)};
  EXPECT_EQ(chosen(console, 0), Kind::discovered);
}

TEST_F(InternetRouteProgress, ReleasesHeldRoutesInOrderToOneWatcher) {
  play::InternetStart::HeldRoutes held{LiveProviders::threading()};
  HeardRoutes watcher;
  held.release_to(&watcher);
  EXPECT_TRUE(watcher.heard().empty());
  held.on_route({Route::Step::exchanging_offers, Route::Purpose::stream,
                 Route::Candidates{1, 1, 0}});
  held.on_route({Route::Step::testing_candidates, Route::Purpose::stream,
                 Route::Candidates{2, 2, 0}});
  held.release_to(nullptr);
  held.release_to(&watcher);
  EXPECT_TRUE(watcher.heard().empty());
  held.on_route({Route::Step::route_chosen, Route::Purpose::stream,
                 std::nullopt, Route::Kind::local});
  held.on_route({Route::Step::confirming_route, Route::Purpose::stream,
                 std::nullopt, Route::Kind::local});
  held.release_to(&watcher);
  held.release_to(&watcher);
  ASSERT_EQ(watcher.heard().size(), 2U);
  EXPECT_EQ(watcher.heard().front().step, Route::Step::route_chosen);
  EXPECT_EQ(watcher.heard().back().step, Route::Step::confirming_route);
}

}  // namespace
}  // namespace play_test
