#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/legacy_cloud_stream_endpoints.hpp"
#include "src/core/users/ports/play_calls.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/setup.hpp"
#include "tests/support/users/signed_in.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace users_test {

class PreflightProvider final : public libchiaking::platform::HttpProvider {
 public:
  auto perform(const libchiaking::platform::HttpRequest& request,
               const libchiaking::platform::StopToken&)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    ++calls;
    seen = request.clone();
    if (during) during();
    if (error) return libchiaking::unexpected(*error);
    libchiaking::platform::HttpResponse response;
    response.status_code = status;
    return response;
  }

  std::optional<libchiaking::platform::HttpError> error;
  std::uint16_t status{204};
  std::atomic<unsigned> calls{};
  libchiaking::platform::HttpRequest seen;
  std::function<void()> during;
};

class CodeProvider final : public libchiaking::platform::HttpProvider {
 public:
  auto perform(const libchiaking::platform::HttpRequest& request,
               const libchiaking::platform::StopToken&)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    ++calls;
    urls.emplace_back(request.url.view());
    EXPECT_EQ(request.method, libchiaking::platform::HttpMethod::get);
    EXPECT_EQ(request.redirect_policy,
              libchiaking::platform::HttpRedirectPolicy::deny);
    EXPECT_EQ(request.headers.size(), 2U);
    if (request.headers.size() == 2) {
      EXPECT_EQ(request.headers[0].name, "User-Agent");
      EXPECT_EQ(request.headers[1].value.view(), "npsso=synthetic-token");
    }
    if (fail_call == calls)
      return libchiaking::unexpected(
          libchiaking::platform::HttpError::backend_failure);
    if (cancel_call == calls)
      return libchiaking::unexpected(
          libchiaking::platform::HttpError::cancelled);
    libchiaking::platform::HttpResponse response;
    response.status_code =
        reject_call == calls ? reject_status : std::uint16_t{302};
    response.headers.push_back(
        {"Location",
         libchiaking::SecretString{
             (request.url.view().find("grc-response") != std::string_view::npos
                  ? std::string{libchiaking::psn::
                                    legacy_cloud_stream_redirect_uri}
                  : std::string{"gaikai://local"}) +
             "?code=synthetic-" + std::to_string(calls)}});
    return response;
  }

  std::vector<std::string> urls;
  unsigned calls{};
  unsigned fail_call{};
  unsigned cancel_call{};
  unsigned reject_call{};
  std::uint16_t reject_status{200};
};

class CloudPlayer {
 public:
  static constexpr std::string_view duid =
      "000000070041008000112233445566778899aabbccddeeff";

  explicit CloudPlayer(std::string_view npsso = "SYNTHETIC_TOKEN",
                       std::string_view device = duid) {
    libchiaking::users::SavedSignIn saved{libchiaking::AccountId{7}, "player",
                                          libchiaking::psn::auth::Npsso{npsso}};
    saved.device = libchiaking::users::DeviceId::parse(device);
    test_support::Setup::require(saved.device.has_value(),
                                 "the device id parses");
    user = signed_in.as(std::move(saved));
  }

  test_support::users::Offline offline;
  test_support::FakeClock clock;
  test_support::FakeClock wall;
  test_support::users::SignedIn signed_in{offline, clock, wall};
  std::shared_ptr<libchiaking::users::PsUser> user;
};

class CloudAuthorisation {
 public:
  using Refusal = libchiaking::users::AuthorisationRefusal;
  using Checked = libchiaking::Expected<void, Refusal>;
  using Kind = libchiaking::psn::auth::AuthErrorKind;
  using Service = libchiaking::psn::StreamingService;
  using Platform = libchiaking::psn::StreamingPlatform;

  struct Granted {
    libchiaking::Expected<libchiaking::psn::auth::GkCodes, Refusal> codes;
    std::vector<libchiaking::psn::auth::GkStep> steps;
  };

