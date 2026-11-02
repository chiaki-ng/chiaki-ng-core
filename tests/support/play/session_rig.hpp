#pragma once

#include "src/core/play/adapters/links/scripted_link_core.hpp"
#include "src/core/play/adapters/session/session_core.hpp"

#include <libchiaking/detail/play/session.hpp>
#include <libchiaking/detail/play/session_events.hpp>
#include <libchiaking/testing/play/scripted_link.hpp>

#include "tests/support/live_providers.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/support/setup.hpp"
#include "tests/support/thread_set.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace test_support::play {

class RecordedEvents final : public libchiaking::play::SessionEvents {
 public:
  void on_state(libchiaking::play::SessionState state) override {
    const libchiaking::platform::Lock lock(mutex_);
    states_.push_back(state);
    heard("state");
  }

  void on_ended(const libchiaking::play::Outcome& outcome) override {
    const libchiaking::platform::Lock lock(mutex_);
    ended_ = outcome;
    heard("ended");
  }

  void on_connect_progress(
      const libchiaking::play::ConnectProgress& progress) override {
    const libchiaking::platform::Lock lock(mutex_);
    progress_.push_back(progress);
    heard("connect_progress");
  }

  void on_launch_progress(
      const libchiaking::play::LaunchProgress& progress) override {
    const libchiaking::platform::Lock lock(mutex_);
    launch_progress_ = progress;
    launch_history_.push_back(progress);
    heard("launch_progress");
  }

  void on_datacenters_offered(
      const libchiaking::play::DatacenterOffer& offer) override {
    const libchiaking::platform::Lock lock(mutex_);
    offered_ = offer.entries().size();
    heard("datacenters_offered");
  }

  void on_datacenter_chosen(
      const libchiaking::play::DatacenterChoice&) override {
    const libchiaking::platform::Lock lock(mutex_);
    heard("datacenter_chosen");
  }

  void on_login_pin_needed(bool previous_was_wrong) override {
    const libchiaking::platform::Lock lock(mutex_);
    pin_was_wrong_ = previous_was_wrong;
    heard("login_pin_needed");
  }

  void on_picture_blocked(bool blocked) override {
    const libchiaking::platform::Lock lock(mutex_);
    picture_blocked_ = blocked;
    heard("picture_blocked");
  }

  void on_keyboard_opened(std::string_view text) override {
    const libchiaking::platform::Lock lock(mutex_);
    keyboard_text_ = text;
    heard("keyboard_opened");
  }

  void on_keyboard_text(std::string_view text) override {
    const libchiaking::platform::Lock lock(mutex_);
    keyboard_text_ = text;
    heard("keyboard_text");
  }

  void on_keyboard_closed() override {
    const libchiaking::platform::Lock lock(mutex_);
    heard("keyboard_closed");
  }

  void on_seat_passcode_needed(libchiaking::controllers::SeatNumber seat,
                               bool retry) override {
    const libchiaking::platform::Lock lock(mutex_);
    passcode_seat_ = seat;
    passcode_retry_ = retry;
    heard("seat_passcode_needed");
  }

  void on_seat_joined(libchiaking::controllers::SeatNumber seat,
                      libchiaking::controllers::SeatLight light) override {
    const libchiaking::platform::Lock lock(mutex_);
    joined_seat_ = seat;
    joined_light_ = light;
    heard("seat_joined");
  }

  void on_seat_left(libchiaking::controllers::SeatNumber seat) override {
    const libchiaking::platform::Lock lock(mutex_);
    left_seat_ = seat;
    heard("seat_left");
  }

  void on_seat_refused(const libchiaking::play::SeatRefusal& refusal) override {
    const libchiaking::platform::Lock lock(mutex_);
    refusals_.push_back(refusal);
    heard("seat_refused");
  }

  void until(std::string_view name, std::size_t times = 1) {
    libchiaking::platform::Lock lock(mutex_);
    const auto arrived = [this, name, times] {
      return count_locked(name) >= times;
    };
    if (!changed_.wait_for(lock, std::chrono::seconds{20}, arrived))
      ADD_FAILURE() << "no " << name << " event within 20 seconds";
  }

  void until_state(libchiaking::play::SessionState state) {
    libchiaking::platform::Lock lock(mutex_);
    const auto arrived = [this, state] {
      for (const auto seen : states_)
        if (seen == state) return true;
      return false;
    };
    if (!changed_.wait_for(lock, std::chrono::seconds{20}, arrived))
      ADD_FAILURE() << "the session did not reach the state within 20 seconds";
  }

  auto count(std::string_view name) const -> std::size_t {
    const libchiaking::platform::Lock lock(mutex_);
    return count_locked(name);
  }

  auto names() const -> std::vector<std::string> {
    const libchiaking::platform::Lock lock(mutex_);
    return names_;
  }

  auto states() const -> std::vector<libchiaking::play::SessionState> {
    const libchiaking::platform::Lock lock(mutex_);
    return states_;
  }

