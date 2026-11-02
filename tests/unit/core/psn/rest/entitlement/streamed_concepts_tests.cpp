#include "tests/support/psn/fixtures.hpp"
#include "tests/unit/core/psn/rest/entitlement/library_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace psn_test {
namespace {

using test_support::psn::Fixtures;

class StreamedConcepts : public LibraryFixture {};

TEST_F(StreamedConcepts, GiveEachStreamedCopyItsConcept) {
  rig.fake.on(
      "GET", concepts_url(0),
      {Reply{200, Fixtures::text("entitlements_v2_ps4gs_synthetic.json")}});
  rig.fake.on("GET", concepts_url(2), {Reply{200, copies(5, 2, 0)}});
  const auto found =
      library.concepts_of(LibraryEntitlements::streamed_ps4_games);
  ASSERT_TRUE(found);
  EXPECT_EQ(found.value().total, 5U);
  EXPECT_EQ(found.value().received, 2U);
  EXPECT_EQ(found.value().requests, 2);
  EXPECT_FALSE(found.value().truncated);
  const auto& known = found.value().known;
  ASSERT_EQ(known.size(), 2U);
  EXPECT_EQ(known[0].id, "EP0109-CUSA37002_00-FAKE0107TITLE000");
  EXPECT_EQ(known[0].product_id, "EP0109-CUSA37002_00-FAKE0050TITLE000");
  EXPECT_EQ(known[0].concept_id, "900001");
  EXPECT_EQ(known[1].id, "EP0103-CUSA07008_00-FAKE0107TITLE000");
  EXPECT_EQ(known[1].product_id, "EP0103-CUSA07008_00-FAKE0054TITLE000");
  EXPECT_EQ(known[1].concept_id, "900003");
  EXPECT_TRUE(rig.fake.unmatched.empty());
  EXPECT_EQ(rig.fake.seen.size(), 2U);
  for (const auto& seen : rig.fake.seen) {
    EXPECT_EQ(seen.method, "GET");
    EXPECT_TRUE(seen.url.starts_with("https://m.np.playstation.com/"))
        << seen.url;
    ASSERT_EQ(seen.headers.size(), 2U);
    EXPECT_EQ(seen.headers[0].name, "Authorization");
    EXPECT_EQ(seen.headers[0].value, "Bearer APP-FAKE");
    EXPECT_EQ(seen.headers[1].name, "Accept");
    EXPECT_EQ(seen.headers[1].value, "application/json");
  }
}

TEST_F(StreamedConcepts, StopWhenTheTotalIsCovered) {
  rig.fake.on("GET", concepts_url(0), {Reply{200, copies(45, 0, 20)}});
  rig.fake.on("GET", concepts_url(20), {Reply{200, copies(45, 20, 20)}});
  rig.fake.on("GET", concepts_url(40), {Reply{200, copies(45, 40, 5)}});
  const auto found = library.concepts_of("PS4GS");
  ASSERT_TRUE(found);
  EXPECT_EQ(found.value().known.size(), 45U);
  EXPECT_EQ(found.value().requests, 3);
  EXPECT_EQ(found.value().total, 45U);
  EXPECT_FALSE(found.value().truncated);
  EXPECT_EQ(rig.fake.seen.size(), 3U);
  EXPECT_TRUE(rig.fake.unmatched.empty());
  EXPECT_EQ(rig.waiter.waits, (std::vector<int>{100, 100}));
}

TEST_F(StreamedConcepts, AskOnceForAnExactPage) {
  rig.fake.on("GET", concepts_url(0), {Reply{200, copies(20, 0, 20)}});
  const auto whole = library.concepts_of("PS4GS");
  ASSERT_TRUE(whole);
  EXPECT_EQ(whole.value().requests, 1);
  EXPECT_EQ(rig.fake.seen.size(), 1U);
}

TEST_F(StreamedConcepts, AreTruncatedAtThePageCap) {
  for (std::size_t page = 0; page < 30; ++page)
    rig.fake.on("GET", concepts_url(page * 20),
                {Reply{200, copies(100000, page * 20, 20)}});
  const auto cut = library.concepts_of("PS4GS");
  ASSERT_TRUE(cut);
  EXPECT_TRUE(cut.value().truncated);
  EXPECT_EQ(cut.value().requests, 25);
  EXPECT_EQ(cut.value().known.size(), 500U);
  EXPECT_EQ(rig.fake.seen.size(), 25U);
}

TEST_F(StreamedConcepts, LeaveOutRowsWithoutAUsableConcept) {
  rig.fake.on(
      "GET", concepts_url(0),
      {Reply{
          200,
          R"({"totalResults":6,"entitlements":[)"
          R"({"id":"A-CUSA00001_00-X","productId":"A-CUSA00001_00-P"},)"
          R"({"id":"A-CUSA00002_00-X","productId":"A-CUSA00002_00-P","conceptMeta":null},)"
          R"({"id":"A-CUSA00003_00-X","productId":"A-CUSA00003_00-P","conceptMeta":{"name":"n"}},)"
          R"({"id":"A-CUSA07001_00-X","productId":"A-CUSA07001_00-P","conceptMeta":{"conceptId":"12x"}},)"
          R"({"id":"A-CUSA07002_00-X","conceptMeta":{"conceptId":777}},)"
          R"({"conceptMeta":{"conceptId":"9"}}]})"}});
  const auto found = library.concepts_of("PS4GS");
  ASSERT_TRUE(found);
  EXPECT_EQ(found.value().received, 6U);
  EXPECT_EQ(found.value().requests, 1);
  ASSERT_EQ(found.value().known.size(), 1U);
  EXPECT_EQ(found.value().known.front().id, "A-CUSA07002_00-X");
  EXPECT_TRUE(found.value().known.front().product_id.empty());
  EXPECT_EQ(found.value().known.front().concept_id, "777");
}

TEST_F(StreamedConcepts, KeepWholeNumbersUnclampedAndDropFractions) {
  rig.fake.on(
      "GET", concepts_url(0),
      {Reply{
          200,
          R"({"totalResults":4,"entitlements":[)"
          R"({"id":"A-CUSA07001_00-X","conceptMeta":{"conceptId":18446744073709551615}},)"
          R"({"id":"A-CUSA07002_00-X","conceptMeta":{"conceptId":1.5}},)"
          R"({"id":"A-CUSA07003_00-X","conceptMeta":{"conceptId":"0042"}},)"
          R"({"id":"A-CUSA07004_00-X","conceptMeta":{"conceptId":-3}}]})"}});
  const auto found = library.concepts_of("PS4GS");
  ASSERT_TRUE(found);
  std::vector<std::string> concepts;
  for (const auto& known : found.value().known)
    concepts.push_back(known.id + "=" + known.concept_id);
  EXPECT_EQ(concepts,
            (std::vector<std::string>{"A-CUSA07001_00-X=18446744073709551615",
                                      "A-CUSA07003_00-X=0042"}));
}

TEST_F(StreamedConcepts, ReportARefusalAfterOneRequest) {
  rig.fake.on("GET", concepts_url(0), {Reply{401, R"({"error":"x"})"}});
  const auto denied = library.concepts_of("PS4GS");
  ASSERT_FALSE(denied);
  EXPECT_TRUE(denied.error().auth_refused());
  EXPECT_EQ(rig.fake.seen.size(), 1U);
}

TEST_F(StreamedConcepts, ReportAServerErrorAfterOneRequest) {
  rig.fake.on("GET", concepts_url(0), {Reply{500, ""}});
  const auto failed = library.concepts_of("PS4GS");
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().http_status(), 500);
  EXPECT_EQ(rig.fake.seen.size(), 1U);
  EXPECT_TRUE(rig.waiter.waits.empty());
}

TEST_F(StreamedConcepts, ReportAnUnreadableReplyAfterOneRequest) {
  for (const std::string_view body :
       {"not json", "[]", R"({"entitlements":[]})", R"({"totalResults":1})",
        R"({"totalResults":"1","entitlements":[]})"}) {
    RestRig odd;
    odd.fake.on("GET", concepts_url(0), {Reply{200, std::string{body}}});
    LibraryEntitlements reading{odd.http, app_bearer()};
    const auto unread = reading.concepts_of("PS4GS");
    ASSERT_FALSE(unread) << body;
    EXPECT_EQ(unread.error().cause(), libchiaking::psn::rest::Cause::response)
        << body;
    EXPECT_EQ(odd.fake.seen.size(), 1U) << body;
  }
}

TEST_F(StreamedConcepts, AreCancelledWithoutARequestOnceStopped) {
  rig.stop();
  const auto never = library.concepts_of("PS4GS");
  ASSERT_FALSE(never);
  EXPECT_TRUE(never.error().is_cancelled());
  EXPECT_TRUE(rig.fake.seen.empty());
}

}  // namespace
}  // namespace psn_test
