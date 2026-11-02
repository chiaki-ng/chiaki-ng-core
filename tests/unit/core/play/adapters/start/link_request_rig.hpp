#pragma once

#include "tests/support/api/fakes.hpp"
#include "tests/support/play/signed_in_player.hpp"

#include "libchiaking/platform/runtime/utc_offset.hpp"
#include "src/core/play/adapters/links/internet/internet_link.hpp"
#include "src/core/play/adapters/links/link_driver.hpp"
#include "src/core/play/adapters/start/link_request.hpp"
#include "tests/support/catalog/idle_catalog.hpp"
#include "tests/support/consoles/paired_consoles.hpp"
#include "tests/support/utc_offset.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

namespace play_test {

using test_support::catalog::IdleCatalog;
using test_support::play::SignedInPlayer;

class LinkRequestRig : public ::testing::Test {
 protected:
  using LocalVideoCodec = libchiaking::net::protocol::remote_play::VideoCodec;
  using ConfigurationError = libchiaking::play::ConfigurationError;
  using VideoRequest = libchiaking::play::VideoRequest;

  static constexpr std::string_view ps5_console = "aabbccddee01";
  static constexpr std::string_view ps4_console = "aabbccddee02";

  static auto home_user() -> std::shared_ptr<libchiaking::users::PsUser> {
    auto owner = std::make_shared<test_support::users::SignedIn>();
    const auto added = owner->users.add_home_user(libchiaking::AccountId{40},
                                                  "Living Room Player");
    EXPECT_TRUE(added);
    return {owner, added.value().get()};
  }

  static auto hd_video() -> VideoRequest {
    const auto request =
        VideoRequest::create(1080, 60, 30000, LocalVideoCodec::h265_hdr);
    EXPECT_TRUE(request);
    return request.value();
  }

  static auto device(std::byte fill) -> libchiaking::consoles::DeviceUid {
    std::array<std::byte, libchiaking::consoles::DeviceUid::size> bytes{};
    bytes.fill(fill);
    return libchiaking::consoles::DeviceUid::from_bytes(bytes).value();
  }

  static auto internet_target(const libchiaking::consoles::Console& console,
                              const libchiaking::users::PsUser& player)
      -> libchiaking::consoles::PairedPlay::InternetTarget {
    auto target =
        libchiaking::consoles::PairedPlay{console, player}.over_the_internet();
    EXPECT_TRUE(target);
    return target.value();
  }

  static auto internet(
      const libchiaking::consoles::Console& console,
      std::shared_ptr<libchiaking::users::PsUser> player, VideoRequest video,
      libchiaking::play::ConsolePreferences preferences = {},
      libchiaking::play::PortGuessing guessing = {},
      std::chrono::seconds timeout =
          libchiaking::play::InternetLinkConfiguration::default_start_timeout) {
    const auto target = internet_target(console, *player);
    return libchiaking::play::InternetLinks::configured(
        target, std::move(player), video, std::move(preferences), guessing,
        timeout);
  }

  static auto cloud_launch(
      std::string_view title = "EP0109-PPSA07043_00-FAKE0043TITLE000")
      -> libchiaking::play::CloudLinkRequest {
    libchiaking::play::CloudLinkRequest launch{SignedInPlayer::fresh(),
                                               IdleCatalog::shared(),
                                               IdleCatalog::title(title)};
    launch.video =
        VideoRequest::create(1080, 60, 15000, LocalVideoCodec::h265).value();
    return launch;
  }

  test_support::consoles::PairedConsoles consoles;
};

}  // namespace play_test
