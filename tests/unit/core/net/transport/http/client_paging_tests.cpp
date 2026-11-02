#include "tests/unit/core/net/transport/http/client_rig.hpp"

#include "src/core/utils/json/ps_reply.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace http_client_paging_shapes {

struct Listed {
  libchiaking::json::ps::Tolerant<std::vector<glz::raw_json>> items;
};

}  // namespace http_client_paging_shapes

namespace http_test {
namespace {

using http_client_paging_shapes::Listed;
using libchiaking::net::http::Endpoint;
using libchiaking::net::http::Failure;
using libchiaking::net::http::Paging;
using libchiaking::net::http::Request;
using libchiaking::net::http::Response;
using std::chrono::milliseconds;
using test_support::psn::Reply;

enum class PageProblem : std::uint8_t {
  refused,
  malformed,
};

class HttpClientPaging : public ClientRig {
 protected:
  static constexpr std::string_view page_base =
      "https://m.np.playstation.com/api/list?size=2&offset=";
  static constexpr Endpoint listing{.label = "test.pages", .log_query = true};

  static auto page_url(int offset) -> std::string {
    return std::string{page_base} + std::to_string(offset);
  }

  static auto items(int count) -> std::string {
    std::string body = R"({"items":[)";
    for (int index = 0; index < count; ++index)
      body += std::string{index != 0 ? "," : ""} + "{}";
    return body + "]}";
  }

  auto run(int limit = 5) {
    return client.pages(
        Paging{.page_size = 2, .page_limit = limit, .delay = milliseconds{100}},
        [](int offset) {
          Request request{listing, page_url(offset)};
          request.authorize(TestBearer{"BEARER-FAKE"});
          return request;
        },
        [this](const Response& response)
            -> libchiaking::Expected<int, PageProblem> {
          statuses.push_back(response.status());
          if (response.status() != 200)
            return libchiaking::unexpected(PageProblem::refused);
          const auto page = response.document<Listed>();
          if (!page) return libchiaking::unexpected(PageProblem::malformed);
          const int count = page.value().items
                                ? static_cast<int>(page.value().items->size())
                                : 0;
          consumed += count;
          return count;
        });
  }

  int consumed{};
  std::vector<std::uint16_t> statuses;
};

TEST_F(HttpClientPaging, StopsOnAShortPage) {
  fake.on("GET", page_url(0), {Reply{200, items(2)}});
  fake.on("GET", page_url(2), {Reply{200, items(1)}});
  const auto result = run();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().requests, 2);
  EXPECT_EQ(result.value().last_status, 200);
  EXPECT_FALSE(result.value().truncated);
  EXPECT_EQ(consumed, 3);
  EXPECT_EQ(waiter.waits, std::vector<int>{100});
  ASSERT_EQ(fake.seen.size(), 2U);
  EXPECT_EQ(fake.seen[1].header("Authorization"), "Bearer BEARER-FAKE");
  EXPECT_TRUE(fake.unmatched.empty());
  EXPECT_TRUE(log.contains("[HTTP] test.pages page 1 items 2"));
  EXPECT_TRUE(
      log.contains("[HTTP] test.pages paging ok after 2 pages, last HTTP 200"));
}

TEST_F(HttpClientPaging, StopsOnAnEmptyPage) {
  fake.on("GET", page_url(0), {Reply{200, items(2)}});
  fake.on("GET", page_url(2), {Reply{200, R"({"other":1})"}});
  const auto result = run();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().requests, 2);
}

TEST_F(HttpClientPaging, StopsWithATransportFailure) {
  fake.on("GET", page_url(0), {Reply{200, items(2)}});
  fake.on("GET", page_url(2), {Reply::transport()});
  const auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().requests, 2);
  EXPECT_EQ(result.error().last_status, 0);
  ASSERT_TRUE(std::holds_alternative<Failure>(result.error().cause));
  EXPECT_EQ(std::get<Failure>(result.error().cause).kind(),
            Failure::Kind::transport);
  EXPECT_EQ(statuses.size(), 1U);
  EXPECT_TRUE(log.contains(
      "[HTTP] test.pages paging unavailable after 2 pages, last HTTP 0"));
}

TEST_F(HttpClientPaging, TruncatesAtThePageCap) {
  for (int offset = 0; offset < 6; offset += 2)
    fake.on("GET", page_url(offset), {Reply{200, items(2)}});
  const auto result = run(3);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.value().truncated);
  EXPECT_EQ(result.value().requests, 3);
  EXPECT_EQ(result.value().last_status, 200);
  EXPECT_EQ(consumed, 6);
  EXPECT_EQ(waiter.waits.size(), 2U);
  EXPECT_TRUE(log.contains(
      "[HTTP] test.pages paging truncated after 3 pages, last HTTP 200"));
}

TEST_F(HttpClientPaging, StopsWhenCancelledDuringTheWait) {
  fake.on("GET", page_url(0), {Reply{200, items(2)}});
  fake.on("GET", page_url(2), {Reply{200, items(2)}});
  waiter.stop_on_wait = 1;
  waiter.stop_state = state.get();
  const auto result = run();
  ASSERT_FALSE(result);
  ASSERT_TRUE(std::holds_alternative<Failure>(result.error().cause));
  EXPECT_TRUE(std::get<Failure>(result.error().cause).is_cancelled());
  EXPECT_EQ(result.error().requests, 1);
  EXPECT_EQ(fake.seen.size(), 1U);
}

TEST_F(HttpClientPaging, SendsNothingForAZeroPageLimit) {
  const auto result = run(0);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.value().truncated);
  EXPECT_EQ(result.value().requests, 0);
  EXPECT_TRUE(fake.seen.empty());
}

struct RejectedPage {
  int status;
  std::string body;
  std::string_view logged;
  PageProblem problem;
};

class HttpClientPagingRejected
    : public HttpClientPaging,
      public ::testing::WithParamInterface<RejectedPage> {};

TEST_P(HttpClientPagingRejected, EndsPagingWithTheConsumerError) {
  const auto& rejected = GetParam();
  fake.on("GET", page_url(0), {Reply{200, items(2)}});
  fake.on("GET", page_url(2), {Reply{rejected.status, rejected.body}});
  const auto result = run();
  ASSERT_FALSE(result);
  ASSERT_TRUE(std::holds_alternative<PageProblem>(result.error().cause));
  EXPECT_EQ(std::get<PageProblem>(result.error().cause), rejected.problem);
  EXPECT_EQ(result.error().requests, 2);
  EXPECT_EQ(result.error().last_status, rejected.status);
  EXPECT_EQ(consumed, 2);
  EXPECT_EQ(statuses.size(), 2U);
  EXPECT_TRUE(log.contains(
      "[HTTP] test.pages paging " + std::string{rejected.logged} +
      " after 2 pages, last HTTP " + std::to_string(rejected.status)));
}

INSTANTIATE_TEST_SUITE_P(
    Replies, HttpClientPagingRejected,
    ::testing::Values(
        RejectedPage{500, R"({"error":{"code":1}})", "unavailable",
                     PageProblem::refused},
        RejectedPage{401, "", "auth_refused", PageProblem::refused},
        RejectedPage{404, "{}", "not_found", PageProblem::refused},
        RejectedPage{400, "{}", "rejected", PageProblem::refused},
        RejectedPage{200, "not json", "malformed", PageProblem::malformed}));

}  // namespace
}  // namespace http_test
