#pragma once

#include "tests/support/enum_printing.hpp"
#include "tests/support/play/public_household.hpp"
#include "tests/support/process_threading.hpp"

#include <libchiaking/kakusan.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/testing/kakusan/in_memory_device_store.hpp>
#include <libchiaking/testing/kakusan/kakusan_pair.hpp>
#include <libchiaking/testing/kakusan/visit_turns.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <variant>
#include <vector>

namespace kakusan_test {

class PairTime {
 public:
  static auto threading() -> libchiaking::platform::DefaultThreadingProvider& {
    return test_support::process_threading();
  }

  static auto steady() -> const libchiaking::platform::SteadyClock& {
    static const libchiaking::platform::SteadyClock clock;
    return clock;
  }
};

template <class Event>
class PairEvents {
 public:
  void keep(Event event) {
    {
      const libchiaking::platform::Lock lock{mutex_};
      events_.push_back(std::move(event));
    }
    changed_.notify_all();
  }

  template <class Kind>
  auto count() const -> std::size_t {
    const libchiaking::platform::Lock lock{mutex_};
    return static_cast<std::size_t>(
        std::ranges::count_if(events_, [](const Event& event) {
          return std::holds_alternative<Kind>(event);
        }));
  }

  template <class Kind>
  auto last() const -> const Kind* {
    const libchiaking::platform::Lock lock{mutex_};
    return last_held<Kind>();
  }

  template <class Kind, class Matches>
  auto count_of(Matches matches) const -> std::size_t {
    const libchiaking::platform::Lock lock{mutex_};
    return static_cast<std::size_t>(
        std::ranges::count_if(events_, [&matches](const Event& event) {
          const auto* kind = std::get_if<Kind>(&event);
          return kind != nullptr && matches(*kind);
        }));
  }

  template <class Kind, class Matches>
  auto wait_for(Matches matches) const -> const Kind* {
    libchiaking::platform::Lock lock{mutex_};
    const bool arrived = changed_.wait_for(lock, patience, [&] {
      const auto* found = last_held<Kind>();
      return found != nullptr && matches(*found);
    });
    return arrived ? last_held<Kind>() : nullptr;
  }

  auto size() const -> std::size_t {
    const libchiaking::platform::Lock lock{mutex_};
    return events_.size();
  }

 private:
  static constexpr std::chrono::seconds patience{120};

  template <class Kind>
  auto last_held() const -> const Kind* {
    const Kind* found = nullptr;
    for (const auto& event : events_) {
      if (const auto* kind = std::get_if<Kind>(&event)) found = kind;
    }
    return found;
  }

  mutable libchiaking::platform::Mutex mutex_{PairTime::threading()};
  mutable libchiaking::platform::ConditionVariable changed_{
      PairTime::threading(), PairTime::steady()};
  std::deque<Event> events_;
};

class PairHost final : public libchiaking::kakusan::SharedSession::Observer {
 public:
  using SharedSession = libchiaking::kakusan::SharedSession;

  void on_event(SharedSession::Event event) override {
    seen.keep(std::move(event));
  }

  auto reached(libchiaking::kakusan::Progress wanted) const -> bool {
    const auto* changed = seen.last<SharedSession::PlayerChanged>();
    return changed != nullptr && changed->player.progress == wanted;
  }

  auto reservations() const -> std::size_t {
    return seen.count_of<SharedSession::PlayerChanged>(
        [](const SharedSession::PlayerChanged& changed) {
          return changed.player.reserved_seat.has_value();
        });
  }

  PairEvents<SharedSession::Event> seen;
};

class PairGuest final : public libchiaking::kakusan::GuestVisit::Observer {
 public:
  using GuestVisit = libchiaking::kakusan::GuestVisit;

  void on_event(GuestVisit::Event event) override {
    seen.keep(std::move(event));
  }

  auto reached(libchiaking::kakusan::Progress wanted) const -> bool {
    const auto* changed = seen.last<GuestVisit::VisitChanged>();
    return changed != nullptr && changed->progress == wanted;
  }

  PairEvents<GuestVisit::Event> seen;
  test_support::play::PublicMedia media;
};

class PairPicture final : public libchiaking::media::MediaSink {
 public:
  struct Unit {
    bool profile_header{};
    std::uint16_t frame_index{};
    std::vector<std::byte> bytes;
  };

  void on_stream_description(
      const libchiaking::media::StreamDescription&) override {}

  auto on_video(const libchiaking::media::VideoUnit& unit)
      -> libchiaking::media::VideoResult override {
    const auto bytes = unit.frame().bytes();
    {
      const libchiaking::platform::Lock lock{mutex_};
      units_.push_back({unit.frame().profile_header(),
                        unit.frame().frame_index(),
                        {bytes.begin(), bytes.end()}});
    }
    changed_.notify_all();
    return {};
  }

  void on_video_gap() override {}
  auto on_audio(const libchiaking::media::AudioUnit&)
      -> libchiaking::media::AudioResult override {
    return {};
  }
  void on_feedback(
      const libchiaking::controllers::ControllerFeedback&) override {}