  static auto cannot_be_admitted(std::string_view npsso)
      -> testing::AssertionResult {
    test_support::users::SignedIn signed_in;
    libchiaking::users::SavedSignIn saved{libchiaking::AccountId{7}, "player",
                                          libchiaking::psn::auth::Npsso{npsso}};
    saved.device = libchiaking::users::DeviceId::parse(CloudPlayer::duid);
    if (!signed_in.credentials.save(saved))
      return testing::AssertionFailure() << "the sign-in was not saved";
    const auto returned = signed_in.users.welcome_back();
    if (!returned) return testing::AssertionFailure() << "welcome back failed";
    if (returned.value().rejected != 1)
      return testing::AssertionFailure()
             << "rejected " << returned.value().rejected;
    if (!returned.value().users.empty())
      return testing::AssertionFailure() << "the user was admitted";
    if (signed_in.offline.requests != 0)
      return testing::AssertionFailure()
             << signed_in.offline.requests << " requests were sent";
    return testing::AssertionSuccess();
  }

  static auto preflight(PreflightProvider& provider,
                        libchiaking::users::PsUser& user, Service service,
                        const libchiaking::platform::StopToken& stop = {})
      -> Checked {
    libchiaking::net::http::Client client{provider, Runtime::steady(), stop};
    return libchiaking::users::PlayCalls{user}.cloud_device_check(client,
                                                                  service);
  }

  static auto codes(CodeProvider& provider, libchiaking::users::PsUser& user,
                    const libchiaking::psn::auth::GkClientIds& ids,
                    Service service = Service::cloud_stream,
                    Platform target = Platform::ps5,
                    const libchiaking::platform::StopToken& stop = {})
      -> Granted {
    std::vector<libchiaking::psn::auth::GkStep> steps;
    libchiaking::net::http::Client client{provider, Runtime::steady(), stop};
    auto granted = libchiaking::users::PlayCalls{user}.cloud_streaming_codes(
        client, service, target, ids,
        [&steps](libchiaking::psn::auth::GkStep step) {
          steps.push_back(step);
        });
    return {std::move(granted), std::move(steps)};
  }

  static auto refused_as(const Checked& result, Kind kind,
                         std::uint16_t status = 0) -> testing::AssertionResult {
    if (result) return testing::AssertionFailure() << "the check passed";
    if (result.error().is_signed_out())
      return testing::AssertionFailure() << "refused as signed out";
    if (!result.error().authorisation())
      return testing::AssertionFailure() << "no authorisation error";
    if (result.error().authorisation()->kind() != kind)
      return testing::AssertionFailure()
             << "kind "
             << testing::PrintToString(result.error().authorisation()->kind());
    if (result.error().http_status() != status)
      return testing::AssertionFailure()
             << "status " << result.error().http_status();
    if (result.error().is_cancelled() != (kind == Kind::cancelled))
      return testing::AssertionFailure()
             << "cancelled " << result.error().is_cancelled();
    return testing::AssertionSuccess();
  }

  static auto code_refused_as(const Granted& granted, Kind kind,
                              std::uint16_t status = 0)
      -> testing::AssertionResult {
    const auto& result = granted.codes;
    if (result) return testing::AssertionFailure() << "codes were granted";
    if (!result.error().authorisation())
      return testing::AssertionFailure() << "no authorisation error";
    if (result.error().authorisation()->kind() != kind)
      return testing::AssertionFailure()
             << "kind "
             << testing::PrintToString(result.error().authorisation()->kind());
    if (result.error().http_status() != status)
      return testing::AssertionFailure()
             << "status " << result.error().http_status();
    return testing::AssertionSuccess();
  }

  template <class Answer>
  static auto signed_out(const Answer& answer) -> testing::AssertionResult {
    if (answer) return testing::AssertionFailure() << "answered";
    if (!answer.error().is_signed_out())
      return testing::AssertionFailure() << "not signed out";
    if (answer.error().authorisation())
      return testing::AssertionFailure() << "carries an authorisation error";
    if (answer.error().is_cancelled())
      return testing::AssertionFailure() << "cancelled";
    if (answer.error().expires_sign_in())
      return testing::AssertionFailure() << "expires the sign-in";
    if (answer.error().http_status() != 0)
      return testing::AssertionFailure()
             << "status " << answer.error().http_status();
    return testing::AssertionSuccess();
  }
};

}  // namespace users_test
