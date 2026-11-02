#include "src/core/catalog/usecases/show_details.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/catalog/details_rig.hpp"

#include <libchiaking/testing/manual_clock.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace catalog_test {
namespace {

using libchiaking::catalog::CacheId;
using libchiaking::catalog::Catalog;
using libchiaking::catalog::ImageHost;
using libchiaking::catalog::usecases::ShowDetails;
using Reason = libchiaking::catalog::ImageReason;
using Reply = DetailsRig::Reply;
using Answer =
    std::optional<libchiaking::Expected<DetailsRig::Bytes,
                                        libchiaking::catalog::ImageFailure>>;

class OversizedStore final : public libchiaking::platform::HttpProvider {
 public:
  auto perform(const libchiaking::platform::HttpRequest& request,
               const libchiaking::platform::StopToken&)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    ++requests;
    limit = request.maximum_response_bytes;
    authorised = std::ranges::any_of(
        request.headers, [](const libchiaking::platform::HttpHeader& header) {
          return header.name == "Authorization";
        });
    return libchiaking::unexpected(
        libchiaking::platform::HttpError::response_too_large);
  }

  int requests{};
  std::size_t limit{};
  bool authorised{};
};

class GatedImages final : public libchiaking::platform::HttpProvider {
 public:
  auto perform(const libchiaking::platform::HttpRequest& request,
               const libchiaking::platform::StopToken& stop)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    libchiaking::platform::Lock lock{mutex_};
    asked_.emplace_back(request.url.view());
    most_ = std::max(most_, ++inside_);
    changed_.notify_all();
    if (!changed_.wait(lock, stop, [this] { return open_; })) {
      --inside_;
      changed_.notify_all();
      return libchiaking::unexpected(
          libchiaking::platform::HttpError::cancelled);
    }
    --inside_;
    changed_.notify_all();
    libchiaking::platform::HttpResponse response;
    response.status_code = 200;
    response.body.assign(request.url.view());
    return response;
  }

  void open(bool open) {
    const libchiaking::platform::Lock lock{mutex_};
    open_ = open;
    changed_.notify_all();
  }

  void wait_for_inside(std::size_t wanted) {
    libchiaking::platform::Lock lock{mutex_};
    EXPECT_TRUE(changed_.wait_for(lock, std::chrono::seconds{5},
                                  [&] { return inside_ == wanted; }))
        << wanted << " downloads never ran at once";
  }

  auto inside() const -> std::size_t {
    const libchiaking::platform::Lock lock{mutex_};
    return inside_;
  }

  auto most() const -> std::size_t {
    const libchiaking::platform::Lock lock{mutex_};
    return most_;
  }

  auto asked(std::string_view url) const -> int {
    const libchiaking::platform::Lock lock{mutex_};
    return static_cast<int>(std::ranges::count(asked_, url));
  }

  auto asked() const -> std::size_t {
    const libchiaking::platform::Lock lock{mutex_};
    return asked_.size();
  }

 private:
  libchiaking::platform::DefaultThreadingProvider threading_;
  libchiaking::platform::SteadyClock steady_;
  mutable libchiaking::platform::Mutex mutex_{threading_};
  libchiaking::platform::ConditionVariable changed_{threading_, steady_};
  std::vector<std::string> asked_;
  std::size_t inside_{};
  std::size_t most_{};
  bool open_{};
};

class GameImages : public ::testing::Test {
 protected:
  static auto image(std::string_view url) -> libchiaking::catalog::ImageRef {
    return DetailsRig::image(url);
  }

  static auto bytes_of(std::string_view text) -> DetailsRig::Bytes {
    return DetailsRig::bytes_of(text);
  }

  DetailsRig rig;
};

