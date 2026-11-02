#pragma once

#include "libchiaking/detail/kakusan/values.hpp"
#include "libchiaking/detail/media/media_sink.hpp"
#include "libchiaking/detail/play/controllers.hpp"
#include "libchiaking/detail/play/session_share.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/kakusan/fakes.hpp"
#include "tests/unit/core/kakusan/manual_pair.hpp"
#include "tests/unit/core/kakusan/source_description.hpp"
#include "tests/support/play/source_stream.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kakusan_test {

using test_support::play::SourceStream;

class ManualPair {
 public:
  struct PublicIdentity {
    manual::IdentityKey key;
    manual::Fingerprint fingerprint;
  };

  static auto derive(const manual::DeviceIdentity& identity) -> PublicIdentity {
    return PublicIdentity{identity.key(), identity.fingerprint()};
  }

  template <class Target>
  static auto pump(Target& target) -> manual::Result<void> {
    inside_pump_ = true;
    const auto result = target.pump();
    inside_pump_ = false;
    return result;
  }

  static auto inside_pump() noexcept -> bool { return inside_pump_; }

  template <class T>
  static auto failed_with(const manual::Result<T>& result, manual::Error error)
      -> testing::AssertionResult {
    if (result) return testing::AssertionFailure() << "it succeeded";
    if (result.error() != error)
      return testing::AssertionFailure()
             << "it failed with " << testing::PrintToString(result.error());
    return testing::AssertionSuccess();
  }

  static auto pressed_state() -> libchiaking::controllers::ControllerState {
    libchiaking::controllers::ControllerState pressed;
    pressed.buttons = static_cast<std::uint32_t>(
                          libchiaking::controllers::ControllerButton::cross) |
                      static_cast<std::uint32_t>(
                          libchiaking::controllers::ControllerButton::dpad_up);
    pressed.left_x = 12000;
    pressed.left_y = -32768;
    pressed.right_y = -32767;
    pressed.l2 = 255;
    pressed.r2 = 100;
    return pressed;
  }

  static auto rumble(std::uint8_t console_seat, std::uint8_t left,
                     std::uint8_t right)
      -> libchiaking::controllers::ControllerFeedback {
    return libchiaking::controllers::ControllerFeedback{
        libchiaking::controllers::SeatNumber::create(console_seat).value(),
        libchiaking::controllers::Rumble{left, right}};
  }

  static auto seat(std::uint8_t number)
      -> libchiaking::controllers::SeatNumber {
    return libchiaking::controllers::SeatNumber::create(number).value();
  }

 private:
  static inline thread_local bool inside_pump_ = false;
};

class ManualHostSide final : public manual::HostEventSink {
 public:
  struct Input : libchiaking::controllers::ControllerState {
    std::uint8_t controller_id{};
  };

  void on_event(manual::HostEvent event) override {
    EXPECT_TRUE(ManualPair::inside_pump());
    events.push_back(std::move(event));
    const auto& seen = events.back();
    if (const auto* code = std::get_if<manual::host_event::CodeReady>(&seen);
        code != nullptr && approve_inside_callback) {
      const auto nested = host->pump();
      EXPECT_TRUE(
          ManualPair::failed_with(nested, manual::Error::invalid_argument));
      EXPECT_LE(host->next_wait(std::chrono::milliseconds{50}),
                std::chrono::milliseconds{50});
      EXPECT_TRUE(host->approve(code->guest));
    }
    if (const auto* join =
            std::get_if<manual::host_event::JoinRequested>(&seen);
        join != nullptr && join_inside_callback) {
      EXPECT_GT(join->seat.number, 0U);
      EXPECT_GT(join->seat.generation, 0U);
      pads.emplace_back(join->guest, next_pad);
      EXPECT_TRUE(
          host->source_joined(join->seat, ManualPair::seat(next_pad++)));
    }
  }

