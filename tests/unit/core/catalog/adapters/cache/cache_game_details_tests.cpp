#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/adapters/cache/shelf_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace catalog_test {
namespace {

namespace cache = libchiaking::catalog::cache;

class CachedGameDetails : public ::testing::Test, protected CacheCases {
 protected:
  static constexpr std::string_view name = "details/en-gb/concept-90000016";

  auto concept_details() const -> libchiaking::catalog::GameDetails {
    return libchiaking::catalog::GameDetails{
        .concept_id = libchiaking::catalog::ConceptId::parse("90000016"),
        .locale = locale("en-GB"),
        .fetched_at = shelf.now() - std::chrono::minutes{5}};
  }

  auto read_back(const libchiaking::catalog::GameDetails& details)
      -> std::optional<libchiaking::catalog::GameDetails> {
    const auto read = shelf.cache.read<cache::GameDetailsRecord>(
        cache::GameDetailsRecord{details}.key());
    EXPECT_TRUE(read);
    if (!read || !read.value()) return std::nullopt;
    return read.value()->details();
  }

  CacheShelf shelf;
};

TEST_F(CachedGameDetails, IsWrittenByteForByteWithLayeredArt) {
  libchiaking::catalog::GameDetails details{
      .product = product(rivet),
      .concept_id = libchiaking::catalog::ConceptId::parse("90000016"),
      .locale = locale("en-GB"),
      .fetched_at = shelf.now() - std::chrono::minutes{5}};
  details.key_art =
      libchiaking::catalog::KeyArt{*libchiaking::catalog::LayeredArt::of(
          image("https://image.api.playstation.com/bg.png"),
          image("https://image.api.playstation.com/hero.png"),
          image("https://image.api.playstation.com/logo.png"))};
  details.screenshots = {image("https://image.api.playstation.com/shot1.jpg")};
  details.trailers = {*libchiaking::catalog::TrailerRef::at(
      "https://vulcan.dl.playstation.net/trailer.mp4")};
  details.star_rating =
      libchiaking::catalog::StarRating{4.37, "4.37", 12345, {70, 15, 5, 3, 7}};
  details.age_rating = libchiaking::catalog::AgeRating{
      "PEGI",
      "PEGI 7",
      "Mild violence \"cartoon\"",
      image("https://image.api.playstation.com/pegi7.png"),
      {"Violence", "Online"}};
  details.publisher = "Example Publisher \xC3\xA9";
  details.released_at = shelf.now() - std::chrono::hours{24 * 400};
  details.genres = {"Action", "Adventure"};
  ASSERT_TRUE(shelf.cache.write(cache::GameDetailsRecord{details}));
  EXPECT_EQ(
      shelf.text(name),
      R"({"record":"details","schema":1,"writtenAtMs":1800000000000,"region":"e)"
      R"(n-GB","body":{"product":"EP0109-PPSA07028_00-FAKE0110TITLE000","concep)"
      R"(t":"90000016","fetchedAtMs":1799999700000,"layeredArt":{"background":")"
      R"(https://image.api.playstation.com/bg.png","hero":"https://image.api.pl)"
      R"(aystation.com/hero.png","logo":"https://image.api.playstation.com/logo)"
      R"(.png"},"screenshots":["https://image.api.playstation.com/shot1.jpg"],")"
      R"(trailers":["https://vulcan.dl.playstation.net/trailer.mp4"],"starRatin)"
      R"(g":{"average":4.37,"averageText":"4.37","ratings":12345,"percentByStar)"
      R"(s":[{"percent":70},{"percent":15},{"percent":5},{"percent":3},{"percen)"
      R"(t":7}]},"ageRating":{"authority":"PEGI","name":"PEGI 7","description":)"
      R"("Mild violence \"cartoon\"","icon":"https://image.api.playstation.com/)"
      R"(pegi7.png","descriptors":["Violence","Online"]},"publisher":"Example Publisher é","releasedAtMs":1765440000000,"genres":["Actio)"
      R"(n","Adventure"]}})");
  EXPECT_EQ(read_back(details), details);
}