TEST_F(GameImages, AreFetchedOnceForTwoAskersAtOnce) {
  rig.http.on("GET", std::string{DetailsRig::logo_file}, {Reply{200, "logo"}});
  rig.http.hold(std::string{DetailsRig::logo_file});
  Answer left;
  Answer right;
  auto third = DetailsRig::started(rig.threading, [&] {
    left = rig.catalog.image_of(image(DetailsRig::concept_logo));
  });
  rig.http.wait_until_held();
  auto fourth = DetailsRig::started(rig.threading, [&] {
    right = rig.catalog.image_of(image(DetailsRig::concept_logo));
  });
  DetailsRig::pause_for(rig.threading, std::chrono::milliseconds{50});
  rig.http.release();
  static_cast<void>(third.join());
  static_cast<void>(fourth.join());
  ASSERT_TRUE(left);
  ASSERT_TRUE(*left);
  ASSERT_TRUE(right);
  ASSERT_TRUE(*right);
  EXPECT_EQ(left->value(), bytes_of("logo"));
  EXPECT_EQ(right->value(), bytes_of("logo"));
  EXPECT_EQ(rig.http.count(DetailsRig::logo_file), 1);
}

TEST_F(GameImages, AreFetchedAtTheSameTimeAndKept) {
  rig.http.on("GET", std::string{DetailsRig::logo_file}, {Reply{200, "logo"}});
  rig.http.on("GET", std::string{DetailsRig::hero_file}, {Reply{200, "hero!"}});
  rig.http.on("GET", std::string{DetailsRig::background_file},
              {Reply{200, "back.."}});
  const std::array wanted{
      std::pair{image(DetailsRig::concept_logo), std::string{"logo"}},
      std::pair{image(DetailsRig::concept_hero), std::string{"hero!"}},
      std::pair{image(DetailsRig::concept_background), std::string{"back.."}}};
  const auto asked = rig.http.requests();
  GatedStore gate{rig.http, static_cast<int>(wanted.size())};
  auto catalog = rig.restarted_over(gate);
  std::array<Answer, 3> got;
  {
    std::vector<libchiaking::platform::Thread> askers;
    for (std::size_t at = 0; at < wanted.size(); ++at)
      askers.push_back(DetailsRig::started(rig.threading, [&, at] {
        got[at] = catalog.image_of(wanted[at].first);
      }));
  }
  EXPECT_FALSE(gate.waited_alone());
  EXPECT_EQ(gate.arrived(), 3);
  for (std::size_t at = 0; at < wanted.size(); ++at) {
    ASSERT_TRUE(got[at]) << at;
    ASSERT_TRUE(*got[at]) << at;
    EXPECT_EQ(got[at]->value(), bytes_of(wanted[at].second)) << at;
  }
  const auto held = catalog.cache_entries(CacheId::images);
  ASSERT_TRUE(held);
  EXPECT_EQ(held.value().size(), 3U);

  auto restarted = rig.restarted();
  for (const auto& [picture, pixels] : wanted) {
    const auto kept = restarted.image_of(picture);
    ASSERT_TRUE(kept) << pixels;
    EXPECT_EQ(kept.value(), bytes_of(pixels));
  }
  EXPECT_EQ(rig.http.requests(), asked + 3);
}

TEST_F(GameImages, AreDownloadedOnceAndThenServedFromTheCore) {
  const std::string pixels{"\x89PNG\0\x01\x02pixels", 13};
  rig.http.on("GET", std::string{DetailsRig::logo_file}, {Reply{200, pixels}});
  const auto asked = rig.http.requests();
  const auto first = rig.catalog.image_of(image(DetailsRig::concept_logo));
  ASSERT_TRUE(first);
  EXPECT_EQ(first.value(), bytes_of(pixels));
  EXPECT_EQ(rig.http.requests(), asked + 1);
  const auto seen = rig.http.seen();
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen.back().method, "GET");
  EXPECT_EQ(seen.back().url, DetailsRig::concept_logo);
  EXPECT_TRUE(rig.sent_no_credential());

  const auto second = rig.catalog.image_of(image(DetailsRig::concept_logo));
  ASSERT_TRUE(second);
  EXPECT_EQ(second.value(), bytes_of(pixels));
  auto restarted = rig.restarted();
  const auto after_restart =
      restarted.image_of(image(DetailsRig::concept_logo));
  ASSERT_TRUE(after_restart);
  EXPECT_EQ(after_restart.value(), bytes_of(pixels));
  EXPECT_EQ(rig.http.requests(), asked + 1);
}

