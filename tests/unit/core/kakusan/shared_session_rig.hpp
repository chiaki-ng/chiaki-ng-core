#pragma once

#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/testing/service_builder.hpp"
#include "src/core/net/protocol/takion/source_tap.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/play/session_rig.hpp"
#include "tests/support/play/source_stream.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/kakusan/fakes.hpp"
#include "tests/unit/core/kakusan/identities.hpp"
#include "tests/unit/core/kakusan/manual_pair.hpp"
#include "tests/unit/core/kakusan/source_description.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kakusan_test {

using test_support::play::SourceStream;

class SharingValues {
 public:
  using SeatNumber = libchiaking::controllers::SeatNumber;
  using Refusal = libchiaking::kakusan::Refusal;

  static auto seat(std::size_t index) -> SeatNumber {
    return SeatNumber::create(index).value();
  }

  static auto steady_clock() -> manual::Environment::ClockSource {
    return [] {
      return manual::Clock{TestPlatform::steady().now_microseconds(),
                           TestPlatform::wall().now_microseconds() / 1'000'000};
    };
  }

  static void nap(std::chrono::milliseconds duration) {
    libchiaking::platform::Sleeper sleeper{TestPlatform::threading(),
                                           TestPlatform::steady()};
    sleeper.sleep_for(duration);
  }

  template <class T>
  static auto refused(const libchiaking::Expected<T, Refusal>& result,
                      Refusal::Kind kind) -> ::testing::AssertionResult {
    if (result) return ::testing::AssertionFailure() << "it was not refused";
    if (result.error().kind() != kind)
      return ::testing::AssertionFailure()
             << "it was refused as " << result.error().name();
    return ::testing::AssertionSuccess();
  }

  static auto pressed() -> libchiaking::controllers::ControllerState {
    libchiaking::controllers::ControllerState state;
    state.buttons = static_cast<std::uint32_t>(
                        libchiaking::controllers::ControllerButton::cross) |
                    static_cast<std::uint32_t>(
                        libchiaking::controllers::ControllerButton::dpad_up);
    state.left_x = 12000;
    state.l2 = 255;
    return state;
  }

  static auto rumble(SeatNumber console_seat, std::uint8_t left,
                     std::uint8_t right)
      -> libchiaking::controllers::ControllerFeedback {
    return libchiaking::controllers::ControllerFeedback{
        console_seat, libchiaking::controllers::Rumble{left, right}};
  }
};

class SharedLog final : public libchiaking::platform::DiagnosticsProvider {
 public:
  void write(libchiaking::platform::LogRecord record) noexcept override {
    try {
      const libchiaking::platform::Lock lock{mutex_};
      lines_.emplace_back(record.message);
    } catch (...) {
    }
  }

  auto logged(std::string_view text) const -> std::size_t {
    const libchiaking::platform::Lock lock{mutex_};
    return static_cast<std::size_t>(
        std::ranges::count_if(lines_, [text](const std::string& line) {
          return line.find(text) != std::string::npos;
        }));
  }

 private:
  mutable libchiaking::platform::Mutex mutex_{TestPlatform::threading()};
  std::vector<std::string> lines_;
};

