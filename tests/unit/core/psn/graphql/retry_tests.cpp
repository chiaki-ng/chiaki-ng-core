#include "tests/unit/core/psn/graphql/rig.hpp"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

namespace psn_test {
namespace {

using Kind = GraphqlRig::Kind;
using Reply = ClientRig::Reply;

class GraphqlRetry : public ::testing::Test {
 protected:
  void answers(std::vector<Reply> replies) {
    rig.http.on("GET", GraphqlRig::url_of(GraphqlRig::probe, "{}"),
                std::move(replies));
  }

  GraphqlRig rig;
};

TEST_F(GraphqlRetry, FollowsUnavailableTransportAnd429WithWaits) {
  answers({Reply{503, ""}, Reply::transport(), Reply{429, ""},
           Reply{200, R"({"data":{}})"}});
  const auto result = rig.send(GraphqlRig::probe, GraphqlRig::retrying(3, 250));
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().attempts, 4);
  EXPECT_EQ(rig.http.seen.size(), 4U);
  EXPECT_EQ(rig.waiter.waits, (std::vector<int>{250, 250, 250}));
  const auto entries = rig.stats.entries();
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries[0].requests, 4U);
  EXPECT_EQ(entries[0].last_status, "ok");
}

TEST_F(GraphqlRetry, StopsAtTheBound) {
  answers({Reply{503, ""}});
  const auto result = rig.send(GraphqlRig::probe, GraphqlRig::retrying(2, 100));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), Kind::server_error);
  EXPECT_EQ(result.error().attempts(), 3);
  EXPECT_EQ(rig.http.seen.size(), 3U);
  EXPECT_EQ(rig.waiter.waits.size(), 2U);
  const auto entries = rig.stats.entries();
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries[0].failures, 1U);
  EXPECT_EQ(entries[0].last_status, "unavailable");
  EXPECT_EQ(entries[0].last_http_status, 503);
}

TEST_F(GraphqlRetry, IsCappedAndNeverNegative) {
  const auto capped =
      GraphqlRig::run({Reply{503, ""}}, GraphqlRig::retrying(1000));
  ASSERT_FALSE(capped);
  EXPECT_EQ(capped.error().attempts(), libchiaking::net::http::max_retries + 1);
  const auto negative =
      GraphqlRig::run({Reply{503, ""}}, GraphqlRig::retrying(-4, 10));
  ASSERT_FALSE(negative);
  EXPECT_EQ(negative.error().attempts(), 1);
}

TEST_F(GraphqlRetry, NeverFollowsAGraphqlOrClientFailure) {
  const std::vector<Reply> failures{
      Reply{
          200,
          R"({"errors":[{"message":"x","extensions":{"code":"PERSISTED_QUERY_NOT_FOUND"}}]})"},
      Reply{200, R"({"data":{},"errors":[{"message":"x"}]})"},
      Reply{200, "<html>"},
      Reply{401, ""},
      Reply{404, ""},
  };
  for (const auto& failure : failures) {
    SCOPED_TRACE(failure.status);
    SCOPED_TRACE(failure.body);
    GraphqlRig fresh;
    fresh.http.on("GET", GraphqlRig::url_of(GraphqlRig::probe, "{}"),
                  {failure, Reply{200, R"({"data":{}})"}});
    const auto result =
        fresh.send(GraphqlRig::probe, GraphqlRig::retrying(3, 100));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().attempts(), 1);
    EXPECT_EQ(fresh.http.seen.size(), 1U);
    EXPECT_TRUE(fresh.waiter.waits.empty());
  }
}

TEST_F(GraphqlRetry, IsCancelledByAStopDuringAnAttempt) {
  answers({rig.stopping_during(Reply{200, R"({"data":{}})"})});
  const auto stopped = rig.send(GraphqlRig::probe);
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error().kind(), Kind::cancelled);
  EXPECT_EQ(stopped.error().attempts(), 1);
}

TEST_F(GraphqlRetry, IsCancelledByAStopBetweenAttempts) {
  rig.waiter.stop_state = rig.state.get();
  rig.waiter.stop_on_wait = 1;
  answers({Reply{503, ""}, Reply{200, R"({"data":{}})"}});
  const auto result = rig.send(GraphqlRig::probe, GraphqlRig::retrying(3, 500));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), Kind::cancelled);
  EXPECT_EQ(result.error().attempts(), 1);
  EXPECT_EQ(rig.http.seen.size(), 1U);
  EXPECT_EQ(rig.waiter.waits.size(), 1U);
}

}  // namespace
}  // namespace psn_test