  template <class Kind>
  auto count() const -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(
        events,
        [](const auto& event) { return std::holds_alternative<Kind>(event); }));
  }

  template <class Kind>
  auto index_of(const manual::GuestId& guest) const -> std::size_t {
    for (std::size_t index = 0; index < events.size(); ++index) {
      const auto* found = std::get_if<Kind>(&events[index]);
      if (found != nullptr && found->guest == guest) return index;
    }
    return events.size();
  }

  template <class Kind>
  auto find(const manual::GuestId& guest) const -> const Kind* {
    const auto index = index_of<Kind>(guest);
    return index == events.size() ? nullptr : std::get_if<Kind>(&events[index]);
  }

  auto last_state(const manual::GuestId& guest) const
      -> const manual::host_event::StateChanged* {
    const manual::host_event::StateChanged* found = nullptr;
    for (const auto& event : events) {
      const auto* changed =
          std::get_if<manual::host_event::StateChanged>(&event);
      if (changed != nullptr && changed->guest == guest) found = changed;
    }
    return found;
  }

  auto state(const manual::GuestId& guest) const -> manual::Progress {
    const auto* found = last_state(guest);
    return found == nullptr ? manual::Progress::waiting : found->state;
  }

  auto inputs() const -> std::vector<Input> {
    std::vector<Input> found;
    for (const auto& event : events) {
      const auto* input =
          std::get_if<manual::host_event::ControllerInput>(&event);
      if (input == nullptr) continue;
      const auto pad = std::ranges::find(
          pads, input->guest, &std::pair<manual::GuestId, std::uint8_t>::first);
      if (pad == pads.end()) {
        ADD_FAILURE() << "input from a guest without a pad";
        continue;
      }
      found.push_back(Input{input->state, pad->second});
    }
    return found;
  }

  auto monotonic(const manual::GuestId& guest) const
      -> testing::AssertionResult {
    manual::Progress previous = manual::Progress::waiting;
    for (const auto& event : events) {
      const auto* changed =
          std::get_if<manual::host_event::StateChanged>(&event);
      if (changed == nullptr || changed->guest != guest) continue;
      if (changed->state < previous)
        return testing::AssertionFailure()
               << testing::PrintToString(changed->state) << " came after "
               << testing::PrintToString(previous);
      previous = changed->state;
    }
    return testing::AssertionSuccess();
  }

  manual::Host* host{};
  std::vector<manual::HostEvent> events;
  std::vector<std::pair<manual::GuestId, std::uint8_t>> pads;
  std::uint8_t next_pad{1};
  bool join_inside_callback{true};
  bool approve_inside_callback{true};
};

class ManualStream {
 public:
  enum class Media : std::uint8_t { audio, video };

  std::optional<libchiaking::media::StreamDescription> info;
  std::optional<Media> media;
  std::vector<std::byte> bytes;
  std::size_t order{};
};

class ManualPlayer final : public manual::GuestEventSink,
                           public libchiaking::media::MediaSink {
 public:
  struct Seen {
    manual::GuestEvent event;
    std::size_t order{};
  };

  ManualPlayer(Lan& lan, std::uint8_t tag, VirtualClock& clock)
      : random(0x5000U + tag),
        machine(std::in_place, lan, tag),
        seed(TestPlatform::identity(tag)) {
    open(manual::Environment::with(TestPlatform::crypto(),
                                   TestPlatform::secure_datagram(), *machine,
                                   random, clock.source()));
  }

  explicit ManualPlayer(std::uint8_t tag)
      : random(0x5000U + tag), seed(TestPlatform::identity(tag)) {
    open(manual::Environment::system(TestPlatform::crypto(),
                                     TestPlatform::secure_datagram()));
  }

  ManualPlayer(std::uint8_t tag,
               manual::Result<manual::Environment> environment)
      : random(0x5000U + tag), seed(TestPlatform::identity(tag)) {
    open(std::move(environment));
  }

  ManualPlayer(const ManualPlayer&) = delete;
  auto operator=(const ManualPlayer&) -> ManualPlayer& = delete;

  void on_event(manual::GuestEvent event) override {
    EXPECT_TRUE(ManualPair::inside_pump());
    const bool requested =
        std::holds_alternative<manual::guest_event::ControllerRequested>(event);
    events.push_back(Seen{std::move(event), order++});
    if (requested && ready_inside_callback) {
      const auto nested = guest->pump();
      EXPECT_TRUE(
          ManualPair::failed_with(nested, manual::Error::invalid_argument));
      EXPECT_TRUE(guest->controller_ready());
    }
  }

  void on_stream_description(
      const libchiaking::media::StreamDescription& description) override {
    EXPECT_TRUE(ManualPair::inside_pump());
    ManualStream record;
    record.order = order++;
    record.info = description;
    streams.push_back(std::move(record));
  }

  auto on_video(const libchiaking::media::VideoUnit& unit)
      -> libchiaking::media::VideoResult override {
    EXPECT_TRUE(ManualPair::inside_pump());
    const auto& frame = unit.frame();
    EXPECT_EQ(frame.padded_bytes().size(),
              frame.size() + libchiaking::media::VideoFrame::tail_padding);
    const auto bytes = frame.bytes();
    streams.push_back(ManualStream{std::nullopt,
                                   ManualStream::Media::video,
                                   {bytes.begin(), bytes.end()},
                                   order++});
    return {};
  }

  void on_video_gap() override { EXPECT_TRUE(ManualPair::inside_pump()); }

  auto on_audio(const libchiaking::media::AudioUnit& unit)
      -> libchiaking::media::AudioResult override {
    EXPECT_TRUE(ManualPair::inside_pump());
    if (unit.kind == libchiaking::media::AudioUnit::Kind::samples)
      streams.push_back(ManualStream{std::nullopt, ManualStream::Media::audio,
                                     unit.bytes, order++});
    return {};
  }

  void on_feedback(
      const libchiaking::controllers::ControllerFeedback&) override {
    EXPECT_TRUE(ManualPair::inside_pump());
  }

  template <class Kind>
  auto find() const -> const Kind* {
    for (const auto& seen : events) {
      if (const auto* found = std::get_if<Kind>(&seen.event)) return found;
    }
    return nullptr;
  }

  template <class Kind>
  auto order_of() const -> std::size_t {
    for (const auto& seen : events) {
      if (std::holds_alternative<Kind>(seen.event)) return seen.order;
    }
    return order;
  }

  template <class Kind>
  auto count() const -> std::size_t {
    return static_cast<std::size_t>(
        std::ranges::count_if(events, [](const auto& seen) {
          return std::holds_alternative<Kind>(seen.event);
        }));
  }

  auto last_state() const -> const manual::guest_event::StateChanged* {
    const manual::guest_event::StateChanged* found = nullptr;
    for (const auto& seen : events) {
      if (const auto* changed =
              std::get_if<manual::guest_event::StateChanged>(&seen.event))
        found = changed;
    }
    return found;
  }

  auto frames(ManualStream::Media kind) const
      -> std::vector<std::vector<std::byte>> {
    const auto header = test_support::Bytes::unhex(SourceStream::video_header);
    std::vector<std::vector<std::byte>> found;
    for (const auto& record : streams) {
      if (record.media == kind && record.bytes != header)
        found.push_back(record.bytes);
    }
    return found;
  }

  test_support::SeededRandom random;
  std::optional<Machine> machine;
  manual::DeviceIdentity seed;
  manual::GuestId id{};
  std::vector<Seen> events;
  std::vector<ManualStream> streams;
  std::size_t order{};
  bool ready_inside_callback{true};
  std::optional<manual::Guest> guest;

 private:
  void open(manual::Result<manual::Environment> environment) {
    EXPECT_TRUE(environment);
    if (!environment) return;
    auto created = manual::Guest::create(seed, std::move(environment).value(),
                                         *this, *this);
    EXPECT_TRUE(created);
    if (!created) return;
    guest.emplace(std::move(created).value());
  }
};

