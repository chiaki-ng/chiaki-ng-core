#include "tests/unit/core/consoles/adapters/titles/console_library_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::consoles::Generation;
using libchiaking::consoles::InstalledTitle;
using libchiaking::users::Unanswered;
using Reason = Unanswered::Reason;
using Reply = LibraryRig::Reply;
using Titles = LibraryRig::Titles;

class ConsoleLibraryClients : public ::testing::Test {
 protected:
  LibraryRig rig;
};

TEST_F(ConsoleLibraryClients, AskTheExactUrls) {
  rig.clients(
      0,
      {{200, LibraryRig::clients_page({LibraryRig::client(
                 "a", "A", {LibraryRig::installed("NEVER0001_00", "PS5")})})}});
  rig.games(0, {{200, LibraryRig::gamelist(LibraryRig::filler_games(0, 50))}});
  rig.games(50, {{200, LibraryRig::gamelist({})}});
  EXPECT_TRUE(rig.run());
  EXPECT_TRUE(rig.http.unmatched.empty());
  ASSERT_EQ(rig.http.seen.size(), 3U);
  EXPECT_EQ(
      rig.http.seen[0].url,
      "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/"
      "clients?platform=PS5&includeFields=device,systemData&limit=10&offset=0");
  EXPECT_EQ(rig.http.seen[2].url,
            "https://m.np.playstation.com/api/gamelist/v2/users/me/"
            "titles?limit=50&offset=50");
}

TEST_F(ConsoleLibraryClients, MayBeNoneAndAreAskedWithTheBearer) {
  rig.clients(0, {{200, LibraryRig::clients_page({})}});
  rig.games(0, {{200, LibraryRig::gamelist({})}});
  const auto result = rig.run();
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.value()->consoles.empty());
  ASSERT_EQ(rig.http.seen.size(), 2U);
  for (const auto& seen : rig.http.seen) {
    EXPECT_EQ(seen.method, "GET") << seen.url;
    EXPECT_EQ(seen.header("Authorization"),
              "Bearer " + std::string{LibraryRig::token})
        << seen.url;
    EXPECT_EQ(seen.header("Accept"), "application/json") << seen.url;
    EXPECT_TRUE(seen.body.empty()) << seen.url;
  }
  EXPECT_NE(rig.http.seen[1].url.find("limit=50&offset=0"), std::string::npos);
  EXPECT_EQ(rig.http.seen[1].url.find("categories"), std::string::npos);
  EXPECT_TRUE(rig.leaks_no_token());
}

TEST_F(ConsoleLibraryClients, ArePagedThroughUntilAShortPage) {
  rig.clients(0, {{200, LibraryRig::clients_page(LibraryRig::ten_clients(0))}});
  rig.clients(
      10,
      {{200, LibraryRig::clients_page({LibraryRig::client("duid-10", "PS5-10"),
                                       LibraryRig::client("duid-11", "")})}});
  rig.games(0, {{200, LibraryRig::gamelist({})}});
  const auto result = rig.run();
  ASSERT_TRUE(result);
  EXPECT_EQ(rig.http.count("/clients?"), 2);
  EXPECT_TRUE(rig.http.unmatched.empty());
  EXPECT_EQ(result.value()->consoles.size(), 12U);
  EXPECT_TRUE(LibraryRig::shelf(result, "duid-0"));
  const auto* last = LibraryRig::shelf(result, "duid-11");
  ASSERT_TRUE(last);
  EXPECT_TRUE(last->name.empty());
  EXPECT_TRUE(last->titles.empty());
}

TEST_F(ConsoleLibraryClients, StopAtAShortFirstPage) {
  rig.clients(
      0, {{200, LibraryRig::clients_page({LibraryRig::client("a", "A")})}});
  rig.games(0, {{200, LibraryRig::gamelist({})}});
  EXPECT_TRUE(rig.run());
  EXPECT_EQ(rig.http.count("/clients?"), 1);
}

