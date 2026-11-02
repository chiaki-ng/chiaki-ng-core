#pragma once

#include "libchiaking/play_types.hpp"
#include "libchiaking/testing/service_builder.hpp"
#include "tests/support/api/fakes.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/play/source_stream.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/kakusan/fakes.hpp"
#include "tests/unit/core/kakusan/identities.hpp"
#include "tests/unit/core/kakusan/manual_pair.hpp"
#include "tests/unit/core/kakusan/source_description.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kakusan_test {

using test_support::play::SourceStream;

class GuestVisitScreen final : public libchiaking::media::MediaSink {
 public:
  void on_stream_description(
      const libchiaking::media::StreamDescription& description) override {
    profiles = description.profiles;
    ++infos;
  }

  auto on_video(const libchiaking::media::VideoUnit& unit)
      -> libchiaking::media::VideoResult override {
    const auto bytes = unit.frame().bytes();
    frames.emplace_back(bytes.begin(), bytes.end());
    headers += unit.profile_changed() ? 1 : 0;
    return verdict;
  }

  void on_video_gap() override { ++gaps; }

  auto on_audio(const libchiaking::media::AudioUnit& unit)
      -> libchiaking::media::AudioResult override {
    if (unit.kind == libchiaking::media::AudioUnit::Kind::samples)
      audio.push_back(unit.bytes);
    return {};
  }

  void on_feedback(
      const libchiaking::controllers::ControllerFeedback& unit) override {
    feedback.push_back(unit);
  }

  auto played() const -> std::vector<std::vector<std::byte>> {
    const auto header = test_support::Bytes::unhex(SourceStream::video_header);
    std::vector<std::vector<std::byte>> found;
    std::ranges::copy_if(
        frames, std::back_inserter(found),
        [&header](const auto& frame) { return frame != header; });
    return found;
  }

  libchiaking::media::VideoResult verdict{};
  std::vector<libchiaking::media::VideoProfile> profiles;
  int infos{};
  int headers{};
  int gaps{};
  std::vector<std::vector<std::byte>> frames;
  std::vector<std::vector<std::byte>> audio;
  std::vector<libchiaking::controllers::ControllerFeedback> feedback;
};

class GuestVisitHostSide final : public manual::HostEventSink {
 public:
  void on_event(manual::HostEvent event) override {
    events.push_back(std::move(event));
    const auto& seen = events.back();
    if (const auto* code = std::get_if<manual::host_event::CodeReady>(&seen)) {
      EXPECT_TRUE(host->approve(code->guest));
    }
    if (const auto* join =
            std::get_if<manual::host_event::JoinRequested>(&seen)) {
      EXPECT_TRUE(host->source_joined(
          join->seat, libchiaking::controllers::SeatNumber::create(1).value()));
    }
  }

  template <class Kind>
  auto find() const -> const Kind* {
    for (const auto& event : events) {
      if (const auto* found = std::get_if<Kind>(&event)) return found;
    }
    return nullptr;
  }

  auto pressed() const -> bool {
    return std::ranges::any_of(events, [](const auto& event) {
      const auto* input =
          std::get_if<manual::host_event::ControllerInput>(&event);
      return input != nullptr && input->state.buttons != 0;
    });
  }

  manual::Host* host{};
  std::vector<manual::HostEvent> events;
};