TEST_F(CachedGameDetails, RoundTripsGameHubArt) {
  auto details = concept_details();
  details.key_art =
      libchiaking::catalog::KeyArt{*libchiaking::catalog::GameHubArt::of(
          image("https://image.api.playstation.com/cover.png"),
          image("https://image.api.playstation.com/logo.png"))};
  ASSERT_TRUE(shelf.cache.write(cache::GameDetailsRecord{details}));
  EXPECT_EQ(
      shelf.text(name),
      R"({"record":"details","schema":1,"writtenAtMs":1800000000000,"region":"en-GB",)"
      R"("body":{"concept":"90000016","fetchedAtMs":1799999700000,"gameHubArt":{"cover":)"
      R"("https://image.api.playstation.com/cover.png","logo":"https://image.api.playstati)"
      R"(on.com/logo.png"},"screenshots":[],"trailers":[],"publisher":"","genres":[]}})");
  EXPECT_EQ(read_back(details), details);
}

TEST_F(CachedGameDetails, RoundTripsGameHubArtWithoutALogo) {
  auto details = concept_details();
  details.key_art =
      libchiaking::catalog::KeyArt{*libchiaking::catalog::GameHubArt::of(
          image("https://image.api.playstation.com/cover.png"), std::nullopt)};
  ASSERT_TRUE(shelf.cache.write(cache::GameDetailsRecord{details}));
  EXPECT_NE(
      shelf.text(name).find(
          R"("gameHubArt":{"cover":"https://image.api.playstation.com/cover.png"},)"),
      std::string::npos);
  EXPECT_EQ(read_back(details), details);
}

TEST_F(CachedGameDetails, LoadsOldDetailsWithoutArt) {
  shelf.put(
      name,
      R"({"record":"details","schema":1,"writtenAtMs":1800000000000,"region":"en-GB",)"
      R"("body":{"concept":"90000016","fetchedAtMs":1799999700000,"screenshots":[],)"
      R"("trailers":[],"publisher":"","genres":[]}})");
  const auto read = shelf.cache.read<cache::GameDetailsRecord>(
      cache::GameDetailsRecord{concept_details()}.key());
  ASSERT_TRUE(read);
  ASSERT_TRUE(read.value());
  EXPECT_FALSE(read.value()->details().key_art);
}

TEST_F(CachedGameDetails, LoadsOldDetailsWithLayeredArt) {
  shelf.put(
      name,
      R"({"record":"details","schema":1,"writtenAtMs":1800000000000,"region":"en-GB",)"
      R"("body":{"concept":"90000016","fetchedAtMs":1799999700000,"layeredArt":{"backgr)"
      R"(ound":"https://image.api.playstation.com/bg.png","hero":"https://image.api.play)"
      R"(station.com/hero.png","logo":"https://image.api.playstation.com/logo.png"},"scr)"
      R"(eenshots":[],"trailers":[],"publisher":"","genres":[]}})");
  const auto read = shelf.cache.read<cache::GameDetailsRecord>(
      cache::GameDetailsRecord{concept_details()}.key());
  ASSERT_TRUE(read);
  ASSERT_TRUE(read.value());
  ASSERT_TRUE(read.value()->details().key_art);
  EXPECT_TRUE(std::holds_alternative<libchiaking::catalog::LayeredArt>(
      read.value()->details().key_art->choice()));
}

TEST_F(CachedGameDetails, IsErasedWhenTheArtIsHostedElsewhere) {
  shelf.put(
      name,
      R"({"record":"details","schema":1,"writtenAtMs":1800000000000,"region":"en-GB",)"
      R"("body":{"concept":"90000016","fetchedAtMs":1799999700000,"gameHubArt":{"cover":)"
      R"("https://elsewhere.example/cover.png"},"screenshots":[],"trailers":[],)"
      R"("publisher":"","genres":[]}})");
  const auto read = shelf.cache.read<cache::GameDetailsRecord>(
      cache::GameDetailsRecord{concept_details()}.key());
  ASSERT_TRUE(read);
  EXPECT_FALSE(read.value());
  EXPECT_FALSE(shelf.has(name));
}

TEST(CachedImageIndex, IsWrittenByteForByte) {
  CacheShelf shelf;
  libchiaking::catalog::cache::ImageShelf images{shelf.cache, shelf.threading};
  const std::array<std::byte, 3> bytes{std::byte{1}, std::byte{2},
                                       std::byte{3}};
  ASSERT_TRUE(images.keep(
      CacheCases::image("https://image.api.playstation.com/cover.png"), bytes));
  EXPECT_EQ(
      shelf.text("image-index"),
      R"({"schema":1,"images":[{"url":"https://image.api.playstation.com/cover.)"
      R"(png","bytes":3,"storedAtMs":1800000000000,"usedAtMs":1800000000000}]})");
}

}  // namespace
}  // namespace catalog_test
