#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/rule_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace catalog_test {
namespace {

using libchiaking::catalog::LaunchRefusal;
using libchiaking::catalog::MyGame;
using libchiaking::catalog::Platform;
using libchiaking::catalog::PlayPath;
using libchiaking::catalog::ReleaseDate;
using libchiaking::catalog::ServiceGeneration;
using State = ReleaseDate::State;
using Time = ReleaseDate::Time;

class MyGameRelease : public ::testing::Test, protected RuleCases {
 protected:
  static auto at(std::int64_t value) -> Time {
    return Time{std::chrono::milliseconds{value}};
  }

  static auto refusal_kind(ReleaseDate release, std::optional<Time> now)
      -> std::optional<LaunchRefusal::Kind> {
    const auto refusal = LaunchRefusal::of_release(release, now);
    return refusal ? std::optional{refusal->kind()} : std::nullopt;
  }

  static auto title(std::string_view id, Platform platform,
                    ServiceGeneration service) -> libchiaking::catalog::Title {
    libchiaking::catalog::Title::Facts facts{.product = product(id)};
    facts.name = "A Game";
    facts.platform = platform;
    facts.service = service;
    return libchiaking::catalog::Title::from(std::move(facts)).value();
  }

  static auto owned(std::string_view id, Platform platform,
                    ServiceGeneration service, ReleaseDate release)
      -> libchiaking::catalog::OwnedTitle {
    libchiaking::catalog::OwnedTitle::Facts facts{
        .id = *libchiaking::catalog::OwnershipId::parse(id)};
    facts.name = "A Game";
    facts.platform = platform;
    facts.service = service;
    facts.held = libchiaking::catalog::HeldAs::full_game;
    facts.release = release;
    return libchiaking::catalog::OwnedTitle::from(std::move(facts)).value();
  }

  static auto game(Platform platform, ServiceGeneration service, PlayPath path,
                   std::optional<ReleaseDate> owned_release)
      -> libchiaking::Expected<MyGame, MyGame::Invalid> {
    const std::string_view id = platform == Platform::ps5
                                    ? "UP0114-PPSA07045_00-FAKE0097TITLE000"
                                    : "UP0114-CUSA07035_00-FAKE0097TITLE000";
    MyGame::Facts facts{
        .title = title(id, platform, service),
        .inclusion = libchiaking::catalog::PlusInclusion::not_included(
            libchiaking::catalog::PlusInclusion::Source::store_lists),
        .path = path,
        .visibility = libchiaking::catalog::Visibility::visible(),
        .launch_identifier =
            libchiaking::catalog::LaunchIdentifier::of(product(id)),
    };
    if (owned_release)
      facts.ownership = owned(id, platform, service, *owned_release);
    return MyGame::from(std::move(facts));
  }
};

TEST_F(MyGameRelease, IsReleasedForAnOwnedTitleWithoutADate) {
  const auto played = game(Platform::ps5, ServiceGeneration::ps5_cloud,
                           PlayPath::owned, ReleaseDate::unrestricted());
  ASSERT_TRUE(played);
  EXPECT_TRUE(played.value().release().is_unrestricted());
  for (const auto clock :
       {std::optional<Time>{at(1500)}, std::optional<Time>{}}) {
    EXPECT_EQ(played.value().release().state(clock), State::released);
    EXPECT_FALSE(LaunchRefusal::of_release(played.value().release(), clock));
  }
}

TEST_F(MyGameRelease, NeverGatesATitleThatIsNotOwned) {
  const auto included = game(Platform::ps5, ServiceGeneration::ps5_cloud,
                             PlayPath::included, std::nullopt);
  ASSERT_TRUE(included);
  EXPECT_FALSE(included.value().is_owned());
  EXPECT_TRUE(included.value().release().is_unrestricted());
  EXPECT_EQ(included.value().release().state(std::nullopt), State::released);
}

TEST_F(MyGameRelease, GatesAnOwnedOldServiceTitle) {
  const auto played = game(Platform::ps4, ServiceGeneration::old_service,
                           PlayPath::old_service, ReleaseDate::at(at(2000)));
  ASSERT_TRUE(played);
  EXPECT_TRUE(played.value().is_owned());
  EXPECT_FALSE(played.value().is_in_ps5_library());
  EXPECT_EQ(played.value().play_path(), PlayPath::old_service);
  EXPECT_EQ(played.value().release(), ReleaseDate::at(at(2000)));
  EXPECT_EQ(played.value().release().state(at(1500)), State::not_released);
  EXPECT_EQ(refusal_kind(played.value().release(), at(1500)),
            LaunchRefusal::Kind::not_released);
}

TEST_F(MyGameRelease, RefusesTheOwnedPathWithoutOwnership) {
  const auto unowned = game(Platform::ps5, ServiceGeneration::ps5_cloud,
                            PlayPath::owned, std::nullopt);
  ASSERT_FALSE(unowned);
  EXPECT_EQ(unowned.error(), MyGame::Invalid::owned_path_without_ownership);
}

TEST_F(MyGameRelease, CountsDownTheOwnedRelease) {
  const auto played = game(Platform::ps5, ServiceGeneration::ps5_cloud,
                           PlayPath::owned, ReleaseDate::at(at(2000)));
  ASSERT_TRUE(played);
  EXPECT_EQ(played.value().release().state(at(1500)), State::not_released);
  EXPECT_EQ(played.value().release().countdown(at(1500)),
            (ReleaseDate::Countdown{0, 0, 0, 1}));
}

}  // namespace
}  // namespace catalog_test
