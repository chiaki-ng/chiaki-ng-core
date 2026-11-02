#pragma once

#include "src/core/net/protocol/takion/source_tap.hpp"
#include "tests/unit/core/play/adapters/session/session_case.hpp"
#include "tests/unit/core/play/shared_audience.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace play_test {

using test_support::play::SessionRig;

class ShareCase : public SessionCase {
 protected:
  using SourceReceiveKeys =
      libchiaking::net::protocol::takion::SourceReceiveKeys;

  static constexpr std::uint32_t shareable_version = 12;
  static constexpr std::uint32_t unshareable_version = 7;

  static auto shared_description(bool with_picture = true)
      -> libchiaking::media::StreamDescription {
    libchiaking::media::StreamDescription description;
    description.audio.channels = 2;
    description.audio.rate = 48000;
    description.profiles.push_back({1280, 720, api_test::bytes_of({1, 2, 3})});
    if (with_picture)
      description.picture = libchiaking::media::GrantedPicture{
          libchiaking::media::VideoCodec::h265, 1280, 720, 60, 10000};
    description.audio_header[0] = std::byte{2};
    return description;
  }

  static auto receive_keys(int salt = 0) -> SourceReceiveKeys {
    constexpr auto size = libchiaking::net::protocol::takion::source_key_size;
    libchiaking::SecretArray<size> key;
    libchiaking::SecretArray<size> iv;
    for (std::size_t index = 0; index < size; ++index) {
      key[index] = static_cast<std::byte>(salt + static_cast<int>(index));
      iv[index] = static_cast<std::byte>(salt + 16 + static_cast<int>(index));
    }
    return SourceReceiveKeys{std::move(key), std::move(iv)};
  }

  static auto same_keys(const RecordedAudience::Started& started,
                        const SourceReceiveKeys& keys)
      -> ::testing::AssertionResult {
    if (!std::ranges::equal(started.key, keys.key()))
      return ::testing::AssertionFailure() << "the key differs";
    if (!std::ranges::equal(started.iv, keys.iv()))
      return ::testing::AssertionFailure() << "the iv differs";
    return ::testing::AssertionSuccess();
  }

  static void announce(SessionRig& rig,
                       std::uint32_t version = shareable_version,
                       int salt = 0) {
    rig.link.then([&rig, version,
                   salt](libchiaking::play::testing::ScriptedLink&) {
      rig.core->share_rules().tap().on_source_keys(version, receive_keys(salt));
    });
  }
};

}  // namespace play_test
