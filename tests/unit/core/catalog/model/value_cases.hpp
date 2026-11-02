#pragma once

#include "tests/support/enum_printing.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace catalog_test {

class ValueCases : public ::testing::Test {
 protected:
  using Time = std::chrono::sys_time<std::chrono::milliseconds>;
  using MyGame = libchiaking::catalog::MyGame;
  using OwnedTitle = libchiaking::catalog::OwnedTitle;
  using Platform = libchiaking::catalog::Platform;
  using PlayPath = libchiaking::catalog::PlayPath;
  using PlusInclusion = libchiaking::catalog::PlusInclusion;
  using ProductId = libchiaking::catalog::ProductId;
  using ServiceGeneration = libchiaking::catalog::ServiceGeneration;
  using Title = libchiaking::catalog::Title;
  using Visibility = libchiaking::catalog::Visibility;
  using Source = PlusInclusion::Source;

  static constexpr std::string_view rivet =
      "EP0109-PPSA07028_00-FAKE0110TITLE000";
  static constexpr std::string_view rivet_deluxe =
      "EP0109-PPSA07028_00-FAKE0109TITLE000";
  static constexpr std::string_view show =
      "UP0114-PPSA37004_00-FAKE0071TITLE000";
  static constexpr std::string_view god_of_war =
      "UP0114-CUSA07009_00-FAKE0051TITLE000";
  static constexpr std::string_view bus =
      "EP0106-PPSA07037_00-FAKE0016TITLE000";
  static constexpr libchiaking::AccountId account{42};

  static auto at_ms(std::int64_t value) -> Time {
    return Time{std::chrono::milliseconds{value}};
  }

  static auto product(std::string_view text) -> ProductId {
    auto parsed = ProductId::parse(text);
    EXPECT_TRUE(parsed) << text;
    return std::move(parsed).value();
  }

  static auto title(std::string_view id, std::string_view name,
                    Platform platform, ServiceGeneration service) -> Title {
    Title::Facts facts{.product = product(id)};
    facts.name = std::string{name};
    facts.platform = platform;
    facts.service = service;
    facts.streamability = libchiaking::catalog::ListStreamability::listed;
    return Title::from(std::move(facts)).value();
  }

  static auto owned(std::string_view id, Platform platform,
                    ServiceGeneration service,
                    libchiaking::catalog::HeldAs held =
                        libchiaking::catalog::HeldAs::full_game) -> OwnedTitle {
    OwnedTitle::Facts facts{.id =
                                *libchiaking::catalog::OwnershipId::parse(id)};
    facts.name = "Owned";
    facts.platform = platform;
    facts.service = service;
    facts.held = held;
    facts.product = product(id);
    return OwnedTitle::from(std::move(facts)).value();
  }

  static auto game(Title listed, PlusInclusion inclusion, PlayPath path,
                   std::optional<OwnedTitle> ownership = std::nullopt,
                   Visibility visibility = Visibility::visible())
      -> libchiaking::Expected<MyGame, MyGame::Invalid> {
    auto launch =
        libchiaking::catalog::LaunchIdentifier::of(listed.product_id());
    MyGame::Facts facts{
        .title = std::move(listed),
        .inclusion = inclusion,
        .path = path,
        .visibility = std::move(visibility),
        .launch_identifier = std::move(launch),
    };
    facts.ownership = std::move(ownership);
    return MyGame::from(std::move(facts));
  }

  static auto owned_rivet() -> MyGame {
    return game(title(rivet, "Rivet", Platform::ps5,
                      ServiceGeneration::ps5_cloud),
                PlusInclusion::not_included(Source::store_tags),
                PlayPath::owned,
                owned(rivet, Platform::ps5, ServiceGeneration::ps5_cloud))
        .value();
  }
};

}  // namespace catalog_test