class ManualWorld {
 public:
  ManualWorld() = default;
  ManualWorld(const ManualWorld&) = delete;
  auto operator=(const ManualWorld&) -> ManualWorld& = delete;

  void start() {
    const auto config = manual::SharingRules::create(4, 1, manual::all_buttons);
    ASSERT_TRUE(config);
    auto environment = manual::Environment::with(
        TestPlatform::crypto(), TestPlatform::secure_datagram(), host_machine,
        host_random, clock.source());
    ASSERT_TRUE(environment);
    auto created = manual::Host::create(config.value(), host_seed,
                                        std::move(environment).value(), side);
    ASSERT_TRUE(created);
    host.emplace(std::move(created).value());
    side.host = &*host;
  }

  void start_source() {
    host->source_keys(SourceStream::takion_version,
                      SourceStream::receive_keys());
    ASSERT_TRUE(host->source_started(SourceStream::description(),
                                     SourceStream::video_fps));
  }

  void step(std::uint64_t advance_us = 1'000) {
    clock.now_us += advance_us;
    ASSERT_TRUE(ManualPair::pump(*host));
    for (auto& player : players) ASSERT_TRUE(ManualPair::pump(*player->guest));
    ASSERT_TRUE(ManualPair::pump(*host));
  }

  template <class Done>
  void until(Done done, std::size_t limit = 20'000) {
    for (std::size_t index = 0; index < limit && !done(); ++index)
      ASSERT_NO_FATAL_FAILURE(step());
    ASSERT_TRUE(done());
  }

  void steps(std::size_t count, std::uint64_t advance_us = 1'000) {
    for (std::size_t index = 0; index < count; ++index)
      ASSERT_NO_FATAL_FAILURE(step(advance_us));
  }

  auto code(const manual::GuestId& guest) const -> std::string {
    const auto* found = side.find<manual::host_event::CodeReady>(guest);
    return found == nullptr ? std::string{} : std::string{found->code.digits()};
  }

  auto latest() -> ManualPlayer& { return *players.back(); }

