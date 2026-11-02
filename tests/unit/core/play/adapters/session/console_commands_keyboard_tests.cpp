#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/console_commands_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace play_test {
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using play::InputRefusal;
using play::KeyboardState;

class ConsoleKeyboard : public testing::Test {
 protected:
  auto console() -> ConsoleRig& { return console_; }
  auto keyboard() -> play::ConsoleKeyboard::Core& {
    return console_.commands().keyboard();
  }

  void open(std::string text) {
    console_.commands().observe(
        remote_play::event::KeyboardOpen{std::move(text)});
  }

  void type_and_accept() {
    open("abc");
    EXPECT_TRUE(keyboard().set_text("hi"));
    EXPECT_TRUE(keyboard().set_text("hello"));
    console_.streaming();
    EXPECT_TRUE(keyboard().set_text("done"));
    EXPECT_TRUE(keyboard().accept());
    console_.streaming();
  }

 private:
  ConsoleRig console_;
};

TEST_F(ConsoleKeyboard, HoldsUpToFourKibibytesOfText) {
  EXPECT_EQ(play::ConsoleKeyboard::text_limit, 4096U);
}

TEST_F(ConsoleKeyboard, RefusesInputUntilTheConsoleOpensIt) {
  EXPECT_TRUE(ConsoleRig::refused(keyboard().set_text("text"),
                                  InputRefusal::not_ready));
  EXPECT_TRUE(ConsoleRig::refused(keyboard().set_text(std::string(4097, 'a')),
                                  InputRefusal::invalid));
  EXPECT_TRUE(
      ConsoleRig::refused(keyboard().accept(), InputRefusal::not_ready));
  EXPECT_TRUE(
      ConsoleRig::refused(keyboard().reject(), InputRefusal::not_ready));
}

TEST_F(ConsoleKeyboard, CoalescesTextWithItsClose) {
  open("abc");
  EXPECT_TRUE(keyboard().active());
  EXPECT_EQ(console().logged("Console keyboard opened: length=3"), 1U);
  EXPECT_TRUE(keyboard().set_text("hi"));
  EXPECT_TRUE(keyboard().set_text("hello"));
  EXPECT_TRUE(keyboard().pending());
  EXPECT_TRUE(console().commands().pending(true));
  EXPECT_FALSE(console().commands().pending(false));
  console().streaming();
  ASSERT_EQ(console().session().commands.size(), 1U);
  const auto* typed = std::get_if<remote_play::command::KeyboardSetText>(
      &console().session().commands[0]);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed->text.view(), "hello");

  EXPECT_TRUE(keyboard().set_text("done"));
  EXPECT_TRUE(keyboard().accept());
  EXPECT_FALSE(keyboard().active());
  EXPECT_TRUE(ConsoleRig::refused(keyboard().set_text("late"),
                                  InputRefusal::not_ready));
  console().streaming();
  EXPECT_EQ(console().session().calls_between_pumps(0),
            (std::vector<std::string>{"keyboard_text", "keyboard_text",
                                      "keyboard_accept"}));
}

TEST_F(ConsoleKeyboard, SendsARejectionAndTurnsARefusedTextIntoOne) {
  type_and_accept();
  open("");
  EXPECT_TRUE(keyboard().reject());
  console().streaming();
  EXPECT_EQ(console().session().count("keyboard_reject"), 1U);
  EXPECT_EQ(console().session().count("keyboard_text"), 2U);

  open("");
  console().session().refused_command =
      ConsoleRig::index_of<remote_play::command::KeyboardSetText>();
  EXPECT_TRUE(keyboard().set_text("lost"));
  EXPECT_TRUE(keyboard().accept());
  console().streaming();
  EXPECT_EQ(console().session().count("keyboard_accept"), 1U);
  EXPECT_EQ(console().session().count("keyboard_reject"), 2U);
}

TEST_F(ConsoleKeyboard, DropsPendingTextWhenTheConsoleClosesIt) {
  open("");
  EXPECT_TRUE(keyboard().set_text("x"));
  console().commands().observe(remote_play::event::KeyboardRemoteClose{});
  EXPECT_FALSE(keyboard().active());
  EXPECT_FALSE(keyboard().pending());
}

TEST(ConsoleKeyboardStates, KeepTheirModifiersLedsAndKeys) {
  const std::array<std::uint16_t, 3> keys{0x04, 0x2c, 0x28};
  const auto state = KeyboardState::create(
      KeyboardState::left_shift | KeyboardState::right_gui,
      KeyboardState::caps_lock, keys);
  ASSERT_TRUE(state);
  EXPECT_EQ(state->modifiers(), 0x82);
  EXPECT_EQ(state->leds(), 0x02);
  EXPECT_TRUE(std::ranges::equal(state->keys(), keys));
  EXPECT_TRUE(KeyboardState{}.keys().empty());
}

TEST(ConsoleKeyboardStates, RefuseRepeatedZeroOrTooManyKeysAndUnknownLeds) {
  const std::array<std::uint16_t, 2> repeated{0x04, 0x04};
  const std::array<std::uint16_t, 1> zero{0};
  std::array<std::uint16_t, KeyboardState::max_keys + 1> crowded{};
  for (std::size_t index = 0; index < crowded.size(); ++index)
    crowded[index] = static_cast<std::uint16_t>(index + 4);
  EXPECT_FALSE(KeyboardState::create(0, 0, repeated));
  EXPECT_FALSE(KeyboardState::create(0, 0, zero));
  EXPECT_FALSE(KeyboardState::create(0, 0, crowded));
  EXPECT_TRUE(KeyboardState::create(
      0, 0, std::span{crowded}.first(KeyboardState::max_keys)));
  EXPECT_FALSE(KeyboardState::create(0, KeyboardState::max_leds + 1, {}));
}

TEST(ConsoleKeyboardStates, ReachTheLinkOnEveryRoute) {
  const std::array<std::uint16_t, 2> keys{0x04, 0x05};
  const auto state = *KeyboardState::create(KeyboardState::left_shift, 0, keys);
  for (const auto route :
       {ConsoleRig::Venue::home(), ConsoleRig::Venue::internet(),
        ConsoleRig::cloud()}) {
    SCOPED_TRACE(testing::PrintToString(route.where()));
    ConsoleRig console{route};
    EXPECT_TRUE(console.commands().send_keyboard(state));
    EXPECT_EQ(console.session().count("keyboard"), 1U);
    EXPECT_EQ(console.session().keyboard, state);
    EXPECT_TRUE(console.commands().send_keyboard(KeyboardState{}));
    EXPECT_EQ(console.session().count("keyboard"), 2U);
    EXPECT_TRUE(console.session().keyboard.keys().empty());
  }
}

TEST(ConsoleKeyboardStates, AreRefusedBeforeTheConsoleRuns) {
  const std::array<std::uint16_t, 2> keys{0x04, 0x05};
  const auto state = *KeyboardState::create(KeyboardState::left_shift, 0, keys);
  ConsoleRig idle{ConsoleRig::Venue::home(), false};
  EXPECT_TRUE(ConsoleRig::refused(idle.commands().send_keyboard(state),
                                  InputRefusal::not_running));
  EXPECT_EQ(idle.session().count("keyboard"), 0U);
}

}  // namespace
}  // namespace play_test