class GuestVisitStage final : public libchiaking::kakusan::GuestVisit::Observer,
                              public libchiaking::kakusan::GuestVisit::Pause {
 public:
  using GuestVisit = libchiaking::kakusan::GuestVisit;
  using Refusal = libchiaking::kakusan::Refusal;

  struct Cue {
    std::function<bool()> when;
    std::function<void()> act;
  };

  GuestVisitStage() {
    const auto config = manual::SharingRules::create(4, 1);
    EXPECT_TRUE(config);
    auto environment = manual::Environment::with(
        TestPlatform::crypto(), TestPlatform::secure_datagram(), host_machine,
        host_random, clock.source());
    EXPECT_TRUE(environment);
    auto created =
        manual::Host::create(config.value(), TestPlatform::identity(0x10),
                             std::move(environment).value(), side);
    EXPECT_TRUE(created);
    host.emplace(std::move(created).value());
    side.host = &*host;
    host->source_keys(SourceStream::takion_version,
                      SourceStream::receive_keys());
    EXPECT_TRUE(host->source_started(SourceStream::description(),
                                     SourceStream::video_fps));
    guest = host->invite().value();
    for (std::size_t index = 0; index < 100 && invitation().empty(); ++index) {
      clock.now_us += 1'000;
      EXPECT_TRUE(host->pump());
    }
    EXPECT_FALSE(invitation().empty());
  }

  auto invitation() const -> std::string_view {
    const auto* ready = side.find<manual::host_event::InvitationReady>();
    return ready == nullptr ? std::string_view{} : ready->block.view();
  }

  auto make(std::string_view block, GuestVisit::Pacing pacing = {})
      -> libchiaking::Expected<std::unique_ptr<GuestVisit>, Refusal> {
    auto invitation_read = root.read(block);
    if (!invitation_read)
      return libchiaking::unexpected(invitation_read.error());
    return root.join(std::move(invitation_read).value(),
                     GuestVisit::Ports{*this, screen, this}, pacing);
  }

  void on_event(GuestVisit::Event event) override {
    events.push_back(std::move(event));
  }

  void wait(std::chrono::milliseconds limit) override {
    EXPECT_GT(limit, std::chrono::milliseconds{0});
    EXPECT_LT(waits.size(), 200'000U);
    waits.push_back(limit);
    steady.advance(limit);
    if (host_answers) {
      EXPECT_TRUE(host->pump());
    }
    while (!cues.empty() && cues.front().when()) {
      auto cue = std::move(cues.front());
      cues.pop_front();
      cue.act();
    }
  }

  void wake() noexcept override { ++wakes; }

  void cue(std::function<bool()> when, std::function<void()> act) {
    cues.push_back(Cue{std::move(when), std::move(act)});
  }

  template <class Kind>
  auto count() const -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(
        events,
        [](const auto& event) { return std::holds_alternative<Kind>(event); }));
  }

  template <class Kind>
  auto find() const -> const Kind* {
    for (const auto& event : events) {
      if (const auto* found = std::get_if<Kind>(&event)) return found;
    }
    return nullptr;
  }

  template <class Kind>
  auto index_of() const -> std::size_t {
    for (std::size_t index = 0; index < events.size(); ++index) {
      if (std::holds_alternative<Kind>(events[index])) return index;
    }
    return events.size();
  }

  auto state() const -> const GuestVisit::VisitChanged* {
    const GuestVisit::VisitChanged* found = nullptr;
    for (const auto& event : events) {
      if (const auto* changed = std::get_if<GuestVisit::VisitChanged>(&event))
        found = changed;
    }
    return found;
  }

  auto logged(std::string_view text) const -> std::size_t {
    return static_cast<std::size_t>(
        std::ranges::count_if(log.messages, [text](const std::string& line) {
          return line.find(text) != std::string::npos;
        }));
  }

  auto elapsed() const -> std::chrono::microseconds {
    return std::chrono::microseconds{clock.now_us.load()};
  }

  void pair(GuestVisit& run, bool correct = true) {
    cue([this] { return find<GuestVisit::ReplyReady>() != nullptr; },
        [this] {
          const auto imported =
              host->import_reply(find<GuestVisit::ReplyReady>()->reply.text());
          EXPECT_TRUE(imported);
          if (imported) {
            EXPECT_EQ(imported.value(), guest);
          }
        });
    cue(
        [this] {
          return find<GuestVisit::CodeToReadOut>() != nullptr &&
                 side.find<manual::host_event::CodeReady>() != nullptr;
        },
        [this, &run, correct] {
          std::string shown{
              side.find<manual::host_event::CodeReady>()->code.digits()};
          if (!correct)
            shown[3] = shown[3] == '9' ? '0' : static_cast<char>(shown[3] + 1);
          EXPECT_TRUE(host->enter_code(
              guest, find<GuestVisit::CodeToReadOut>()->code.digits()));
          EXPECT_TRUE(run.verify(*manual::VerificationCode::parse(shown)));
        });
  }

  void forward_source() {
    for (std::size_t index = 0; index < SourceStream::vectors.size(); ++index) {
      const auto bytes = SourceStream::datagram(index);
      host->source_datagram(SourceDescriptions::describe(
          SourceStream::takion_version, SourceStream::vectors[index].media,
          SourceStream::vectors[index].key_position, bytes));
      EXPECT_TRUE(host->pump());
    }
  }

  Lan lan;
  VirtualClock clock;
  SteppedClock steady{clock};
  test_support::SeededRandom host_random{0x1001};
  test_support::SeededRandom guest_random{0x5040};
  Machine host_machine{lan, 1};
  Machine guest_machine{lan, 0x40};
  test_support::FakeLog log;
  MemoryStore store{TestPlatform::identity(0x40)};
  libchiaking::kakusan::Kakusan root{
      libchiaking::testing::ServiceBuilder::on<libchiaking::kakusan::Kakusan>(
          Neighbourhood::surroundings(guest_machine, guest_random,
                                      clock.source(), store, &log))};
  GuestVisitScreen screen;
  GuestVisitHostSide side;
  std::optional<manual::Host> host;
  manual::GuestId guest;
  bool host_answers{true};
  int wakes{};
  std::vector<std::chrono::milliseconds> waits;
  std::vector<GuestVisit::Event> events;
  std::deque<Cue> cues;
};

class GuestVisitTest : public ::testing::Test {
 protected:
  using GuestVisit = libchiaking::kakusan::GuestVisit;
  using Refusal = libchiaking::kakusan::Refusal;
  using VisitOutcome = libchiaking::kakusan::VisitOutcome;
  using Stage = GuestVisitStage;

  template <class T>
  static auto refused(const libchiaking::Expected<T, Refusal>& result,
                      Refusal::Kind kind) -> ::testing::AssertionResult {
    if (result) return ::testing::AssertionFailure() << "it was not refused";
    if (result.error().kind() != kind)
      return ::testing::AssertionFailure()
             << "refused as " << result.error().name();
    return ::testing::AssertionSuccess();
  }

  static auto pressed() -> libchiaking::controllers::ControllerState {
    libchiaking::controllers::ControllerState state;
    state.buttons = static_cast<std::uint32_t>(
        libchiaking::controllers::ControllerButton::cross);
    state.left_x = 900;
    return state;
  }

  static auto rumble(std::uint8_t console_seat, std::uint8_t left,
                     std::uint8_t right)
      -> libchiaking::controllers::ControllerFeedback {
    return libchiaking::controllers::ControllerFeedback{
        libchiaking::controllers::SeatNumber::create(console_seat).value(),
        libchiaking::controllers::Rumble{left, right}};
  }
};

}  // namespace kakusan_test
