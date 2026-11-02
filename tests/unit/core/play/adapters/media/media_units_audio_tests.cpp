#include "src/core/play/adapters/media/media_units.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/media/counted_allocations.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::play::MediaUnits;

TEST(MediaUnitsAudio, TakesTheBufferInsteadOfCopyingIt) {
  remote_play::event::EncodedMedia packet{remote_play::EncodedMediaKind::audio,
                                          {}};
  packet.bytes.assign(512, std::byte{3});
  const auto* bytes = packet.bytes.data();
  const auto before = CountedAllocations::on_this_thread();
  const auto unit = MediaUnits::audio(std::move(packet));
  EXPECT_EQ(CountedAllocations::on_this_thread(), before);
  EXPECT_EQ(unit.bytes.data(), bytes);
  EXPECT_EQ(unit.bytes.size(), 512);
  EXPECT_TRUE(packet.bytes.empty());
}

TEST(MediaUnitsAudio, NamesTheSeatOfControllerSpeakerAudio) {
  remote_play::event::EncodedMedia packet{
      remote_play::EncodedMediaKind::controller_speaker, {std::byte{7}}};
  packet.pad_index = 2;
  const auto unit = MediaUnits::audio(std::move(packet));
  EXPECT_EQ(unit.kind, libchiaking::media::AudioUnit::Kind::samples);
  EXPECT_EQ(unit.controller_speaker, std::optional<std::uint8_t>{2});
  EXPECT_FALSE(unit.voice);
  const auto game = MediaUnits::audio(remote_play::event::EncodedMedia{
      remote_play::EncodedMediaKind::audio, {std::byte{8}}});
  EXPECT_FALSE(game.controller_speaker);
}

}  // namespace
}  // namespace play_test