TEST_F(ConsoleLibraryClients, StopWhenTheOffsetIsIgnored) {
  auto page = LibraryRig::ten_clients(0);
  page[1] = LibraryRig::client(" duid-0 ", "dup");
  rig.clients(0, {{200, LibraryRig::clients_page(page)}});
  rig.clients(10, {{200, LibraryRig::clients_page(page)}});
  rig.games(0, {{200, LibraryRig::gamelist({})}});
  const auto result = rig.run();
  ASSERT_TRUE(result);
  EXPECT_EQ(rig.http.count("/clients?"), 2);
  for (const auto& console : result.value()->consoles)
    EXPECT_NE(console.name, "dup") << console.duid;
}

TEST_F(ConsoleLibraryClients, StopAtAnEmptyPage) {
  rig.clients(0, {{200, LibraryRig::clients_page(LibraryRig::ten_clients(0))}});
  rig.clients(10, {{200, LibraryRig::clients_page({})}});
  rig.games(0, {{200, LibraryRig::gamelist({})}});
  EXPECT_TRUE(rig.run());
  EXPECT_EQ(rig.http.count("/clients?"), 2);
}

TEST(ConsoleLibraryClientPages, FailEverythingWhenOneFails) {
  struct Case {
    Reply later;
    Reason reason;
  };
  const std::vector<Case> cases{
      {Reply{500, "oops"}, Reason::unavailable},
      {Reply{200, "not json"}, Reason::malformed},
      {Reply{200, "{\"other\":[]}"}, Reason::malformed},
      {Reply{200, ""}, Reason::malformed},
      {Reply::transport(), Reason::unavailable}};
  for (std::size_t index = 0; index < cases.size(); ++index) {
    const auto& entry = cases[index];
    LibraryRig rig;
    rig.clients(0,
                {{200, LibraryRig::clients_page(LibraryRig::ten_clients(0))}});
    rig.clients(10, {entry.later});
    rig.games(0, {{200, LibraryRig::gamelist({})}});
    const auto result = rig.run();
    ASSERT_FALSE(result) << index;
    EXPECT_EQ(result.error().reason, entry.reason) << index;
    EXPECT_EQ(rig.http.count("/gamelist/"), 0) << index;
    EXPECT_TRUE(rig.leaks_no_token()) << index;
  }
}

TEST(ConsoleLibraryAuthRefusal, EndsAsLapsedStoreAccess) {
  for (const int status : {401, 403}) {
    LibraryRig rig;
    rig.account.renews_store_access(LibraryRig::renewed_token);
    rig.clients(0,
                {{status, "{\"error\":\"invalid token " +
                              std::string{LibraryRig::token} + "\"}"},
                 {status, "{\"error\":\"invalid token " +
                              std::string{LibraryRig::renewed_token} + "\"}"}});
    const auto result = rig.run();
    ASSERT_FALSE(result) << status;
    EXPECT_EQ(result.error().reason, Reason::store_access_lapsed) << status;
    EXPECT_EQ(result.error().http_status, status) << status;
    EXPECT_EQ(rig.http.count("/clients?"), 2) << status;
    EXPECT_EQ(rig.http.seen.back().header("Authorization"),
              "Bearer " + std::string{LibraryRig::renewed_token})
        << status;
    EXPECT_TRUE(rig.leaks_no_token()) << status;
  }
}

TEST_F(ConsoleLibraryClients, EndAsLapsedStoreAccessWhenTheGamelistRefuses) {
  rig.account.renews_store_access(LibraryRig::renewed_token);
  rig.clients(
      0,
      {{200, LibraryRig::clients_page({LibraryRig::client(
                 "a", "A", {LibraryRig::installed("PPSA00001_00", "PS5")})})}});
  rig.games(0, {{200, LibraryRig::gamelist(LibraryRig::filler_games(0, 50))}});
  rig.games(50, {{401, ""}});
  const auto result = rig.run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Reason::store_access_lapsed);
  EXPECT_TRUE(rig.leaks_no_token());
}

