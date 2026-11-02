#pragma once

#include "src/core/net/protocol/remote_play/stream/audio_layout.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/play/adapters/links/session_diagnostics.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/live_providers.hpp"
#include "tests/unit/core/play/real_session_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <utility>

namespace play_test {

using test_support::LiveProviders;

class LinkDriverRig {
 public:
  using RealStream = api_test::RealStream;
  using CloudService = libchiaking::cloud::CloudService;
  using EcdhCurve = libchiaking::platform::EcdhCurve;
  using RegistrationTarget =
      libchiaking::net::protocol::remote_play::RegistrationTarget;
  using StreamError = libchiaking::net::protocol::remote_play::StreamError;
  using StreamState = libchiaking::net::protocol::remote_play::StreamState;

  static auto since(std::uint64_t began) -> std::chrono::microseconds {
    return std::chrono::microseconds{
        static_cast<std::int64_t>(LiveProviders::steady_us() - began)};
  }

  template <class Value>
  static auto refused(const libchiaking::Expected<Value, StreamError>& result,
                      StreamError error) -> testing::AssertionResult {
    if (result) return testing::AssertionFailure() << "it was accepted";
    if (result.error() != error)
      return testing::AssertionFailure()
             << "refused with " << testing::PrintToString(result.error());
    return testing::AssertionSuccess();
  }

  static auto pinned_version(std::uint32_t base, bool cloud_stream)
      -> std::uint32_t {
    const auto pinned =
        libchiaking::net::protocol::takion::takion_build_version_override;
    const bool cloud_only = pinned == 21 || pinned == 23 || pinned == 25;
    return pinned != 0 && (cloud_stream || !cloud_only) ? pinned : base;
  }

  static auto curve_for(std::uint32_t version) -> EcdhCurve {
    return version == 10 || version >= 13 ? EcdhCurve::secp521r1
                                          : EcdhCurve::secp256k1;
  }

  static auto local_for(RegistrationTarget target,
                        std::uint32_t override_version = 0)
      -> libchiaking::play::HomeLinkConfiguration {
    auto configuration = RealStream::local();
    configuration.console.target = target;
    configuration.tuning.takion_version_override = override_version;
    return configuration;
  }

  static auto remote_for(RegistrationTarget target,
                         std::uint32_t override_version = 0)
      -> libchiaking::play::InternetLinkConfiguration {
    auto configuration = RealStream::remote();
    configuration.console.target = target;
    configuration.tuning.takion_version_override = override_version;
    return configuration;
  }

  static auto cloud_for(CloudService service,
                        std::uint32_t override_version = 0)
      -> libchiaking::play::CloudLinkConfiguration {
    auto configuration = RealStream::cloud(service);
    configuration.tuning.takion_version_override = override_version;
    return configuration;
  }

  static auto cloud_venue(CloudService service) -> libchiaking::play::Venue {
    return service == CloudService::legacy_cloud_stream
               ? libchiaking::play::Venue::cloud(
                     libchiaking::catalog::ServiceGeneration::old_service)
               : libchiaking::play::Venue::cloud(
                     libchiaking::catalog::ServiceGeneration::ps5_cloud);
  }
};

}  // namespace play_test