TEST_F(GameImages, ReportWhyTheyCouldNotBeFetchedAndKeepNothing) {
  rig.http.on("GET", std::string{DetailsRig::logo_file}, {Reply{200, "logo"}});
  ASSERT_TRUE(rig.catalog.image_of(image(DetailsRig::concept_logo)));
  rig.http.on("GET", std::string{DetailsRig::hero_file}, {Reply{404, "gone"}});
  const auto missing = rig.catalog.image_of(image(DetailsRig::concept_hero));
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().reason(), Reason::not_listed);
  rig.http.on("GET", std::string{DetailsRig::background_file},
              {Reply{200, ""}});
  const auto empty =
      rig.catalog.image_of(image(DetailsRig::concept_background));
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error().reason(), Reason::malformed);
  const auto unscripted = rig.catalog.image_of(image(DetailsRig::rating_icon));
  ASSERT_FALSE(unscripted);
  EXPECT_EQ(unscripted.error().reason(), Reason::unavailable);
  const auto held = rig.catalog.cache_entries(CacheId::images);
  ASSERT_TRUE(held);
  ASSERT_EQ(held.value().size(), 1U);
  EXPECT_EQ(held.value().front().image, image(DetailsRig::concept_logo));
}

TEST_F(GameImages, AreDroppedWhenRefreshedDetailsNoLongerShowThem) {
  rig.script_whole_store();
  rig.http.on("GET", std::string{DetailsRig::hero_file}, {Reply{200, "hero"}});
  rig.http.on("GET", std::string{DetailsRig::logo_file}, {Reply{200, "logo"}});
  const auto user = rig.user();
  EXPECT_TRUE(
      rig.catalog.details_of(*user, DetailsRig::product(DetailsRig::remaster)));
  EXPECT_TRUE(rig.catalog.image_of(image(DetailsRig::concept_hero)));
  EXPECT_TRUE(rig.catalog.image_of(image(DetailsRig::concept_logo)));

  rig.advance(std::chrono::hours{25});
  rig.the_store_answers(
      DetailsRig::sold_lacking({{"concept", "HERO_CHARACTER"},
                                {"concept", "GAMEHUB_COVER_ART"},
                                {"product", "GAMEHUB_COVER_ART"}}));
  const auto refreshed =
      rig.catalog.details_of(*user, DetailsRig::product(DetailsRig::remaster));
  ASSERT_TRUE(refreshed);
  EXPECT_FALSE(refreshed.value().key_art);
  const auto images = rig.catalog.cache_entries(CacheId::images);
  ASSERT_TRUE(images);
  EXPECT_TRUE(images.value().empty());
}

TEST(GameImagesOverTheSizeCap, AreRefusedWithoutACredential) {
  OversizedStore store;
  libchiaking::platform::DefaultThreadingProvider threading;
  libchiaking::platform::SteadyClock steady;
  libchiaking::testing::InMemoryRecordStore records{threading};
  libchiaking::testing::ManualClock wall;
  libchiaking::users::RequestBudget budget{threading, steady};
  Catalog catalog{libchiaking::testing::ServiceBuilder::build<Catalog>(
      store, records, threading, steady, wall, budget)};
  const auto refused =
      catalog.image_of(DetailsRig::image(DetailsRig::concept_hero));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().reason(), Reason::rejected);
  EXPECT_EQ(store.requests, 1);
  EXPECT_EQ(store.limit, ImageHost::largest_bytes);
  EXPECT_FALSE(store.authorised);
  EXPECT_EQ(ImageHost::largest_bytes, 16U * 1024U * 1024U);
}

class GameImageDownloads : public ::testing::Test {
 protected:
  static auto cover(std::size_t number) -> std::string {
    return "https://image.api.playstation.com/cover/" + std::to_string(number) +
           ".png";
  }

  static auto image(std::size_t number) -> libchiaking::catalog::ImageRef {
    return DetailsRig::image(cover(number));
  }