class ShareRecorder final
    : public libchiaking::kakusan::SharedSession::Observer {
 public:
  using SharedSession = libchiaking::kakusan::SharedSession;

  void on_event(SharedSession::Event event) override {
    const SharedSession::Event* seen = nullptr;
    {
      const libchiaking::platform::Lock lock{mutex_};
      events_.push_back(std::move(event));
      seen = &events_.back();
    }
    if (const auto* code = std::get_if<SharedSession::CodeToReadOut>(seen)) {
      EXPECT_TRUE(share->give_seat(code->guest, seating));
    }
  }

  auto size() const -> std::size_t {
    const libchiaking::platform::Lock lock{mutex_};
    return events_.size();
  }

  template <class Kind>
  auto count() const -> std::size_t {
    const libchiaking::platform::Lock lock{mutex_};
    return static_cast<std::size_t>(std::ranges::count_if(
        events_,
        [](const auto& event) { return std::holds_alternative<Kind>(event); }));
  }

  template <class Kind>
  auto find() const -> const Kind* {
    const libchiaking::platform::Lock lock{mutex_};
    for (const auto& event : events_) {
      if (const auto* found = std::get_if<Kind>(&event)) return found;
    }
    return nullptr;
  }

  template <class Kind>
  auto last() const -> const Kind* {
    const libchiaking::platform::Lock lock{mutex_};
    const Kind* found = nullptr;
    for (const auto& event : events_) {
      if (const auto* kind = std::get_if<Kind>(&event)) found = kind;
    }
    return found;
  }

  auto reserved(const manual::GuestId& guest) const
      -> const libchiaking::kakusan::PlayerSummary* {
    const libchiaking::platform::Lock lock{mutex_};
    for (const auto& event : events_) {
      const auto* changed = std::get_if<SharedSession::PlayerChanged>(&event);
      if (changed != nullptr && changed->player.guest == guest &&
          changed->player.reserved_seat)
        return &changed->player;
    }
    return nullptr;
  }

  template <class Kind, class Matches>
  auto any(Matches matches) const -> bool {
    const libchiaking::platform::Lock lock{mutex_};
    return std::ranges::any_of(events_, [&matches](const auto& event) {
      const auto* found = std::get_if<Kind>(&event);
      return found != nullptr && matches(*found);
    });
  }

  auto state() const -> const libchiaking::kakusan::PlayerSummary* {
    const libchiaking::platform::Lock lock{mutex_};
    const libchiaking::kakusan::PlayerSummary* found = nullptr;
    for (const auto& event : events_) {
      if (const auto* changed =
              std::get_if<SharedSession::PlayerChanged>(&event))
        found = &changed->player;
    }
    return found;
  }

  auto reached(manual::Progress wanted) const -> bool {
    const auto* last = state();
    return last != nullptr && last->progress == wanted;
  }

  SharedSession* share{};
  libchiaking::kakusan::GuestSeating seating{};

 private:
  mutable libchiaking::platform::Mutex mutex_{TestPlatform::threading()};
  std::deque<SharedSession::Event> events_;
};

class SharePlayer final : public manual::GuestEventSink,
                          public libchiaking::media::MediaSink {
 public:
  SharePlayer(Lan& lan, std::uint8_t tag)
      : random_(0x5000U + tag), machine_(lan, tag) {
    auto environment = manual::Environment::with(
        TestPlatform::crypto(), TestPlatform::secure_datagram(), machine_,
        random_, SharingValues::steady_clock());
    EXPECT_TRUE(environment);
    if (!environment) return;
    auto created =
        manual::Guest::create(TestPlatform::identity(tag),
                              std::move(environment).value(), *this, *this);
    EXPECT_TRUE(created);
    if (!created) return;
    guest.emplace(std::move(created).value());
  }

  void on_event(manual::GuestEvent event) override {
    const bool requested =
        std::holds_alternative<manual::guest_event::ControllerRequested>(event);
    events.push_back(std::move(event));
    if (requested) {
      EXPECT_TRUE(guest->controller_ready());
    }
  }

  void on_stream_description(
      const libchiaking::media::StreamDescription&) override {}
  auto on_video(const libchiaking::media::VideoUnit&)
      -> libchiaking::media::VideoResult override {
    return {};
  }
  void on_video_gap() override {}
  auto on_audio(const libchiaking::media::AudioUnit&)
      -> libchiaking::media::AudioResult override {
    return {};
  }
  void on_feedback(
      const libchiaking::controllers::ControllerFeedback&) override {}

  template <class Kind>
  auto find() const -> const Kind* {
    for (const auto& event : events) {
      if (const auto* found = std::get_if<Kind>(&event)) return found;
    }
    return nullptr;
  }

  template <class Kind>
  auto count() const -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(
        events,
        [](const auto& event) { return std::holds_alternative<Kind>(event); }));
  }

  auto rumbles_after(std::size_t seen) const
      -> std::vector<manual::guest_event::Rumble> {
    std::vector<manual::guest_event::Rumble> rumbles;
    for (const auto& event : events) {
      if (const auto* rumble = std::get_if<manual::guest_event::Rumble>(&event))
        rumbles.push_back(*rumble);
    }
    rumbles.erase(rumbles.begin(),
                  rumbles.begin() + static_cast<std::ptrdiff_t>(
                                        std::min(seen, rumbles.size())));
    return rumbles;
  }

  auto state() const -> const manual::guest_event::StateChanged* {
    const manual::guest_event::StateChanged* found = nullptr;
    for (const auto& event : events) {
      if (const auto* changed =
              std::get_if<manual::guest_event::StateChanged>(&event))
        found = changed;
    }
    return found;
  }

  auto reached(manual::Progress wanted) const -> bool {
    const auto* last = state();
    return last != nullptr && last->state == wanted;
  }

  std::deque<manual::GuestEvent> events;
  std::optional<manual::Guest> guest;

 private:
  test_support::SeededRandom random_;
  Machine machine_;
};