  auto threads() const -> ThreadSet {
    const libchiaking::platform::Lock lock(mutex_);
    return threads_;
  }

  auto progress() const -> std::vector<libchiaking::play::ConnectProgress> {
    const libchiaking::platform::Lock lock(mutex_);
    return progress_;
  }

  auto refusals() const -> std::vector<libchiaking::play::SeatRefusal> {
    const libchiaking::platform::Lock lock(mutex_);
    return refusals_;
  }

  auto ended() const -> std::optional<libchiaking::play::Outcome> {
    const libchiaking::platform::Lock lock(mutex_);
    return ended_;
  }

  auto launch_progress() const
      -> std::optional<libchiaking::play::LaunchProgress> {
    const libchiaking::platform::Lock lock(mutex_);
    return launch_progress_;
  }

  auto launch_history() const
      -> std::vector<libchiaking::play::LaunchProgress> {
    const libchiaking::platform::Lock lock(mutex_);
    return launch_history_;
  }

  auto offered() const -> std::optional<std::size_t> {
    const libchiaking::platform::Lock lock(mutex_);
    return offered_;
  }

  auto pin_was_wrong() const -> std::optional<bool> {
    const libchiaking::platform::Lock lock(mutex_);
    return pin_was_wrong_;
  }

  auto picture_blocked() const -> std::optional<bool> {
    const libchiaking::platform::Lock lock(mutex_);
    return picture_blocked_;
  }

  auto keyboard_text() const -> std::string {
    const libchiaking::platform::Lock lock(mutex_);
    return keyboard_text_;
  }

  auto passcode_seat() const
      -> std::optional<libchiaking::controllers::SeatNumber> {
    const libchiaking::platform::Lock lock(mutex_);
    return passcode_seat_;
  }

  auto passcode_retry() const -> bool {
    const libchiaking::platform::Lock lock(mutex_);
    return passcode_retry_;
  }

  auto joined_seat() const
      -> std::optional<libchiaking::controllers::SeatNumber> {
    const libchiaking::platform::Lock lock(mutex_);
    return joined_seat_;
  }

  auto joined_light() const -> libchiaking::controllers::SeatLight {
    const libchiaking::platform::Lock lock(mutex_);
    return joined_light_;
  }

  auto left_seat() const
      -> std::optional<libchiaking::controllers::SeatNumber> {
    const libchiaking::platform::Lock lock(mutex_);
    return left_seat_;
  }

 private:
  void heard(std::string name) {
    names_.push_back(std::move(name));
    threads_.insert(ThreadSet::Identity::current());
    changed_.notify_all();
  }

  auto count_locked(std::string_view name) const -> std::size_t {
    std::size_t total = 0;
    for (const auto& seen : names_) total += seen == name ? 1U : 0U;
    return total;
  }

  mutable libchiaking::platform::Mutex mutex_{LiveProviders::threading()};
  libchiaking::platform::ConditionVariable changed_{LiveProviders::threading(),
                                                    LiveProviders::steady()};
  std::vector<std::string> names_;
  ThreadSet threads_;
  std::vector<libchiaking::play::SessionState> states_;
  std::vector<libchiaking::play::ConnectProgress> progress_;
  std::vector<libchiaking::play::SeatRefusal> refusals_;
  std::optional<libchiaking::play::Outcome> ended_;
  std::optional<libchiaking::play::LaunchProgress> launch_progress_;
  std::vector<libchiaking::play::LaunchProgress> launch_history_;
  std::optional<std::size_t> offered_;
  std::optional<bool> pin_was_wrong_;
  std::optional<bool> picture_blocked_;
  std::string keyboard_text_;
  std::optional<libchiaking::controllers::SeatNumber> passcode_seat_;
  bool passcode_retry_{};
  std::optional<libchiaking::controllers::SeatNumber> joined_seat_;
  libchiaking::controllers::SeatLight joined_light_{};
  std::optional<libchiaking::controllers::SeatNumber> left_seat_;
};

class RecordedMedia final : public libchiaking::media::MediaSink {
 public:
  struct Shown {
    std::uint16_t frame_index{};
    std::vector<std::byte> bytes;
    std::vector<std::byte> header;
    std::size_t padded{};
    bool profile_changed{};
    bool hevc{};
    std::uint32_t width{};
    std::uint32_t height{};
  };

  void on_stream_description(
      const libchiaking::media::StreamDescription& description) override {
    const libchiaking::platform::Lock lock(mutex_);
    descriptions_.push_back(description);
  }

  auto on_video(const libchiaking::media::VideoUnit& unit)
      -> libchiaking::media::VideoResult override {
    const libchiaking::platform::Lock lock(mutex_);
    const auto& frame = unit.frame();
    shown_.push_back(
        {frame.frame_index(),
         {frame.bytes().begin(), frame.bytes().end()},
         {unit.profile_header().begin(), unit.profile_header().end()},
         frame.padded_bytes().size(),
         unit.profile_changed(),
         unit.hevc(),
         unit.width(),
         unit.height()});
    threads_.insert(ThreadSet::Identity::current());
    return verdict_;
  }