  auto wait_for_frames(std::size_t wanted) const -> std::vector<Unit> {
    libchiaking::platform::Lock lock{mutex_};
    const bool arrived = changed_.wait_for(lock, patience, [&] {
      return static_cast<std::size_t>(
                 std::ranges::count_if(units_, [](const Unit& unit) {
                   return !unit.profile_header;
                 })) >= wanted;
    });
    return arrived ? units_ : std::vector<Unit>{};
  }

 private:
  static constexpr std::chrono::seconds patience{120};

  mutable libchiaking::platform::Mutex mutex_{PairTime::threading()};
  mutable libchiaking::platform::ConditionVariable changed_{
      PairTime::threading(), PairTime::steady()};
  std::vector<Unit> units_;
};

class PairStage {
 public:
  using SharedSession = libchiaking::kakusan::SharedSession;
  using GuestVisit = libchiaking::kakusan::GuestVisit;
  using Progress = libchiaking::kakusan::Progress;
  using KakusanPair = libchiaking::kakusan::testing::KakusanPair;
  using InMemoryDeviceStore =
      libchiaking::kakusan::testing::InMemoryDeviceStore;

  static constexpr std::size_t most_turns = 200'000;
  static constexpr std::uint64_t guest_account = 77;

  PairStage(InMemoryDeviceStore& host_store, InMemoryDeviceStore& guest_store)
      : host_store_(host_store), guest_store_(guest_store) {}

  ~PairStage() {
    if (session) {
      static_cast<void>(session->leave());
      session->wait_until_ended();
    }
    if (visit && turns && !turns->ended()) {
      visit->cancel();
      while (turns->turn()) pair->advance(std::chrono::milliseconds{100});
    }
    if (turns) static_cast<void>(turns->finish());
    visit.reset();
    share.reset();
  }

  PairStage(const PairStage&) = delete;
  auto operator=(const PairStage&) -> PairStage& = delete;