TEST_F(ConsoleLibraryClients, ParseTheInstalledTitles) {
  const std::string body = LibraryRig::clients_page(
      {LibraryRig::client("d1", "Living room",
                          {LibraryRig::installed(" ppsa00001_00", "PS5"),
                           LibraryRig::installed("PPSA00001_00", "PS5"),
                           LibraryRig::installed("", "PS5"),
                           LibraryRig::installed("CUSA00002_00", "PS4")}),
       LibraryRig::client("  ", "blank"), "{\"duid\":\"d2\"}", "7"});
  rig.clients(0, {{200, body}});
  rig.games(0,
            {{200, LibraryRig::gamelist(
                       {LibraryRig::game("PPSA00001_00", "One"),
                        LibraryRig::game("CUSA00002_00", "Two",
                                         "https://img/b.png", "ps4_game")})}});
  const auto result = rig.run();
  ASSERT_TRUE(result);
  EXPECT_EQ(rig.http.count("/clients?"), 1);
  const auto& consoles = result.value()->consoles;
  ASSERT_EQ(consoles.size(), 2U);
  EXPECT_EQ(consoles[0].duid, "d1");
  EXPECT_EQ(consoles[0].name, "Living room");
  EXPECT_FALSE(consoles[0].device_uid);
  EXPECT_EQ(
      consoles[0].titles,
      (Titles{LibraryRig::title("PPSA00001_00", "PPSA00001", "One",
                                "https://img/a.png", Generation::ps5,
                                LibraryRig::at_ms(1767290400000), true),
              LibraryRig::title("CUSA00002_00", "CUSA00002", "Two",
                                "https://img/b.png", Generation::ps4,
                                LibraryRig::at_ms(1767290400000), true)}));
  EXPECT_EQ(consoles[1].duid, "d2");
  EXPECT_TRUE(consoles[1].name.empty());
  EXPECT_TRUE(consoles[1].titles.empty());
}

TEST_F(ConsoleLibraryClients, MatchTheGoldenTypedAnswer) {
  rig.clients(
      0,
      {{200,
        LibraryRig::clients_page(
            {LibraryRig::client("ABCDEF0123", "Living \\\"Room\\\"",
                                {LibraryRig::installed("PPSA01234_00", "PS5"),
                                 LibraryRig::installed("CUSA07014_00", "PS4"),
                                 LibraryRig::installed("PPSA07055_00", "PS5")}),
             LibraryRig::client("SECOND", "Bedroom")})}});
  rig.games(
      0,
      {{200, LibraryRig::gamelist(
                 {LibraryRig::game("PPSA01234_00", "Game", "https://img/g.png",
                                   "ps5_native_game", "2026-01-01T18:00:00Z"),
                  LibraryRig::game("CUSA07014_00", "Old", "https://img/o.png",
                                   "ps4_game", "2026-01-01T12:00:00.000Z"),
                  LibraryRig::game("PPSA07055_00", "TV", "https://img/t.png",
                                   "ps5_native_media_app",
                                   "2026-01-01T12:00:00Z")})}});
  const auto result = rig.run();
  ASSERT_TRUE(result);
  const auto& consoles = result.value()->consoles;
  ASSERT_EQ(consoles.size(), 2U);
  EXPECT_EQ(consoles[0].duid, "ABCDEF0123");
  EXPECT_EQ(consoles[0].name, "Living \"Room\"");
  EXPECT_EQ(
      consoles[0].titles,
      (Titles{LibraryRig::title("PPSA01234_00", "PPSA01234", "Game",
                                "https://img/g.png", Generation::ps5,
                                LibraryRig::at_ms(1767290400000), true),
              LibraryRig::title("CUSA07014_00", "CUSA07014", "Old",
                                "https://img/o.png", Generation::ps4,
                                LibraryRig::at_ms(1767268800000), true)}));
  EXPECT_EQ(consoles[1].duid, "SECOND");
  EXPECT_EQ(consoles[1].name, "Bedroom");
  EXPECT_TRUE(consoles[1].titles.empty());
}

}  // namespace
}  // namespace consoles_test
