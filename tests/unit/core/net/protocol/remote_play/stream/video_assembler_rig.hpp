#pragma once

#include "src/core/net/protocol/remote_play/stream/video_assembler.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

namespace remote_play_test {

class VideoAssemblerRig : public ::testing::Test {
 protected:
  using Pushed = libchiaking::Expected<
      std::optional<
          libchiaking::net::protocol::remote_play::AssembledVideoFrame>,
      libchiaking::net::protocol::remote_play::VideoAssemblerError>;

  static auto bytes(std::initializer_list<int> values)
      -> std::vector<std::byte> {
    std::vector<std::byte> out;
    for (const auto value : values)
      out.push_back(static_cast<std::byte>(value));
    return out;
  }

  static auto unit(std::uint16_t frame, std::uint16_t index,
                   std::uint16_t total, std::uint16_t fec,
                   std::vector<std::byte> data)
      -> libchiaking::net::protocol::takion::AvPacketValues {
    return libchiaking::net::protocol::takion::AvPacketValues{
        libchiaking::net::protocol::takion::PacketType{0x02},
        false,
        0,
        frame,
        index,
        total,
        fec,
        0x65,
        0,
        0,
        std::move(data)};
  }

  static auto error_of(const Pushed& pushed) -> std::optional<
      libchiaking::net::protocol::remote_play::VideoAssemblerError> {
    if (pushed) return std::nullopt;
    return pushed.error();
  }

  static auto pending(const Pushed& pushed) -> testing::AssertionResult {
    if (!pushed)
      return testing::AssertionFailure() << "the unit was refused with error "
                                         << static_cast<int>(pushed.error());
    if (pushed.value())
      return testing::AssertionFailure() << "the unit completed a frame";
    return testing::AssertionSuccess();
  }

  static auto completes(const Pushed& pushed) -> testing::AssertionResult {
    if (!pushed)
      return testing::AssertionFailure() << "the unit was refused with error "
                                         << static_cast<int>(pushed.error());
    if (!pushed.value())
      return testing::AssertionFailure() << "the unit completed no frame";
    return testing::AssertionSuccess();
  }

  static auto loss_is(
      const std::optional<
          libchiaking::net::protocol::remote_play::VideoFrameLoss>& loss,
      std::uint16_t first, std::uint16_t last, bool fec_failure)
      -> testing::AssertionResult {
    if (!loss) return testing::AssertionFailure() << "no loss was reported";
    if (loss->first != first || loss->last != last ||
        loss->fec_failure != fec_failure)
      return testing::AssertionFailure()
             << "the loss is " << loss->first << ".." << loss->last
             << " fec_failure " << loss->fec_failure << ", not " << first
             << ".." << last << " fec_failure " << fec_failure;
    return testing::AssertionSuccess();
  }

  libchiaking::net::protocol::remote_play::VideoAssembler assembler;
};

}  // namespace remote_play_test