  void pair(std::uint8_t tag) {
    const auto invited = host->invite();
    ASSERT_TRUE(invited);
    const auto guest = invited.value();
    ASSERT_NO_FATAL_FAILURE(until([&] {
      return side.find<manual::host_event::InvitationReady>(guest) != nullptr;
    }));
    const auto* invitation =
        side.find<manual::host_event::InvitationReady>(guest);
    const auto block = invitation->block.view();
    EXPECT_FALSE(block.empty());
    EXPECT_GT(invitation->expires_at, clock.start_wall);

    const auto host_identity = ManualPair::derive(host_seed);
    const auto preview = manual::Guest::preview(TestPlatform::crypto(), block);
    ASSERT_TRUE(preview);
    EXPECT_EQ(preview.value().host_hint,
              Identities::hint_of(host_identity.key));
    EXPECT_EQ(preview.value().seats.capacity, 4U);
    EXPECT_EQ(preview.value().expires_at, invitation->expires_at);
    EXPECT_EQ(preview.value().stream_protocol,
              SourceStream::takion_version);

    players.push_back(std::make_unique<ManualPlayer>(lan, tag, clock));
    auto& player = *players.back();
    ASSERT_TRUE(player.guest);
    player.id = guest;
    std::vector<char> unterminated{block.begin(), block.end()};
    unterminated.push_back('X');
    const auto accepted = player.guest->accept_invitation(
        std::string_view{unterminated.data(), block.size()});
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted.value().host_hint,
              Identities::hint_of(host_identity.key));
    EXPECT_EQ(accepted.value().expires_at, preview.value().expires_at);
    EXPECT_EQ(accepted.value().seats.capacity, 4U);
    EXPECT_EQ(accepted.value().stream_protocol,
              preview.value().stream_protocol);
    ASSERT_NO_FATAL_FAILURE(until([&] {
      return player.find<manual::guest_event::ReplyReady>() != nullptr;
    }));
    const auto reply =
        player.find<manual::guest_event::ReplyReady>()->block.view();
    EXPECT_FALSE(reply.empty());

    const auto imported = host->import_reply(reply);
    ASSERT_TRUE(imported);
    EXPECT_EQ(imported.value(), guest);
    ASSERT_NO_FATAL_FAILURE(until([&] {
      return !code(guest).empty() &&
             player.find<manual::guest_event::CodeReady>() != nullptr;
    }));

    const auto* bound = side.find<manual::host_event::ReplyBound>(guest);
    ASSERT_NE(bound, nullptr);
    const auto guest_identity = ManualPair::derive(player.seed);
    EXPECT_EQ(bound->identity, guest_identity.key);
    EXPECT_EQ(bound->fingerprint, guest_identity.fingerprint);
  }

  void enter_codes(ManualPlayer& player, bool correct = true) {
    const std::string guest_code{
        player.find<manual::guest_event::CodeReady>()->code.digits()};
    auto host_code = code(player.id);
    ASSERT_EQ(guest_code.size(), manual::VerificationCode::size);
    ASSERT_EQ(host_code.size(), manual::VerificationCode::size);
    EXPECT_TRUE(std::ranges::all_of(
        guest_code, [](char digit) { return digit >= '0' && digit <= '9'; }));
    if (!correct)
      host_code[3] =
          host_code[3] == '9' ? '0' : static_cast<char>(host_code[3] + 1);
    EXPECT_TRUE(host->enter_code(player.id, guest_code));
    EXPECT_TRUE(player.guest->enter_code(host_code));
  }

  void join(std::uint8_t tag) {
    ASSERT_NO_FATAL_FAILURE(pair(tag));
    auto& player = latest();
    ASSERT_NO_FATAL_FAILURE(enter_codes(player));
    ASSERT_NO_FATAL_FAILURE(until([&] {
      return player.guest->state() == manual::Progress::playing &&
             side.state(player.id) == manual::Progress::playing;
    }));
    ASSERT_NO_FATAL_FAILURE(steps(20));
  }

  void forward_source() {
    for (std::size_t index = 0; index < SourceStream::vectors.size();
         ++index) {
      const auto bytes = SourceStream::datagram(index);
      host->source_datagram(SourceDescriptions::describe(
          SourceStream::takion_version,
          SourceStream::vectors[index].media,
          SourceStream::vectors[index].key_position, bytes));
      EXPECT_EQ(host->next_wait(std::chrono::milliseconds{1000}),
                std::chrono::milliseconds{0});
      ASSERT_NO_FATAL_FAILURE(step());
    }
    ASSERT_NO_FATAL_FAILURE(steps(100));
  }

  Lan lan;
  VirtualClock clock;
  test_support::SeededRandom host_random{0x1001};
  Machine host_machine{lan, 1};
  manual::DeviceIdentity host_seed{TestPlatform::identity(0x10)};
  ManualHostSide side;
  std::optional<manual::Host> host;
  std::vector<std::unique_ptr<ManualPlayer>> players;
};

class ManualWorldTest : public testing::Test {
 protected:
  using Error = manual::Error;
  using GuestId = manual::GuestId;
  using Progress = manual::Progress;

  void SetUp() override { ASSERT_NO_FATAL_FAILURE(world.start()); }

  ManualWorld world;
};

}  // namespace kakusan_test