  void on_video_gap() override {
    const libchiaking::platform::Lock lock(mutex_);
    ++gaps_;
  }

  auto on_audio(const libchiaking::media::AudioUnit& unit)
      -> libchiaking::media::AudioResult override {
    const libchiaking::platform::Lock lock(mutex_);
    sounds_.push_back(unit);
    return {};
  }

  void on_feedback(
      const libchiaking::controllers::ControllerFeedback& feedback) override {
    const libchiaking::platform::Lock lock(mutex_);
    feedback_.push_back(feedback);
    if (const auto* audio =
            feedback.get<libchiaking::controllers::HapticAudio>())
      haptic_audio_.assign(audio->pcm.begin(), audio->pcm.end());
  }

  void answer(libchiaking::media::VideoResult verdict) {
    const libchiaking::platform::Lock lock(mutex_);
    verdict_ = verdict;
  }

  auto descriptions() const
      -> std::vector<libchiaking::media::StreamDescription> {
    const libchiaking::platform::Lock lock(mutex_);
    return descriptions_;
  }

  auto shown() const -> std::vector<Shown> {
    const libchiaking::platform::Lock lock(mutex_);
    return shown_;
  }

  auto gaps() const -> std::size_t {
    const libchiaking::platform::Lock lock(mutex_);
    return gaps_;
  }

  auto sounds() const -> std::vector<libchiaking::media::AudioUnit> {
    const libchiaking::platform::Lock lock(mutex_);
    return sounds_;
  }

  auto feedback() const
      -> std::vector<libchiaking::controllers::ControllerFeedback> {
    const libchiaking::platform::Lock lock(mutex_);
    return feedback_;
  }

  auto haptic_audio() const -> std::vector<std::byte> {
    const libchiaking::platform::Lock lock(mutex_);
    return haptic_audio_;
  }

  auto threads() const -> ThreadSet {
    const libchiaking::platform::Lock lock(mutex_);
    return threads_;
  }

 private:
  mutable libchiaking::platform::Mutex mutex_{LiveProviders::threading()};
  libchiaking::media::VideoResult verdict_{};
  std::vector<std::byte> haptic_audio_;
  std::vector<libchiaking::media::StreamDescription> descriptions_;
  std::vector<Shown> shown_;
  std::size_t gaps_{};
  std::vector<libchiaking::media::AudioUnit> sounds_;
  std::vector<libchiaking::controllers::ControllerFeedback> feedback_;
  ThreadSet threads_;
};

class SessionRig {
 public:
  explicit SessionRig(
      libchiaking::play::Where where = libchiaking::play::Where::home,
      std::optional<libchiaking::catalog::ServiceGeneration> service =
          std::nullopt,
      std::shared_ptr<libchiaking::play::SessionShare::Core> share = nullptr) {
    libchiaking::play::Session::Core::Opening opening{
        .player = SignedInPlayer::fresh(),
        .console = nullptr,
        .where = where,
        .service = service,
        .link = link.core().link(
            stop, where,
            service == libchiaking::catalog::ServiceGeneration::old_service),
        .stop = stop,
        .media = media,
        .events = events,
        .clock = clock,
        .threading = LiveProviders::threading(),
        .journal = nullptr,
        .first_controller = libchiaking::controllers::ControllerKind::dualsense,
        .settings = {},
        .share = std::move(share)};
    opening.settings.commands.rest_flush = std::chrono::milliseconds{20};
    auto opened =
        Setup::value(libchiaking::play::Session::Core::open(std::move(opening)),
                     "the session opens");
    core = opened.get();
    session = std::make_unique<libchiaking::play::Session>(std::move(opened));
  }

  void begin() {
    Setup::require(core->begin().has_value(), "the session begins");
  }

  void start_playing() {
    link.connect();
    begin();
    events.until_state(libchiaking::play::SessionState::playing);
  }

  template <class Condition>
  static void eventually(Condition condition) {
    const auto deadline = LiveProviders::steady_us() + 20'000'000;
    while (!condition()) {
      if (LiveProviders::steady_us() >= deadline) {
        ADD_FAILURE() << "the condition did not hold within 20 seconds";
        return;
      }
      LiveProviders::nap(std::chrono::milliseconds{1});
    }
  }

  libchiaking::platform::StopSource stop;
  libchiaking::platform::SteadyClock clock;
  libchiaking::play::testing::ScriptedLink link{LiveProviders::threading(),
                                                LiveProviders::steady()};
  RecordedEvents events;
  RecordedMedia media;
  libchiaking::play::Session::Core* core{};
  std::unique_ptr<libchiaking::play::Session> session;
};

}  // namespace test_support::play
