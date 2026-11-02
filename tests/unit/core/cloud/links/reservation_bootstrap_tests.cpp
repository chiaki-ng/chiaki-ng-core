#include "libchiaking/detail/diagnostics/log_queue.hpp"
#include "src/core/utils/diagnostics/trace.hpp"
#include "tests/support/clock.hpp"
#include "tests/unit/core/cloud/session_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace cloud_test {
namespace {

using Reason = gk::Error::Reason;
using Stage = gk::Error::Stage;

class BootstrapProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    platform::HttpResponse response;
    response.status_code = 200;
    std::string body;
    const auto url = request.url.view();
    switch (calls) {
      case 1:
        EXPECT_TRUE(url.ends_with("/client_ids?virtType=cronos")) << url;
        body =
            R"({"gkClientId":"a","ps3GkClientId":"b","streamServerClientId":"c"})";
        if (stop_after_ids) stop_after_ids->stopped = true;
        break;
      case 2:
        EXPECT_TRUE(url.ends_with("/config")) << url;
        response.status_code = config_status;
        body = R"({"configKey":"synthetic-key"})";
        if (stop_after_config) stop_after_config->stopped = true;
        break;
      case 3:
        EXPECT_NE(url.find("/sessions/start"), std::string::npos);
        response.status_code = start_status;
        body = start_status == 200 ? R"({"sessionId":"synthetic-session"})"
                                   : R"({"name":"noGameForEntitlementId"})";
        if (rotate_on_start)
          response.headers.push_back(
              {"X-Gaikai-Session",
               libchiaking::SecretString{"rotated-synthetic-key"}});
        break;
      default:
        ADD_FAILURE() << "unexpected request " << calls << " to " << url;
    }
    for (const char character : body)
      response.body.push_back(static_cast<std::byte>(character));
    return response;
  }

  unsigned calls{};
  std::uint16_t config_status{200};
  std::uint16_t start_status{200};
  bool rotate_on_start{true};
  std::shared_ptr<test_support::StopState> stop_after_ids;
  std::shared_ptr<test_support::StopState> stop_after_config;
};

class LogSummary {
 public:
  unsigned leaks{};
  unsigned starts{};
  unsigned successes{};
};

class ReservationBootstrap : public ::testing::Test {
 protected:
  static auto spec() -> gk::LaunchSpec {
    return gk::LaunchSpec::prebuilt(CloudService::cloud_stream, "{}");
  }

  static auto summarize(libchiaking::diagnostics::LogQueue& logs)
      -> LogSummary {
    LogSummary summary;
    while (const auto log = logs.read()) {
      if (std::strstr(log->c_str(), "synthetic")) ++summary.leaks;
      if (std::strstr(log->c_str(), "outcome=begin")) ++summary.starts;
      if (std::strstr(log->c_str(), "outcome=succeeded")) ++summary.successes;
    }
    return summary;
  }

  const platform::StopToken running;
  BootstrapProvider provider;
};

TEST_F(ReservationBootstrap, OpensASessionAndTracesEachStepWithoutSecrets) {
  test_support::FakeClock clock{1};
  libchiaking::diagnostics::LogQueue logs{clock};
  libchiaking::diagnostics::Trace trace{
      &logs, clock, libchiaking::diagnostics::Component::cloud};
  Chain traced{provider, running, &trace};
  EXPECT_TRUE(traced.open(spec()));
  EXPECT_EQ(provider.calls, 3U);
  const auto summary = summarize(logs);
  EXPECT_EQ(summary.leaks, 0U);
  EXPECT_EQ(summary.starts, 3U);
  EXPECT_EQ(summary.successes, 3U);
  EXPECT_EQ(traced.session().client_ids().gk_client_id, "a");
  EXPECT_EQ(traced.session().config_key().view(), "rotated-synthetic-key");
  EXPECT_EQ(traced.session().id().view(), "synthetic-session");
}

TEST_F(ReservationBootstrap, KeepsTheConfigKeyWhenTheStartDoesNotRotateIt) {
  provider.rotate_on_start = false;
  Chain unrotated{provider, running};
  EXPECT_TRUE(unrotated.open(spec()));
  EXPECT_EQ(provider.calls, 3U);
  EXPECT_EQ(unrotated.session().config_key().view(), "synthetic-key");
}

TEST_F(ReservationBootstrap, StopsAtARefusedConfig) {
  provider.config_status = 500;
  Chain chain{provider, running};
  EXPECT_TRUE(
      GkRig::failed(chain.open(spec()), Stage::config, Reason::rejected, 3));
  EXPECT_EQ(provider.calls, 2U);
}

TEST_F(ReservationBootstrap, AsksForAnEntitlementRetryWithoutAGame) {
  provider.start_status = 401;
  Chain chain{provider, running};
  EXPECT_TRUE(GkRig::failed(chain.open(spec()), Stage::start,
                            Reason::entitlement_retry, 1));
  EXPECT_EQ(provider.calls, 3U);
}

TEST_F(ReservationBootstrap, StopsAfterTheClientIdsWhenStoppedThere) {
  provider.stop_after_ids = std::make_shared<test_support::StopState>();
  Chain chain{provider, platform::StopToken{provider.stop_after_ids}};
  const auto result = chain.open(spec());
  ASSERT_FALSE(result);
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_EQ(result.error().stage(), Stage::client_ids);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(result.error().detail(), 1U);
}

TEST_F(ReservationBootstrap, StopsAfterTheConfigWhenStoppedThere) {
  provider.stop_after_config = std::make_shared<test_support::StopState>();
  Chain chain{provider, platform::StopToken{provider.stop_after_config}};
  const auto result = chain.open(spec());
  ASSERT_FALSE(result);
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_EQ(result.error().stage(), Stage::config);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_EQ(result.error().detail(), 1U);
}

}  // namespace
}  // namespace cloud_test
