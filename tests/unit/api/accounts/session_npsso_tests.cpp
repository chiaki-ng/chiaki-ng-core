#include "tests/unit/api/accounts/rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace accounts_test {
namespace {

using libchiaking::psn::Session;
using namespace std::string_view_literals;

template <class T>
concept ExposesInternals = requires(T& session) { session.tokens(); } ||
                           requires(T& session) { session.app_token(); } ||
                           requires(T& session) { session.titles(); } ||
                           requires(T& session) { session.subscription(); } ||
                           requires(T& session) { session.graphql(); } ||
                           requires(T& session) { session.snapshot(); };

static_assert(!ExposesInternals<Session>);
static_assert(Session::npsso_limit == 4096);
static_assert(Session::app_token_limit == 8192);

class PsnSessionNpsso : public PsnSessionTest {};

TEST_F(PsnSessionNpsso, RefusesAnEmptyOrDelimitedCredential) {
  for (const auto npsso : {""sv, "NP\0SO"sv, "NP;SO"sv, "NP,SO"sv, "NP SO"sv,
                           "NP\rSO"sv, "NP\"SO"sv, "NP\\SO"sv}) {
    const auto refused = open(npsso);
    ASSERT_FALSE(refused) << npsso;
    EXPECT_EQ(refused.error(), Session::Error::invalid_npsso) << npsso;
    EXPECT_FALSE(Session::valid_npsso(npsso)) << npsso;
  }
}

TEST_F(PsnSessionNpsso, AcceptsUpToItsLimitAndNoMore) {
  const std::string oversized(Session::npsso_limit + 1, 'a');
  EXPECT_FALSE(open(oversized));
  const auto opened =
      open(std::string_view{oversized}.substr(0, Session::npsso_limit));
  ASSERT_TRUE(opened);
  ASSERT_TRUE(opened.value());
  EXPECT_FALSE(open(std::string(4097, 'a')));
  EXPECT_TRUE(open(std::string(4096, 'a')));
}

TEST_F(PsnSessionNpsso, MatchesOnlyTheCredentialItWasOpenedWith) {
  const std::string npsso(Session::npsso_limit, 'a');
  const auto opened = open(npsso);
  ASSERT_TRUE(opened);
  ASSERT_TRUE(opened.value());
  EXPECT_TRUE(opened.value()->matches(npsso));
  EXPECT_FALSE(opened.value()->matches("OTHER"));
}

TEST_F(PsnSessionNpsso, SnapshotsAFreshSessionWithoutTheCredential) {
  const std::string npsso(Session::npsso_limit, 'a');
  const auto opened = open(npsso);
  ASSERT_TRUE(opened);
  ASSERT_TRUE(opened.value());
  const auto snapshot = opened.value()->snapshot_json().value();
  EXPECT_TRUE(snapshot.starts_with(R"({"createdAtMs":)")) << snapshot;
  EXPECT_NE(
      snapshot.find(
          R"("totals":{"exchanges":0,"uses":0,"refusals":0},"tokens":[],"subscription":null,"graphql":[]})"),
      std::string::npos)
      << snapshot;
  EXPECT_EQ(snapshot.find("aaaa"), std::string::npos) << snapshot;
  opened.value()->clear();
}

}  // namespace
}  // namespace accounts_test