class ShareRig : public SharingValues {
 public:
  using SharedSession = libchiaking::kakusan::SharedSession;
  using Where = libchiaking::play::Where;

  explicit ShareRig(Where where = Where::home,
                    std::optional<libchiaking::catalog::ServiceGeneration>
                        service = std::nullopt,
                    std::vector<libchiaking::kakusan::SavedDevice> known = {})
      : store(TestPlatform::identity(0x10), std::move(known)),
        session(where, service) {}

  ~ShareRig() {
    if (share) share->stop_sharing();
  }

  ShareRig(const ShareRig&) = delete;
  auto operator=(const ShareRig&) -> ShareRig& = delete;

  static auto description(std::uint32_t fps = SourceStream::video_fps)
      -> libchiaking::media::StreamDescription {
    auto made = SourceStream::description();
    made.picture = libchiaking::media::GrantedPicture{
        libchiaking::media::VideoCodec::h264, 1280, 720, fps, 10000};
    return made;
  }

  void announce_keys(std::uint32_t version = SourceStream::takion_version) {
    namespace takion = libchiaking::net::protocol::takion;
    session.link.then([this,
                       version](libchiaking::play::testing::ScriptedLink&) {
      const auto keys = SourceStream::receive_keys();
      libchiaking::SecretArray<takion::source_key_size> key;
      libchiaking::SecretArray<takion::source_key_size> iv;
      std::ranges::copy(keys.key(), key.begin());
      std::ranges::copy(keys.iv(), iv.begin());
      session.core->share_rules().tap().on_source_keys(
          version, takion::SourceReceiveKeys{std::move(key), std::move(iv)});
      announced.fetch_add(1);
    });
    test_support::play::SessionRig::eventually(
        [this] { return announced.load() != 0; });
  }

  auto begin() -> libchiaking::Expected<void, Refusal> {
    auto begun =
        root.share(session.session->share(),
                   *libchiaking::kakusan::SharingRules::create(4, 1), recorder);
    if (!begun) return libchiaking::unexpected(begun.error());
    share = std::move(begun).value();
    recorder.share = share.get();
    return {};
  }

  void play() {
    session.start_playing();
    ASSERT_TRUE(begin());
  }

  void stream() {
    announce_keys();
    session.link.describe(description());
    ASSERT_TRUE(until(
        [this] { return recorder.count<SharedSession::StreamShared>() == 1; }));
  }

