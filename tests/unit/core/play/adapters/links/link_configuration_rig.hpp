#pragma once

#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "src/core/play/adapters/links/home_link_configuration.hpp"
#include "src/core/play/adapters/links/internet/internet_link_configuration.hpp"
#include "tests/support/catalog/idle_catalog.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/support/users/signed_in.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <utility>

namespace play_test::link_configurations {

using test_support::catalog::IdleCatalog;
using test_support::play::SignedInPlayer;

class Configurations {
 public:
  using StreamError = libchiaking::net::protocol::remote_play::StreamError;

  static auto local() -> libchiaking::play::HomeLinkConfiguration {
    libchiaking::play::HomeLinkConfiguration configuration;
    configuration.host = "127.0.0.1";
    configuration.console.target =
        libchiaking::net::protocol::remote_play::RegistrationTarget::ps5_1;
    configuration.ctrl_endpoint.port = 9295;
    configuration.active_stream_port = 9296;
    configuration.codec =
        libchiaking::net::protocol::remote_play::VideoCodec::h265;
    return configuration;
  }

  static auto remote() -> libchiaking::play::InternetLinkConfiguration {
    libchiaking::play::InternetLinkConfiguration configuration;
    configuration.user =
        SignedInPlayer::fresh_for_remote_play("SYNTHETIC_BEARER_TOKEN");
    configuration.console.target =
        libchiaking::net::protocol::remote_play::RegistrationTarget::ps5_1;
    configuration.codec =
        libchiaking::net::protocol::remote_play::VideoCodec::h265;
    configuration.enable_ipv6 = true;
    return configuration;
  }

  static auto cloud() -> libchiaking::play::CloudLinkConfiguration {
    libchiaking::play::CloudLinkConfiguration configuration;
    configuration.user = SignedInPlayer::fresh();
    configuration.catalog = &IdleCatalog::shared();
    configuration.product = IdleCatalog::title();
    configuration.time_zone = "UTC+00:00";
    configuration.resolution = 1080;
    configuration.bitrate_kbps = 10000;
    return configuration;
  }

  static auto home_user() -> std::shared_ptr<libchiaking::users::PsUser> {
    auto owner = std::make_shared<test_support::users::SignedIn>();
    const auto added = owner->users.add_home_user(libchiaking::AccountId{40},
                                                  "Living Room Player");
    EXPECT_TRUE(added);
    if (!added) return nullptr;
    return {owner, added.value().get()};
  }

  static auto allocation() -> libchiaking::play::CloudAllocation {
    libchiaking::play::CloudAllocation value;
    value.entitlement_id = "synthetic-entitlement";
    value.server_ip = "127.0.0.1";
    value.server_port = 9296;
    value.handshake_key.assign("AAECAwQFBgcICQoLDA0ODw==");
    value.session_id.assign("synthetic-session");
    value.launch_spec.assign("synthetic-launch");
    return value;
  }

  template <class Configuration>
  static auto refusal(Configuration configuration)
      -> std::optional<StreamError> {
    const auto checked = std::move(configuration).validated();
    if (checked) return std::nullopt;
    return checked.error();
  }

  template <class Configuration>
  static auto accepts(Configuration configuration) -> testing::AssertionResult {
    const auto checked = std::move(configuration).validated();
    if (checked) return testing::AssertionSuccess();
    return testing::AssertionFailure()
           << "refused with " << testing::PrintToString(checked.error());
  }
};

}  // namespace play_test::link_configurations