  GatedImages ps;
  libchiaking::platform::DefaultThreadingProvider threading;
  libchiaking::platform::SteadyClock steady;
  libchiaking::testing::InMemoryRecordStore records{threading};
  libchiaking::testing::ManualClock wall;
  libchiaking::users::RequestBudget budget{threading, steady};
  Catalog catalog{libchiaking::testing::ServiceBuilder::build<Catalog>(
      ps, records, threading, steady, wall, budget)};
};

TEST_F(GameImageDownloads, RunFourAtOnceForFiftyCovers) {
  constexpr std::size_t covers = 50;
  constexpr std::size_t duplicates = 10;
  std::array<Answer, covers + duplicates> got;
  {
    std::vector<libchiaking::platform::Thread> tiles;
    for (std::size_t at = 0; at < covers + duplicates; ++at)
      tiles.push_back(DetailsRig::started(threading, [&, at] {
        got[at] = catalog.image_of(image(at % covers));
      }));
    ps.wait_for_inside(ShowDetails::downloads_at_once);
    DetailsRig::pause_for(threading, std::chrono::milliseconds{50});
    EXPECT_EQ(ps.inside(), ShowDetails::downloads_at_once);
    ps.open(true);
  }
  EXPECT_EQ(ShowDetails::downloads_at_once, 4U);
  EXPECT_EQ(ps.most(), ShowDetails::downloads_at_once);
  EXPECT_EQ(ps.asked(), covers);
  for (std::size_t at = 0; at < covers + duplicates; ++at) {
    ASSERT_TRUE(got[at]) << at;
    ASSERT_TRUE(*got[at]) << at;
    EXPECT_EQ(got[at]->value(), DetailsRig::bytes_of(cover(at % covers))) << at;
  }
  for (std::size_t at = 0; at < covers; ++at)
    EXPECT_EQ(ps.asked(cover(at)), 1) << at;
}

TEST_F(GameImageDownloads, SkipAHeldCoverAndNeverStartAScrolledAwayOne) {
  ps.open(true);
  EXPECT_TRUE(catalog.image_of(image(99)));
  ps.open(false);

  const auto busy = ShowDetails::downloads_at_once;
  auto waiting = std::make_shared<test_support::AtomicStop>();
  auto in_flight = std::make_shared<test_support::AtomicStop>();
  Answer queued;
  Answer aborted;
  {
    std::vector<libchiaking::platform::Thread> tiles;
    tiles.push_back(DetailsRig::started(threading, [&] {
      aborted = catalog.image_of(image(0),
                                 libchiaking::platform::StopToken{in_flight});
    }));
    for (std::size_t at = 1; at < busy; ++at)
      tiles.push_back(DetailsRig::started(
          threading, [&, at] { (void)catalog.image_of(image(at)); }));
    ps.wait_for_inside(ShowDetails::downloads_at_once);
    auto scrolled = DetailsRig::started(threading, [&] {
      queued = catalog.image_of(image(busy),
                                libchiaking::platform::StopToken{waiting});
    });
    DetailsRig::pause_for(threading, std::chrono::milliseconds{50});

    const auto held = catalog.image_of(image(99));
    EXPECT_TRUE(held);
    EXPECT_EQ(held.value_or(DetailsRig::Bytes{}),
              DetailsRig::bytes_of(cover(99)));
    EXPECT_EQ(ps.asked(cover(99)), 1);

    waiting->stopped.store(true);
    static_cast<void>(scrolled.join());
    in_flight->stopped.store(true);
    ps.wait_for_inside(ShowDetails::downloads_at_once - 1);
    ps.open(true);
  }
  ASSERT_TRUE(queued);
  ASSERT_FALSE(*queued);
  EXPECT_TRUE(queued->error().is_cancelled());
  ASSERT_TRUE(aborted);
  ASSERT_FALSE(*aborted);
  EXPECT_TRUE(aborted->error().is_cancelled());
  EXPECT_EQ(ps.asked(cover(busy)), 0);
  EXPECT_EQ(ps.asked(cover(0)), 1);
  const auto held = catalog.cache_entries(CacheId::images);
  ASSERT_TRUE(held);
  EXPECT_EQ(held.value().size(), busy);
}

}  // namespace
}  // namespace catalog_test
