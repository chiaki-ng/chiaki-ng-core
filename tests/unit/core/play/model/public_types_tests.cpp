#include <libchiaking/play.hpp>
#include <libchiaking/play_types.hpp>
#include <libchiaking/testing/play/scripted_link.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <type_traits>

namespace play_test {
namespace {

namespace consoles = libchiaking::consoles;
namespace controllers = libchiaking::controllers;
namespace play = libchiaking::play;

static_assert(!std::is_copy_constructible_v<play::ReceiveKeys>);
static_assert(std::is_move_constructible_v<play::ReceiveKeys>);
static_assert(!std::is_copy_constructible_v<play::AccessCode>);
static_assert(!std::is_copy_constructible_v<play::Session>);
static_assert(!std::is_copy_constructible_v<play::Play>);
static_assert(!std::is_copy_constructible_v<play::CloudLaunch>);
static_assert(!std::is_copy_constructible_v<play::Seats>);
static_assert(!std::is_copy_constructible_v<play::Controllers>);
static_assert(!std::is_copy_constructible_v<play::LoginPin>);
static_assert(!std::is_copy_constructible_v<play::ConsoleKeyboard>);
static_assert(!std::is_copy_constructible_v<play::Microphone>);
static_assert(!std::is_copy_constructible_v<play::SessionShare>);
static_assert(!std::is_copy_constructible_v<play::SharedSeat>);
static_assert(!std::is_copy_constructible_v<play::Sharing>);
static_assert(!std::is_copy_constructible_v<play::ForwardedStream>);
static_assert(!std::is_copy_constructible_v<play::InternetPairer>);
static_assert(!std::is_copy_constructible_v<play::testing::ScriptedLink>);
static_assert(
    std::is_base_of_v<consoles::InternetPairing, play::InternetPairer>);
static_assert(std::is_abstract_v<libchiaking::media::MediaSink> &&
              std::is_abstract_v<play::SessionEvents>);
static_assert(std::is_abstract_v<play::DatacenterStore> &&
              std::is_abstract_v<play::SharedAudience>);

TEST(ControllerState, StartsIdleWithGravityAndNoTouches) {
  const controllers::ControllerState state;
  EXPECT_EQ(state.buttons, 0);
  EXPECT_EQ(state.accel_y, 1.0F);
  EXPECT_EQ(state.orient_w, 1.0F);
  EXPECT_EQ(state.touches[0].id, -1);
}

TEST(ControllerState, HoldsSixteenButtons) {
  EXPECT_EQ(static_cast<std::uint32_t>(controllers::ControllerButton::ps),
            0x8000U);
  EXPECT_EQ(controllers::ControllerState::button_mask, 0xffff);
  EXPECT_EQ(controllers::ControllerState::button_count, 16);
}

TEST(LoginPin, ComparesTicketsById) {
  EXPECT_EQ(play::LoginPin::Ticket{4}, play::LoginPin::Ticket{4});
  EXPECT_EQ(play::LoginPin::Ticket{4}.id(), 4u);
  EXPECT_EQ(play::LoginPin::default_await, std::chrono::seconds{10});
}

TEST(Microphone, KeepsItsTicketTagAndFrameSize) {
  EXPECT_EQ(play::Microphone::ConnectTicket(1, 2).tag(), 2u);
  EXPECT_EQ(play::Microphone::frame_size, 40u);
  EXPECT_EQ(play::Microphone::Statistics{}, play::Microphone::Statistics{});
}

TEST(ConsoleKeyboard, LimitsTheText) {
  EXPECT_EQ(play::ConsoleKeyboard::text_limit, 4096u);
}

TEST(Statistics, StartEmpty) {
  EXPECT_EQ(play::Statistics{}.controller_pending, 0u);
  EXPECT_FALSE(play::Statistics{}.protocol_version);
  EXPECT_EQ(play::LinkStatistics{}.service_marks.dscp_counts.size(), 64u);
}

TEST(StreamDescription, SizesTheAudioHeaderAndChannels) {
  EXPECT_EQ(libchiaking::media::StreamDescription::audio_header_size, 14u);
  EXPECT_EQ(libchiaking::media::AudioFormat::max_channels, 12u);
}

TEST(PlayEnums, TellTheirValuesApart) {
  EXPECT_NE(play::InputRefusal::invalid, play::InputRefusal::not_ready);
  EXPECT_NE(play::Where::home, play::Where::cloud);
  EXPECT_NE(play::Leave::disconnect, play::Leave::rest_console);
}

TEST(GuestSeat, DefaultsToTheConsoleController) {
  const play::SessionShare::GuestSeat request{play::GuestTag{3}};
  EXPECT_EQ(request.controller, controllers::ControllerKind::console_default);
  EXPECT_FALSE(request.as);
  EXPECT_FALSE(request.preferred);
}

}  // namespace
}  // namespace play_test
