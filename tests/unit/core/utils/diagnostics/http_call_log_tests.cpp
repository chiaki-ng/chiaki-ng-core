#include "libchiaking/platform/runtime/threading.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/unit/core/utils/diagnostics/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace diagnostics_test {
namespace {

namespace platform = libchiaking::platform;
namespace diagnostics = libchiaking::diagnostics;
using diagnostics::HttpCallLog;
using platform::HttpCredential;

class HttpCallLogTest : public ::testing::Test {
 protected:
  HttpCallLog log{test_support::process_threading()};
};

using HttpCallLogRecords = HttpCallLogTest;
using HttpCallLogBounds = HttpCallLogTest;
using HttpCallLogTotals = HttpCallLogTest;

TEST_F(HttpCallLogRecords, SuccessHttpErrorAndTransportError) {
  const auto before = log.revision();
  Calls::record(log, Calls::request_to("https://m.np.playstation.com/api/one"),
                Calls::answered(200, "{}"));
  Calls::record(
      log, Calls::request_to("https://m.np.playstation.com/api/two", "a=1"),
      Calls::answered(404, "gone"));
  Calls::record(log, Calls::request_to("https://cc.prod.gaikai.com/v1/three"),
                libchiaking::unexpected(platform::HttpError::timed_out));
  EXPECT_NE(log.revision(), before);
  const auto snapshot = log.snapshot();
  EXPECT_EQ(snapshot.revision, log.revision());
  ASSERT_EQ(snapshot.calls.size(), 3U);
  const auto& timed_out = *snapshot.calls[0];
  const auto& missing = *snapshot.calls[1];
  const auto& ok = *snapshot.calls[2];
  EXPECT_EQ(ok.sequence(), 1U);
  EXPECT_EQ(missing.sequence(), 2U);
  EXPECT_EQ(timed_out.sequence(), 3U);
  EXPECT_EQ(ok.status(), 200);
  EXPECT_FALSE(ok.failed());
  EXPECT_FALSE(ok.transport_error());
  EXPECT_EQ(ok.base(), "https://m.np.playstation.com");
  EXPECT_EQ(ok.method(), platform::HttpMethod::get);
  EXPECT_EQ(ok.label(), "test.call");
  EXPECT_EQ(ok.duration(), std::chrono::microseconds{1500});
  EXPECT_EQ(ok.response_body().text.text(), "{}");
  EXPECT_EQ(missing.status(), 404);
  EXPECT_TRUE(missing.failed());
  EXPECT_EQ(missing.bytes_sent(), 3U);
  EXPECT_EQ(missing.bytes_received(), 4U);
  EXPECT_EQ(missing.request_body().text.text(), "a=1");
  EXPECT_TRUE(timed_out.failed());
  EXPECT_FALSE(timed_out.status());
  EXPECT_EQ(timed_out.transport_error(), platform::HttpError::timed_out);
  EXPECT_TRUE(timed_out.response_headers().empty());
  EXPECT_EQ(timed_out.response_body().size, 0U);
  EXPECT_EQ(timed_out.base(), "https://cc.prod.gaikai.com");
  EXPECT_EQ(snapshot.totals.all.requests, 3U);
  EXPECT_EQ(snapshot.totals.all.http_errors, 1U);
  EXPECT_EQ(snapshot.totals.all.transport_errors, 1U);
  EXPECT_EQ(snapshot.totals.all.bytes_sent, 3U);
  EXPECT_EQ(snapshot.totals.all.bytes_received, 6U);
}

TEST_F(HttpCallLogRecords, AnExpectedStatusIsAnAnswerNotAnError) {
  static constexpr std::array<std::uint16_t, 1> waiting{400};
  auto request =
      Calls::request_to("https://ca.account.sony.com/api/poll", "{}");
  request.provenance.expected_statuses = waiting;
  Calls::record(log, request, Calls::answered(400, "pending"));
  Calls::record(log, request, Calls::answered(500, "down"));
  Calls::record(log, Calls::request_to("https://ca.account.sony.com/api/poll"),
                Calls::answered(400, "bad"));
  const auto snapshot = log.snapshot();
  ASSERT_EQ(snapshot.calls.size(), 3U);
  EXPECT_FALSE(snapshot.calls[2]->failed());
  EXPECT_EQ(snapshot.calls[2]->status(), 400);
  EXPECT_TRUE(snapshot.calls[1]->failed());
  EXPECT_TRUE(snapshot.calls[0]->failed());
  EXPECT_EQ(snapshot.totals.all.requests, 3U);
  EXPECT_EQ(snapshot.totals.all.http_errors, 2U);
}

TEST_F(HttpCallLogBounds, KeepsTheNewestCalls) {
  for (std::size_t index = 0; index <= HttpCallLog::capacity; ++index)
    Calls::record(log, Calls::request_to("https://m.np.playstation.com/api"),
                  Calls::answered(200));
  const auto snapshot = log.snapshot();
  ASSERT_EQ(snapshot.calls.size(), HttpCallLog::capacity);
  EXPECT_EQ(snapshot.calls.front()->sequence(), HttpCallLog::capacity + 1);
  EXPECT_EQ(snapshot.calls.back()->sequence(), 2U);
}

TEST_F(HttpCallLogBounds, TruncatesLongBodies) {
  const std::string big(HttpCallLog::body_limit + 10, 'x');
  Calls::record(log,
                Calls::request_to("https://m.np.playstation.com/upload", big),
                Calls::answered(200, big));
  const auto snapshot = log.snapshot();
  ASSERT_FALSE(snapshot.calls.empty());
  const auto sent = snapshot.calls.front()->request_body();
  EXPECT_EQ(sent.size, big.size());
  EXPECT_EQ(sent.kept, HttpCallLog::body_limit);
  EXPECT_TRUE(sent.truncated);
  EXPECT_EQ(sent.text.text().size(), HttpCallLog::body_limit);
  const auto received = snapshot.calls.front()->response_body();
  EXPECT_TRUE(received.truncated);
  EXPECT_EQ(received.kept, HttpCallLog::body_limit);
  EXPECT_EQ(snapshot.calls.front()->bytes_received(), big.size());
}

TEST_F(HttpCallLogBounds, BoundsTheBytesItKeeps) {
  const std::string half(HttpCallLog::body_limit, 'y');
  constexpr std::size_t made = 40;
  for (std::size_t index = 0; index < made; ++index)
    Calls::record(log,
                  Calls::request_to("https://image.api.playstation.com/cover"),
                  Calls::answered(200, half));
  const auto snapshot = log.snapshot();
  EXPECT_LT(snapshot.calls.size(), made);
  ASSERT_GE(snapshot.calls.size(), 30U);
  std::size_t kept{};
  for (const auto& call : snapshot.calls) kept += call->response_body().kept;
  EXPECT_LE(kept, HttpCallLog::byte_limit);
  EXPECT_EQ(snapshot.calls.front()->sequence(), made);
  EXPECT_EQ(snapshot.totals.all.requests, made);
}

TEST_F(HttpCallLogTotals, CountPastTheRingByBaseAndCredential) {
  constexpr std::size_t made = 250;
  for (std::size_t index = 0; index < made; ++index) {
    auto request =
        Calls::request_to(index % 2 == 0 ? "https://m.np.playstation.com/api"
                                         : "https://cc.prod.gaikai.com:443/v1",
                          "abc");
    if (index % 5 == 0)
      request.provenance.credentials =
          request.provenance.credentials.with(HttpCredential::account_token)
              .with(HttpCredential::store_session);
    Calls::record(log, request,
                  index % 10 == 0 ? Calls::answered(500, "no")
                                  : Calls::answered(200, "yes"));
  }
  const auto snapshot = log.snapshot();
  EXPECT_EQ(snapshot.calls.size(), HttpCallLog::capacity);
  const auto& totals = snapshot.totals;
  EXPECT_EQ(totals.all.requests, made);
  EXPECT_EQ(totals.all.http_errors, 25U);
  EXPECT_EQ(totals.all.bytes_sent, made * 3);
  EXPECT_EQ(totals.all.bytes_received, 25U * 2 + 225U * 3);
  EXPECT_EQ(totals.anonymous.requests, 200U);
  EXPECT_EQ(totals.credentials.size(), platform::every_http_credential.size());
  for (const auto& tally : totals.credentials) {
    const bool carried = tally.credential == HttpCredential::account_token ||
                         tally.credential == HttpCredential::store_session;
    EXPECT_EQ(tally.tally.requests, carried ? 50U : 0U)
        << testing::PrintToString(tally.credential);
    if (carried) {
      EXPECT_EQ(tally.tally.http_errors, 25U);
    }
  }
  ASSERT_EQ(totals.bases.size(), 2U);
  EXPECT_EQ(totals.bases[0].base, "https://cc.prod.gaikai.com:443");
  EXPECT_EQ(totals.bases[0].tally.requests, 125U);
  EXPECT_EQ(totals.bases[0].tally.http_errors, 0U);
  EXPECT_EQ(totals.bases[1].base, "https://m.np.playstation.com");
  EXPECT_EQ(totals.bases[1].tally.requests, 125U);
  EXPECT_EQ(totals.bases[1].tally.http_errors, 25U);
}

TEST_F(HttpCallLogTotals, SurviveClearingUntilReset) {
  constexpr std::size_t made = 250;
  for (std::size_t index = 0; index < made; ++index)
    Calls::record(log, Calls::request_to("https://m.np.playstation.com/api"),
                  Calls::answered(200));
  const auto before = log.revision();
  log.clear();
  EXPECT_NE(log.revision(), before);
  auto snapshot = log.snapshot();
  EXPECT_TRUE(snapshot.calls.empty());
  EXPECT_EQ(snapshot.totals.all.requests, made);
  Calls::record(log, Calls::request_to("https://m.np.playstation.com/api"),
                Calls::answered(200));
  snapshot = log.snapshot();
  ASSERT_FALSE(snapshot.calls.empty());
  EXPECT_EQ(snapshot.calls.front()->sequence(), made + 1);
  log.reset_totals();
  snapshot = log.snapshot();
  EXPECT_EQ(snapshot.calls.size(), 1U);
  EXPECT_EQ(snapshot.totals.all, diagnostics::HttpTally{});
  EXPECT_TRUE(snapshot.totals.bases.empty());
  EXPECT_EQ(snapshot.totals.credentials.size(),
            platform::every_http_credential.size());
}

TEST_F(HttpCallLogRecords, ConcurrentlyWithReaders) {
  constexpr std::size_t writers = 8;
  constexpr std::size_t each = 100;
  std::atomic<bool> done{false};
  auto reader = platform::Thread::start(
                    test_support::process_threading(), "reader",
                    platform::ThreadRole::unspecified,
                    [&] {
                      while (!done.load()) {
                        const auto snapshot = log.snapshot();
                        EXPECT_LE(snapshot.calls.size(), HttpCallLog::capacity);
                        for (std::size_t index = 1;
                             index < snapshot.calls.size(); ++index)
                          EXPECT_GT(snapshot.calls[index - 1]->sequence(),
                                    snapshot.calls[index]->sequence());
                      }
                    })
                    .value();
  std::vector<platform::Thread> threads;
  for (std::size_t writer = 0; writer < writers; ++writer)
    threads.push_back(
        platform::Thread::start(
            test_support::process_threading(), "writer",
            platform::ThreadRole::unspecified,
            [this, writer] {
              for (std::size_t index = 0; index < each; ++index)
                Calls::record(
                    log,
                    Calls::request_to(writer % 2 == 0
                                          ? "https://m.np.playstation.com/a"
                                          : "https://web.np.playstation.com/b",
                                      "xy"),
                    Calls::answered(200, "abc"));
            })
            .value());
  for (auto& thread : threads) EXPECT_TRUE(thread.join());
  done.store(true);
  EXPECT_TRUE(reader.join());
  const auto snapshot = log.snapshot();
  ASSERT_EQ(snapshot.calls.size(), HttpCallLog::capacity);
  EXPECT_EQ(snapshot.calls.front()->sequence(), writers * each);
  EXPECT_EQ(snapshot.totals.all.requests, writers * each);
  EXPECT_EQ(snapshot.totals.all.bytes_sent, writers * each * 2);
  ASSERT_EQ(snapshot.totals.bases.size(), 2U);
  EXPECT_EQ(snapshot.totals.bases[0].tally.requests, writers * each / 2);
}

TEST(HttpCallLogStorage, WritesNothingToDisk) {
  namespace fs = std::filesystem;
  const auto place =
      fs::temp_directory_path() / "libchiaking_http_call_log_tests";
  fs::remove_all(place);
  fs::create_directories(place);
  const auto previous = fs::current_path();
  fs::current_path(place);
  {
    HttpCallLog log{test_support::process_threading()};
    for (std::size_t index = 0; index < 300; ++index)
      Calls::record(
          log, Calls::request_to("https://m.np.playstation.com/api", "body"),
          Calls::answered(200, "{}"));
    log.clear();
  }
  fs::current_path(previous);
  EXPECT_TRUE(fs::is_empty(place));
  fs::remove_all(place);
}

}  // namespace
}  // namespace diagnostics_test
