#pragma once

#include "libchiaking/detail/controllers/feedback_inbox.hpp"
#include "src/core/play/adapters/media/media_units.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/live_providers.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace play_test {

class FeedbackRig : public ::testing::Test {
 protected:
  using Inbox = libchiaking::controllers::FeedbackInbox;
  using Admission = Inbox::Admission;
  using SeatNumber = libchiaking::controllers::SeatNumber;
  using HapticAudio = libchiaking::controllers::HapticAudio;
  using TriggerEffects = libchiaking::controllers::TriggerEffects;
  using MediaKind = libchiaking::net::protocol::remote_play::EncodedMediaKind;
  using EncodedMedia =
      libchiaking::net::protocol::remote_play::event::EncodedMedia;

  static constexpr Inbox::Clock::time_point start{std::chrono::hours{1}};

  static auto seat(std::size_t index) -> SeatNumber {
    return SeatNumber::create(index).value();
  }

  static auto filled(std::span<const std::byte> bytes, std::uint8_t value)
      -> bool {
    return std::ranges::all_of(
        bytes, [value](std::byte byte) { return byte == std::byte{value}; });
  }

  static auto haptics(Inbox& inbox, std::size_t size, std::uint8_t fill,
                      std::size_t index = 0,
                      Inbox::Clock::time_point now = start) -> Admission {
    const std::vector<std::byte> pcm(size, std::byte{fill});
    return inbox.accept({seat(index), HapticAudio{pcm}}, now);
  }

  static auto triggers(std::uint8_t left_mode = 0x21) -> TriggerEffects {
    TriggerEffects effects;
    effects.left.mode = left_mode;
    effects.left.parameters[0] = 0xaa;
    effects.right.mode = 0x26;
    effects.right.parameters[0] = 0xbb;
    effects.right.parameters[9] = 0x08;
    return effects;
  }

  static auto wire(MediaKind kind, std::vector<std::byte> bytes,
                   std::uint8_t pad_index = 0) -> EncodedMedia {
    EncodedMedia media{kind, std::move(bytes)};
    media.pad_index = pad_index;
    return media;
  }

  static auto trigger_body(std::uint8_t pad_index) -> std::vector<std::byte> {
    std::vector<std::byte> body(25);
    body[0] = std::byte{pad_index};
    body[1] = std::byte{0x21};
    body[2] = std::byte{0x26};
    body[5] = std::byte{0xaa};
    body[15] = std::byte{0xbb};
    body[24] = std::byte{0x08};
    return body;
  }
};

}  // namespace play_test