  static auto seat(std::size_t index) -> libchiaking::controllers::SeatNumber {
    return libchiaking::controllers::SeatNumber::create(index).value();
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

  template <class T>
  static auto refused(
      const libchiaking::Expected<T, libchiaking::kakusan::Refusal>& result,
      libchiaking::kakusan::Refusal::Kind kind) -> ::testing::AssertionResult {
    if (result) return ::testing::AssertionFailure() << "it was not refused";
    if (result.error().kind() != kind)
      return ::testing::AssertionFailure()
             << "it was refused as "
             << ::testing::PrintToString(result.error().kind());
    return ::testing::AssertionSuccess();
  }

  void begin() {
    auto made = KakusanPair::create(home.factory, {host_store_, guest_store_});
    ASSERT_TRUE(made);
    pair = std::move(made).value();
    link.step_by_hand();
    const auto started = home.play.start_scripted(
        home.home_user(), libchiaking::play::Where::home, link, {}, media,
        events);
    ASSERT_TRUE(started);
    session = started.value();
    link.connect();
    ASSERT_NO_FATAL_FAILURE(until([this] {
      return events.reached(libchiaking::play::SessionState::playing);
    }));
  }

  void turn() {
    static_cast<void>(link.turn());
    if (turns) static_cast<void>(turns->turn());
    pair->advance(std::chrono::milliseconds{1});
  }

  template <class Done>
  void until(Done done,
             std::source_location from = std::source_location::current()) {
    for (std::size_t taken = 0; !done(); ++taken) {
      ASSERT_LT(taken, most_turns) << "nothing happened in " << most_turns
                                   << " turns, waiting at line " << from.line();
      turn();
    }
  }

  void quiet(std::size_t turns_taken) {
    for (std::size_t taken = 0; taken < turns_taken; ++taken) turn();
  }

  void open() {
    auto begun = pair->host().share(
        session->share(), *libchiaking::kakusan::SharingRules::create(4, 1),
        host);
    ASSERT_TRUE(begun);
    share = std::move(begun).value();
  }

  void stream() {
    link.start_shared_stream();
    ASSERT_NO_FATAL_FAILURE(until([this] {
      return host.seen.count<SharedSession::StreamShared>() == 1;
    }));
  }

  void invite() {
    const auto invitations = host.seen.count<SharedSession::InvitationReady>();
    const auto invited = share->invite_player();
    ASSERT_TRUE(invited);
    guest_id = invited.value();
    ASSERT_NO_FATAL_FAILURE(until([&] {
      return host.seen.count<SharedSession::InvitationReady>() ==
             invitations + 1;
    }));
    const auto* ready = host.seen.last<SharedSession::InvitationReady>();
    ASSERT_NE(ready, nullptr);
    ASSERT_EQ(ready->guest, guest_id);
    invitation = KakusanPair::copied(ready->invitation);
  }

  void visit_with(const std::string& pasted) {
    auto read = pair->guest().read(pasted);
    ASSERT_TRUE(read);
    visit.reset();
    turns = std::make_unique<libchiaking::kakusan::testing::VisitTurns>(
        PairTime::threading(), PairTime::steady());
    guest = std::make_unique<PairGuest>();
    auto joined = pair->guest().join(std::move(read).value(),
                                     {*guest, guest->media, turns.get()});
    ASSERT_TRUE(joined);
    visit = std::move(joined).value();
    turns->run(*visit);
  }

  void answer() {
    ASSERT_NO_FATAL_FAILURE(until(
        [this] { return guest->seen.count<GuestVisit::ReplyReady>() == 1; }));
    const auto codes = host.seen.count<SharedSession::CodeToReadOut>();
    const auto accepted = share->accept_reply(
        KakusanPair::copied(guest->seen.last<GuestVisit::ReplyReady>()->reply));
    ASSERT_TRUE(accepted);
    ASSERT_EQ(accepted.value(), guest_id);
    ASSERT_NO_FATAL_FAILURE(until([&] {
      return host.seen.count<SharedSession::CodeToReadOut>() == codes + 1 &&
             guest->seen.count<GuestVisit::CodeToReadOut>() == 1;
    }));
  }

  void verify() {
    const auto& hosts = host.seen.last<SharedSession::CodeToReadOut>()->code;
    const auto& guests = guest->seen.last<GuestVisit::CodeToReadOut>()->code;
    ASSERT_EQ(host.seen.last<SharedSession::CodeToReadOut>()->guest, guest_id);
    ASSERT_TRUE(share->verify(guest_id, guests));
    ASSERT_TRUE(visit->verify(hosts));
    ASSERT_NO_FATAL_FAILURE(until([this] {
      return host.reached(Progress::waiting_for_seat) &&
             guest->reached(Progress::waiting_for_seat);
    }));
  }

  void sit(libchiaking::controllers::SeatNumber wanted) {
    const auto joins = link.count("seat_join");
    const auto keyframes = link.keyframe_requests();
    ASSERT_TRUE(share->give_seat(
        guest_id, {libchiaking::controllers::ControllerKind::dualsense,
                   libchiaking::AccountId{guest_account}, wanted}));
    ASSERT_NO_FATAL_FAILURE(
        until([&] { return link.count("seat_join") == joins + 1; }));
    link.join(wanted, {0, 0, 255});
    ASSERT_NO_FATAL_FAILURE(until([this] {
      return host.reached(Progress::playing) &&
             guest->reached(Progress::playing);
    }));
    ASSERT_NO_FATAL_FAILURE(
        until([&] { return link.keyframe_requests() > keyframes; }));
  }

  void verified() {
    ASSERT_NO_FATAL_FAILURE(open());
    ASSERT_NO_FATAL_FAILURE(stream());
    ASSERT_NO_FATAL_FAILURE(invite());
    ASSERT_NO_FATAL_FAILURE(visit_with(invitation));
    ASSERT_NO_FATAL_FAILURE(answer());
    ASSERT_NO_FATAL_FAILURE(verify());
  }

  void finish_visit() {
    ASSERT_NO_FATAL_FAILURE(until([this] { return turns->ended(); }));
    outcome = turns->finish();
    ASSERT_TRUE(outcome);
  }

  test_support::play::PublicHousehold home;
  std::unique_ptr<KakusanPair> pair;
  libchiaking::play::testing::ScriptedLink link{PairTime::threading(),
                                                PairTime::steady()};
  test_support::play::PublicMedia media;
  test_support::play::PublicEvents events;
  std::shared_ptr<libchiaking::play::Session> session;
  PairHost host;
  std::unique_ptr<SharedSession> share;
  libchiaking::kakusan::GuestId guest_id;
  std::string invitation;
  std::unique_ptr<PairGuest> guest;
  std::unique_ptr<libchiaking::kakusan::testing::VisitTurns> turns;
  std::unique_ptr<GuestVisit> visit;
  std::optional<libchiaking::kakusan::VisitOutcome> outcome;

 private:
  InMemoryDeviceStore& host_store_;
  InMemoryDeviceStore& guest_store_;
};

class PairTest : public ::testing::Test {
 protected:
  using SharedSession = libchiaking::kakusan::SharedSession;
  using GuestVisit = libchiaking::kakusan::GuestVisit;
  using Progress = libchiaking::kakusan::Progress;
  using Recognition = libchiaking::kakusan::Recognition;
  using Refusal = libchiaking::kakusan::Refusal;
  using EndReason = libchiaking::kakusan::EndReason;
  using VisitOutcome = libchiaking::kakusan::VisitOutcome;
  using SeatNumber = libchiaking::controllers::SeatNumber;
  using ScriptedLink = libchiaking::play::testing::ScriptedLink;
  using KakusanPair = libchiaking::kakusan::testing::KakusanPair;

  static auto seat(std::size_t index) -> SeatNumber {
    return PairStage::seat(index);
  }

  libchiaking::kakusan::testing::InMemoryDeviceStore host_store{
      PairTime::threading()};
  libchiaking::kakusan::testing::InMemoryDeviceStore guest_store{
      PairTime::threading()};
};

class PairStageTest : public PairTest {
 protected:
  void SetUp() override { ASSERT_NO_FATAL_FAILURE(stage.begin()); }

  PairStage stage{host_store, guest_store};
};

}  // namespace kakusan_test