  template <class Done>
  auto until(Done done) -> ::testing::AssertionResult {
    const auto deadline =
        TestPlatform::steady().now_microseconds() +
        static_cast<std::uint64_t>(
            std::chrono::microseconds{std::chrono::seconds{20}}.count());
    while (!done()) {
      if (TestPlatform::steady().now_microseconds() >= deadline)
        return ::testing::AssertionFailure() << "not reached within 20 s";
      pump();
      nap(std::chrono::milliseconds{1});
    }
    return ::testing::AssertionSuccess();
  }

  void pump() {
    for (auto& player : players) {
      if (player->guest) {
        EXPECT_TRUE(player->guest->pump());
      }
    }
  }

  void join(std::uint8_t tag) {
    const auto invitations = recorder.count<SharedSession::InvitationReady>();
    const auto codes = recorder.count<SharedSession::CodeToReadOut>();
    const auto invited = share->invite_player();
    ASSERT_TRUE(invited);
    guest = invited.value();
    ASSERT_TRUE(until([&] {
      return recorder.count<SharedSession::InvitationReady>() ==
             invitations + 1;
    }));
    const auto* invitation = recorder.last<SharedSession::InvitationReady>();
    ASSERT_NE(invitation, nullptr);
    EXPECT_EQ(invitation->guest, guest);
    EXPECT_FALSE(invitation->invitation.text().empty());
    EXPECT_EQ(invitation->invitation.summary().host_hint,
              Identities::hint_of(root.this_device().key()));
    EXPECT_EQ(invitation->invitation.summary().seats.capacity, 4U);
    players.push_back(std::make_unique<SharePlayer>(lan, tag));
    auto& player = *players.back();
    ASSERT_TRUE(player.guest);
    ASSERT_TRUE(player.guest->accept_invitation(invitation->invitation.text()));
    ASSERT_TRUE(until([&] {
      return player.find<manual::guest_event::ReplyReady>() != nullptr;
    }));
    const auto imported = share->accept_reply(
        player.find<manual::guest_event::ReplyReady>()->block.view());
    ASSERT_TRUE(imported);
    EXPECT_EQ(imported.value(), guest);
    ASSERT_TRUE(until([&] {
      return recorder.count<SharedSession::CodeToReadOut>() == codes + 1 &&
             player.find<manual::guest_event::CodeReady>() != nullptr;
    }));
    const auto shown = manual::VerificationCode::parse(
        player.find<manual::guest_event::CodeReady>()->code.digits());
    ASSERT_TRUE(shown);
    EXPECT_TRUE(share->verify(guest, *shown));
    EXPECT_TRUE(player.guest->enter_code(
        recorder.last<SharedSession::CodeToReadOut>()->code.digits()));
  }

  void seated(std::uint8_t tag, SeatNumber console_seat = seat(1)) {
    ASSERT_NO_FATAL_FAILURE(join(tag));
    auto& player = newest();
    ASSERT_TRUE(until([this] { return session.link.count("seat_join") == 1; }));
    session.link.join(console_seat, {0, 0, 255});
    ASSERT_TRUE(until([&] {
      return recorder.reached(manual::Progress::playing) &&
             player.reached(manual::Progress::playing);
    }));
  }

  auto newest() -> SharePlayer& { return *players.back(); }

  Lan lan;
  test_support::SeededRandom random{0x1001};
  Machine machine{lan, 1};
  SharedLog log;
  MemoryStore store;
  libchiaking::kakusan::Kakusan root{
      libchiaking::testing::ServiceBuilder::on<libchiaking::kakusan::Kakusan>(
          Neighbourhood::surroundings(machine, random, steady_clock(), store,
                                      &log))};
  ShareRecorder recorder;
  test_support::play::SessionRig session;
  std::unique_ptr<SharedSession> share;
  manual::GuestId guest;
  std::atomic<int> announced{};
  std::vector<std::unique_ptr<SharePlayer>> players;
};

class SharedSessionTest : public ::testing::Test, protected SharingValues {};

}  // namespace kakusan_test
