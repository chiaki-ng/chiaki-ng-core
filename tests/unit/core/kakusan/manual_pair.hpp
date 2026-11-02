#pragma once

#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/detail/kakusan/kakusan.hpp"
#include "libchiaking/platform/net/default.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/kakusan/adapters/connections.hpp"
#include "src/core/kakusan/adapters/controller_map.hpp"
#include "src/core/kakusan/adapters/guest_visit/guest_visit_core.hpp"
#include "src/core/kakusan/adapters/pump_gate.hpp"
#include "src/core/kakusan/adapters/shared_session/shared_session_core.hpp"
#include "src/core/kakusan/kakusan_core.hpp"
#include "src/core/kakusan/model/pump_wait.hpp"
#include "tests/unit/core/kakusan/identities.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manual {

using libchiaking::SecretString;
using libchiaking::kakusan::all_buttons;
using libchiaking::kakusan::Button;
using libchiaking::kakusan::Clock;
using libchiaking::kakusan::DeviceIdentity;
using libchiaking::kakusan::Fingerprint;
using libchiaking::kakusan::GuestId;
using libchiaking::kakusan::IdentityKey;
using libchiaking::kakusan::InvitationSummary;
using libchiaking::kakusan::Progress;
using libchiaking::kakusan::required_buttons;
using libchiaking::kakusan::SharingRules;
using libchiaking::kakusan::VerificationCode;
using libchiaking::net::kakusan::Error;
using libchiaking::net::kakusan::error_code;
using libchiaking::net::kakusan::error_name;
using libchiaking::net::kakusan::Result;
using Reason = libchiaking::net::kakusan::control::Reason;
using Seat = libchiaking::kakusan::SeatGrant;
using Seats = libchiaking::kakusan::SeatSummary;
using MediaTuning = libchiaking::kakusan::Tuning;
using HostCore = libchiaking::kakusan::SharedSession::Core;
using GuestCore = libchiaking::kakusan::GuestVisit::Core;

inline constexpr std::size_t max_block_size =
    libchiaking::kakusan::Invitation::most_text;

class Environment {
 public:
  using ClockSource = libchiaking::kakusan::Surroundings::ClockSource;

  struct Injected {
    libchiaking::platform::CryptoProvider& crypto;
    libchiaking::platform::SecureRandomProvider& random;
    libchiaking::net::kakusan::SpokeTransportFactory& transports;
    ClockSource clock;
  };

  static constexpr std::size_t max_stun_servers =
      libchiaking::kakusan::Kakusan::most_stun_servers;
  static constexpr std::size_t max_stun_server_size =
      libchiaking::kakusan::Kakusan::longest_stun_server;

  [[nodiscard]] static auto with(
      libchiaking::platform::CryptoProvider& crypto,
      libchiaking::platform::SecureDatagramProvider& secure_datagram,
      libchiaking::platform::NetworkProvider& network,
      libchiaking::platform::SecureRandomProvider& random, ClockSource clock,
      std::span<const std::string> stun_servers = {}) -> Result<Environment> {
    const auto usable = [](const std::string& server) {
      return !server.empty() && server.size() <= max_stun_server_size;
    };
    if (!clock || stun_servers.size() > max_stun_servers ||
        !std::ranges::all_of(stun_servers, usable))
      return libchiaking::net::kakusan::fail(Error::invalid_argument);
    Environment environment{crypto, random, std::move(clock)};
    environment.connections_ =
        std::make_unique<libchiaking::kakusan::Connections>(
            crypto, secure_datagram, network, random, stun_servers);
    return environment;
  }

  [[nodiscard]] static auto system(
      libchiaking::platform::CryptoProvider& crypto,
      libchiaking::platform::SecureDatagramProvider& secure_datagram,
      std::span<const std::string> stun_servers = {}) -> Result<Environment> {
    auto clocks = std::make_unique<Clocks>();
    auto network =
        std::make_unique<libchiaking::platform::DefaultNetworkProvider>(
            clocks->threading, clocks->steady);
    auto random = std::make_unique<libchiaking::platform::SystemSecureRandom>();
    auto environment = with(
        crypto, secure_datagram, *network, *random,
        [read = clocks.get()] {
          return Clock{read->steady.now_microseconds(),
                       read->wall.now_microseconds() / 1'000'000};
        },
        stun_servers);
    if (!environment) return environment;
    environment.value().network_ = std::move(network);
    environment.value().network_clocks_ = std::move(clocks);
    environment.value().own_random_ = std::move(random);
    return environment;
  }

  [[nodiscard]] static auto with(Injected injected) -> Result<Environment> {
    if (!injected.clock)
      return libchiaking::net::kakusan::fail(Error::invalid_argument);
    Environment environment{injected.crypto, injected.random,
                            std::move(injected.clock)};
    environment.connections_ =
        std::make_unique<libchiaking::kakusan::Connections>(
            injected.transports);
    return environment;
  }

  auto now() const -> Clock { return clock_(); }
  auto crypto() const noexcept -> libchiaking::platform::CryptoProvider& {
    return *crypto_;
  }
  auto random() const noexcept -> libchiaking::platform::SecureRandomProvider& {
    return *random_;
  }
  auto transports() const noexcept
      -> libchiaking::net::kakusan::SpokeTransportFactory& {
    return connections_->factory();
  }
  auto clock() const noexcept -> const ClockSource& { return clock_; }
  auto threading() const noexcept -> libchiaking::platform::ThreadingProvider& {
    return own_->threading;
  }
  auto steady() const noexcept -> const libchiaking::platform::ClockProvider& {
    return own_->steady;
  }
  auto wall() const noexcept -> const libchiaking::platform::ClockProvider& {
    return own_->wall;
  }

 private:
  struct Clocks {
    libchiaking::platform::DefaultThreadingProvider threading;
    libchiaking::platform::SteadyClock steady;
    libchiaking::platform::SystemClock wall;
  };

  Environment(libchiaking::platform::CryptoProvider& crypto,
              libchiaking::platform::SecureRandomProvider& random,
              ClockSource clock)
      : crypto_(&crypto),
        random_(&random),
        clock_(std::move(clock)),
        own_(std::make_unique<Clocks>()) {}

  libchiaking::platform::CryptoProvider* crypto_;
  libchiaking::platform::SecureRandomProvider* random_;
  ClockSource clock_;
  std::unique_ptr<Clocks> own_;
  std::unique_ptr<Clocks> network_clocks_;
  std::unique_ptr<libchiaking::platform::DefaultNetworkProvider> network_;
  std::unique_ptr<libchiaking::platform::SystemSecureRandom> own_random_;
  std::unique_ptr<libchiaking::kakusan::Connections> connections_;
};

namespace host_event {

struct InvitationReady {
  GuestId guest{};
  SecretString block;
  std::uint64_t expires_at{};
};
struct ReplyBound {
  GuestId guest{};
  IdentityKey identity{};
  Fingerprint fingerprint{};
};
struct CodeReady {
  GuestId guest{};
  VerificationCode code;
};
struct StateChanged {
  GuestId guest{};
  Progress state{Progress::waiting};
  std::optional<Error> error;
  std::optional<Reason> reason;
  bool by_peer{};
  std::optional<Seat> seat;
};
struct JoinRequested {
  GuestId guest{};
  Seat seat;
};
struct RemovalRequested {
  GuestId guest{};
  Seat seat;
  std::optional<libchiaking::controllers::SeatNumber> console_seat;
};
struct ControllerInput {
  GuestId guest{};
  libchiaking::controllers::ControllerState state;
};
struct IdrRequested {};
struct SeatsChanged {
  Seats seats;
};
struct Diagnostic {
  GuestId guest{};
  std::string text;
};

}  // namespace host_event

using HostEvent =
    std::variant<host_event::InvitationReady, host_event::ReplyBound,
                 host_event::CodeReady, host_event::StateChanged,
                 host_event::JoinRequested, host_event::RemovalRequested,
                 host_event::ControllerInput, host_event::IdrRequested,
                 host_event::SeatsChanged, host_event::Diagnostic>;

class HostEventSink {
 public:
  virtual ~HostEventSink() = default;
  virtual void on_event(HostEvent event) = 0;
};

class Host {
 private:
  struct Parts {
    Parts(Environment surroundings, std::unique_ptr<HostCore> engine,
          HostEventSink& sink)
        : environment(std::move(surroundings)),
          core(std::move(engine)),
          events(sink),
          gate(environment.threading()),
          mutex(environment.threading()) {}

    Environment environment;
    std::unique_ptr<HostCore> core;
    HostEventSink& events;
    libchiaking::kakusan::PumpGate gate;
    libchiaking::platform::Mutex mutex;
    std::atomic<bool> wanted{};
    std::atomic<bool> pending{};
    std::deque<HostEvent> queued;
    std::vector<std::pair<GuestId, Progress>> guests;
  };

  explicit Host(std::unique_ptr<Parts> parts) noexcept
      : parts_(std::move(parts)) {}

  template <class Call>
  auto call(Call&& invoked) const {
    const libchiaking::platform::Lock lock{parts_->mutex};
    const auto clock = parts_->environment.now();
    if constexpr (std::is_void_v<
                      std::invoke_result_t<Call&, HostCore&, const Clock&>>) {
      invoked(*parts_->core, clock);
      drain();
    } else {
      auto result = invoked(*parts_->core, clock);
      drain();
      return result;
    }
  }

  static auto seat_of(
      const std::optional<libchiaking::net::kakusan::SeatBinding>& seat)
      -> std::optional<Seat> {
    if (!seat) return std::nullopt;
    return Seat{seat->seat, seat->generation};
  }

  void drain() const {
    static constexpr auto forwarding = [](Progress state) {
      return state == Progress::waiting_for_seat ||
             state == Progress::getting_ready || state == Progress::playing;
    };
    auto& parts = *parts_;
    while (auto output = parts.core->take_output()) {
      if (auto* ready = std::get_if<HostCore::InvitationReady>(&*output)) {
        parts.queued.emplace_back(host_event::InvitationReady{
            GuestId{ready->guest}, std::move(ready->block), ready->expires_at});
      } else if (const auto* bound =
                     std::get_if<HostCore::GuestIdentified>(&*output)) {
        parts.queued.emplace_back(host_event::ReplyBound{
            GuestId{bound->guest}, IdentityKey{bound->identity},
            Fingerprint{bound->fingerprint}});
      } else if (const auto* code =
                     std::get_if<HostCore::CodeReady>(&*output)) {
        parts.queued.emplace_back(host_event::CodeReady{
            GuestId{code->guest},
            VerificationCode{std::span<const char, VerificationCode::size>{
                code->code.data(), VerificationCode::size}}});
      } else if (const auto* state =
                     std::get_if<HostCore::GuestState>(&*output)) {
        const GuestId guest{state->guest};
        const auto found = std::ranges::find(
            parts.guests, guest, &std::pair<GuestId, Progress>::first);
        if (state->state == Progress::gone) {
          if (found != parts.guests.end()) parts.guests.erase(found);
        } else if (found == parts.guests.end()) {
          parts.guests.emplace_back(guest, state->state);
        } else {
          found->second = state->state;
        }
        parts.queued.emplace_back(host_event::StateChanged{
            guest, state->state, state->error, state->reason, state->by_peer,
            seat_of(state->seat)});
      } else if (const auto* join =
                     std::get_if<HostCore::JoinRequested>(&*output)) {
        parts.queued.emplace_back(host_event::JoinRequested{
            GuestId{join->guest},
            Seat{join->seat.seat, join->seat.generation}});
      } else if (const auto* removal =
                     std::get_if<HostCore::RemovalRequested>(&*output)) {
        parts.queued.emplace_back(host_event::RemovalRequested{
            GuestId{removal->guest},
            Seat{removal->seat.seat, removal->seat.generation},
            removal->console_seat});
      } else if (const auto* input =
                     std::get_if<HostCore::ControllerInput>(&*output)) {
        parts.queued.emplace_back(
            host_event::ControllerInput{GuestId{input->guest}, input->state});
      } else if (std::holds_alternative<HostCore::IdrRequested>(*output)) {
        parts.queued.emplace_back(host_event::IdrRequested{});
      } else if (const auto* changed =
                     std::get_if<HostCore::SeatsChanged>(&*output)) {
        parts.queued.emplace_back(host_event::SeatsChanged{
            Seats{changed->seats.capacity, changed->seats.local_seats,
                  changed->seats.remote_seats, changed->seats.reserved_seats}});
      } else if (auto* diagnostic =
                     std::get_if<HostCore::Diagnostic>(&*output)) {
        parts.queued.emplace_back(host_event::Diagnostic{
            GuestId{diagnostic->guest}, std::move(diagnostic->text)});
      }
    }
    parts.wanted.store(
        std::ranges::any_of(parts.guests, forwarding,
                            &std::pair<GuestId, Progress>::second),
        std::memory_order_release);
  }

  std::unique_ptr<Parts> parts_;

 public:
  static constexpr std::size_t max_video_profiles =
      libchiaking::net::kakusan::control::max_video_profiles;
  static constexpr std::size_t max_profile_header_size =
      libchiaking::net::kakusan::control::max_profile_header_size;

  [[nodiscard]] static auto create(const SharingRules& rules,
                                   const DeviceIdentity& identity,
                                   Environment environment,
                                   HostEventSink& events) -> Result<Host> {
    auto core = HostCore::create(
        HostCore::Dependencies{environment.crypto(), environment.random(),
                               environment.transports(),
                               environment.threading()},
        identity.clone(), rules);
    if (!core) return libchiaking::net::kakusan::fail(core.error());
    return Host{std::make_unique<Parts>(std::move(environment),
                                        std::move(core).value(), events)};
  }

  void source_keys(std::uint32_t protocol_version,
                   libchiaking::play::ReceiveKeys keys) noexcept {
    parts_->core->source_keys(protocol_version, std::move(keys));
  }

  void source_datagram(
      const libchiaking::play::SharedDatagram& datagram) noexcept {
    parts_->core->source_datagram(datagram);
    if (parts_->wanted.load(std::memory_order_acquire))
      parts_->pending.store(true, std::memory_order_release);
  }

  [[nodiscard]] auto source_started(
      const libchiaking::media::StreamDescription& description,
      std::uint32_t video_fps) -> Result<void> {
    return call([&](HostCore& core, const Clock& clock) -> Result<void> {
      const auto oversized =
          [](const libchiaking::media::VideoProfile& profile) {
            return profile.header.size() > max_profile_header_size;
          };
      if (description.profiles.size() > max_video_profiles ||
          std::ranges::any_of(description.profiles, oversized))
        return libchiaking::net::kakusan::fail(Error::source_format);
      return core.source_started(description, video_fps, clock.now_us);
    });
  }

  void source_feedback(
      const libchiaking::controllers::ControllerFeedback& feedback) {
    const auto rumble =
        libchiaking::kakusan::ControllerMap::rumble_from_feedback(feedback);
    if (!rumble) return;
    call([&](HostCore& core, const Clock& clock) {
      core.source_rumble(*rumble, clock.now_us);
    });
  }

  void source_ended() {
    call([](HostCore& core, const Clock& clock) {
      core.source_ended(clock.now_us);
    });
  }

  [[nodiscard]] auto source_joined(
      Seat seat, libchiaking::controllers::SeatNumber console_seat)
      -> Result<void> {
    if (seat.number == 0 || seat.generation == 0)
      return libchiaking::net::kakusan::fail(Error::invalid_argument);
    return call([&](HostCore& core, const Clock& clock) {
      return core.source_joined({seat.number, seat.generation}, console_seat,
                                clock.now_us);
    });
  }

  [[nodiscard]] auto source_join_failed(Seat seat) -> Result<void> {
    if (seat.number == 0 || seat.generation == 0)
      return libchiaking::net::kakusan::fail(Error::invalid_argument);
    return call([&](HostCore& core, const Clock& clock) {
      return core.source_join_failed({seat.number, seat.generation},
                                     clock.now_us);
    });
  }

  void source_unseated(libchiaking::controllers::SeatNumber console_seat) {
    call([&](HostCore& core, const Clock& clock) {
      core.source_unseated(console_seat, clock.now_us);
    });
  }

  [[nodiscard]] auto invite() -> Result<GuestId> {
    return call([](HostCore& core, const Clock& clock) -> Result<GuestId> {
      const auto invited = core.invite(clock);
      if (!invited) return libchiaking::net::kakusan::fail(invited.error());
      return GuestId{invited.value()};
    });
  }

  [[nodiscard]] auto import_reply(std::string_view block) -> Result<GuestId> {
    if (block.empty())
      return libchiaking::net::kakusan::fail(Error::invalid_argument);
    if (block.size() > max_block_size)
      return libchiaking::net::kakusan::fail(Error::invalid_block);
    return call([&](HostCore& core, const Clock& clock) -> Result<GuestId> {
      const auto imported = core.import_reply(block, clock);
      if (!imported) return libchiaking::net::kakusan::fail(imported.error());
      return GuestId{imported.value()};
    });
  }

  [[nodiscard]] auto enter_code(const GuestId& guest, std::string_view code)
      -> Result<void> {
    if (code.empty())
      return libchiaking::net::kakusan::fail(Error::invalid_argument);
    return call([&](HostCore& core, const Clock& clock) {
      return core.enter_code(guest.bytes(), code, clock.now_us);
    });
  }

  [[nodiscard]] auto approve(const GuestId& guest) -> Result<void> {
    return call([&](HostCore& core, const Clock& clock) {
      return core.approve(guest.bytes(), clock.now_us);
    });
  }

  [[nodiscard]] auto remove(const GuestId& guest) -> Result<void> {
    return call([&](HostCore& core, const Clock& clock) {
      return core.remove(guest.bytes(), clock.now_us);
    });
  }

  [[nodiscard]] auto set_local_seats(std::uint8_t local_seats) -> Result<void> {
    if (local_seats == 0)
      return libchiaking::net::kakusan::fail(Error::invalid_argument);
    return call([&](HostCore& core, const Clock& clock) {
      return core.set_local_seats(local_seats, clock.now_us);
    });
  }

  [[nodiscard]] auto set_capacity(std::uint8_t capacity) -> Result<void> {
    if (capacity == 0)
      return libchiaking::net::kakusan::fail(Error::invalid_argument);
    return call([&](HostCore& core, const Clock& clock) {
      return core.set_capacity(capacity, clock.now_us);
    });
  }

  void set_media_tuning(const MediaTuning& tuning) {
    HostCore::Tuning applied;
    const auto to_us =
        [](std::chrono::milliseconds duration) noexcept -> std::uint64_t {
      return duration.count() <= 0
                 ? 0
                 : static_cast<std::uint64_t>(duration.count()) * 1000U;
    };
    applied.media.queue_age_min_us = to_us(tuning.queue_age_min);
    applied.media.queue_age_limit_us = to_us(tuning.queue_age_max);
    applied.media.queue_age_round_trips = tuning.queue_age_round_trips;
    applied.media.startup_grace_us = to_us(tuning.startup_grace);
    applied.media.drop_windows_to_fail = tuning.drop_seconds_to_end;
    applied.media.progress_timeout_us = to_us(tuning.acknowledgement_timeout);
    applied.media.queue_bytes = tuning.queue_bytes;
    applied.media.queue_packets = tuning.queue_packets;
    applied.keyframe_interval_us = to_us(tuning.keyframe_interval);
    call([&](HostCore& core, const Clock&) { core.set_tuning(applied); });
  }

  auto seats() const -> Seats {
    return call([](HostCore& core, const Clock&) {
      const auto seats = core.seats();
      return Seats{seats.capacity, seats.local_seats, seats.remote_seats,
                   seats.reserved_seats};
    });
  }

  [[nodiscard]] auto pump() -> Result<void> {
    if (parts_->gate.held_by_this_thread())
      return libchiaking::net::kakusan::fail(Error::invalid_argument);
    const libchiaking::kakusan::PumpGate::Turn turn{parts_->gate};
    std::deque<HostEvent> ready;
    {
      const libchiaking::platform::Lock lock{parts_->mutex};
      parts_->pending.store(false, std::memory_order_release);
      parts_->core->pump(parts_->environment.now());
      drain();
      ready.swap(parts_->queued);
    }
    for (auto& event : ready) parts_->events.on_event(std::move(event));
    return {};
  }

  auto next_wait(std::chrono::milliseconds limit) const
      -> std::chrono::milliseconds {
    const libchiaking::platform::Lock lock{parts_->mutex};
    return libchiaking::kakusan::PumpWait::remaining_wait(
        !parts_->queued.empty() ||
            parts_->pending.load(std::memory_order_acquire),
        parts_->core->next_deadline_us(), parts_->environment.now().now_us,
        limit);
  }
};

namespace guest_event {

struct ReplyReady {
  SecretString block;
};
struct CodeReady {
  VerificationCode code;
};
struct StateChanged {
  Progress state{Progress::waiting};
  std::optional<Error> error;
  std::optional<Reason> reason;
  bool by_peer{};
  std::optional<Seat> seat;
};
struct StreamReady {
  std::uint32_t video_fps{};
};
struct ControllerRequested {
  Seat seat;
  std::uint8_t capacity{};
  std::uint8_t local_seats{};
  std::uint32_t allowed_buttons{};
};
struct Rumble {
  std::uint16_t low_frequency{};
  std::uint16_t high_frequency{};
};
struct StreamStopped {};
struct Diagnostic {
  std::string text;
};

}  // namespace guest_event

using GuestEvent =
    std::variant<guest_event::ReplyReady, guest_event::CodeReady,
                 guest_event::StateChanged, guest_event::StreamReady,
                 guest_event::ControllerRequested, guest_event::Rumble,
                 guest_event::StreamStopped, guest_event::Diagnostic>;

class GuestEventSink {
 public:
  virtual ~GuestEventSink() = default;
  virtual void on_event(GuestEvent event) = 0;
};

class Guest {
 private:
  class StreamClock final : public libchiaking::platform::ClockProvider {
   public:
    explicit StreamClock(const Environment& environment) noexcept
        : environment_(environment) {}

    std::uint64_t now_microseconds() const noexcept override {
      return environment_.now().now_us;
    }

   private:
    const Environment& environment_;
  };

  struct Parts {
    Parts(Environment surroundings, GuestEventSink& sink,
          libchiaking::platform::DiagnosticsProvider* log)
        : environment(std::move(surroundings)),
          events(sink),
          stream_clock(environment),
          platform(libchiaking::platform::PlatformProviders{
              .threading = &environment.threading(),
              .clock = &stream_clock,
              .crypto = &environment.crypto(),
              .diagnostics = log}),
          gate(environment.threading()),
          mutex(environment.threading()),
          stream_mutex(environment.threading()) {}

    Environment environment;
    GuestEventSink& events;
    StreamClock stream_clock;
    libchiaking::platform::DefaultPlatformFactory platform;
    std::unique_ptr<GuestCore> core;
    libchiaking::kakusan::PumpGate gate;
    libchiaking::platform::Mutex mutex;
    std::deque<GuestEvent> queued;
    libchiaking::platform::Mutex stream_mutex;
    std::shared_ptr<libchiaking::play::ForwardedStream> stream;
    std::optional<libchiaking::media::MediaFault> fault;
    libchiaking::media::VideoStatistics statistics{};
  };

  explicit Guest(std::unique_ptr<Parts> parts) noexcept
      : parts_(std::move(parts)) {}

  template <class Call>
  auto call(Call&& invoked) const {
    const libchiaking::platform::Lock lock{parts_->mutex};
    const auto clock = parts_->environment.now();
    if constexpr (std::is_void_v<
                      std::invoke_result_t<Call&, GuestCore&, const Clock&>>) {
      invoked(*parts_->core, clock);
      drain();
    } else {
      auto result = invoked(*parts_->core, clock);
      drain();
      return result;
    }
  }

  auto forwarded() const
      -> std::shared_ptr<libchiaking::play::ForwardedStream> {
    const libchiaking::platform::Lock lock{parts_->stream_mutex};
    return parts_->stream;
  }

  void drain() const {
    auto& parts = *parts_;
    while (auto output = parts.core->take_output()) {
      if (auto* ready = std::get_if<GuestCore::ReplyReady>(&*output)) {
        parts.queued.emplace_back(
            guest_event::ReplyReady{std::move(ready->block)});
      } else if (const auto* code =
                     std::get_if<GuestCore::CodeReady>(&*output)) {
        parts.queued.emplace_back(guest_event::CodeReady{
            VerificationCode{std::span<const char, VerificationCode::size>{
                code->code.data(), VerificationCode::size}}});
      } else if (const auto* state = std::get_if<GuestCore::State>(&*output)) {
        std::optional<Seat> seat;
        if (state->seat)
          seat = Seat{state->seat->seat, state->seat->generation};
        parts.queued.emplace_back(guest_event::StateChanged{
            state->state, state->error, state->reason, state->by_peer, seat});
      } else if (const auto* stream =
                     std::get_if<GuestCore::StreamReady>(&*output)) {
        parts.queued.emplace_back(guest_event::StreamReady{stream->video_fps});
      } else if (const auto* requested =
                     std::get_if<GuestCore::ControllerRequested>(&*output)) {
        parts.queued.emplace_back(guest_event::ControllerRequested{
            Seat{requested->seat.seat, requested->seat.generation},
            requested->capacity, requested->local_seats,
            requested->allowed_buttons});
      } else if (const auto* rumble =
                     std::get_if<GuestCore::Rumble>(&*output)) {
        parts.queued.emplace_back(
            guest_event::Rumble{rumble->low_frequency, rumble->high_frequency});
      } else if (std::holds_alternative<GuestCore::StreamStopped>(*output)) {
        parts.queued.emplace_back(guest_event::StreamStopped{});
      } else if (auto* diagnostic =
                     std::get_if<GuestCore::Diagnostic>(&*output)) {
        parts.queued.emplace_back(
            guest_event::Diagnostic{std::move(diagnostic->text)});
      }
    }
    const libchiaking::platform::Lock lock{parts.stream_mutex};
    if (parts.stream == parts.core->forwarded()) return;
    if (parts.stream) {
      if (!parts.fault) parts.fault = parts.stream->fault();
      parts.statistics = parts.stream->statistics();
    }
    parts.stream = parts.core->forwarded();
  }

  std::unique_ptr<Parts> parts_;

 public:
  [[nodiscard]] static auto create(
      const DeviceIdentity& identity, Environment environment,
      GuestEventSink& events, libchiaking::media::MediaSink& media,
      libchiaking::platform::DiagnosticsProvider* log = nullptr)
      -> Result<Guest> {
    auto parts = std::make_unique<Parts>(std::move(environment), events, log);
    auto core = GuestCore::create(
        GuestCore::Dependencies{parts->platform, parts->environment.random(),
                                parts->environment.transports(), media},
        identity.clone());
    if (!core) return libchiaking::net::kakusan::fail(core.error());
    parts->core = std::move(core).value();
    return Guest{std::move(parts)};
  }

  [[nodiscard]] static auto preview(
      libchiaking::platform::CryptoProvider& crypto, std::string_view block)
      -> Result<InvitationSummary> {
    if (block.empty())
      return libchiaking::net::kakusan::fail(Error::invalid_argument);
    if (block.size() > max_block_size)
      return libchiaking::net::kakusan::fail(Error::invalid_block);
    const libchiaking::platform::SystemClock wall;
    return GuestCore::preview(crypto, block,
                              wall.now_microseconds() / 1'000'000);
  }

  [[nodiscard]] auto accept_invitation(std::string_view block)
      -> Result<InvitationSummary> {
    if (block.empty())
      return libchiaking::net::kakusan::fail(Error::invalid_argument);
    if (block.size() > max_block_size)
      return libchiaking::net::kakusan::fail(Error::invalid_block);
    return call([&](GuestCore& core, const Clock& clock) {
      return core.accept_invitation(block, clock);
    });
  }

  [[nodiscard]] auto enter_code(std::string_view code) -> Result<void> {
    if (code.empty())
      return libchiaking::net::kakusan::fail(Error::invalid_argument);
    return call([&](GuestCore& core, const Clock& clock) {
      return core.enter_code(code, clock.now_us);
    });
  }

  [[nodiscard]] auto controller_ready() -> Result<void> {
    return call([](GuestCore& core, const Clock& clock) {
      return core.controller_ready(clock.now_us);
    });
  }

  [[nodiscard]] auto set_controller(
      const libchiaking::controllers::ControllerState& state) -> Result<void> {
    return call([&](GuestCore& core, const Clock& clock) {
      return core.set_controller(
          libchiaking::kakusan::ControllerMap::from_source_state(state),
          clock.now_us);
    });
  }

  void leave() {
    call([](GuestCore& core, const Clock& clock) { core.leave(clock.now_us); });
  }

  auto state() const -> Progress {
    return call([](GuestCore& core, const Clock&) { return core.state(); });
  }

  auto video_presented() noexcept -> libchiaking::media::PresentationLoss {
    if (const auto stream = forwarded()) return stream->video_presented();
    return {};
  }

  void video_rejected(std::uint16_t frame, int cause) {
    if (const auto stream = forwarded()) stream->video_rejected(frame, cause);
  }

  void video_output_failed(int cause) {
    if (const auto stream = forwarded()) stream->video_output_failed(cause);
  }

  [[nodiscard]] auto media_fault() const
      -> std::optional<libchiaking::media::MediaFault> {
    const libchiaking::platform::Lock lock{parts_->stream_mutex};
    if (parts_->fault || !parts_->stream) return parts_->fault;
    return parts_->stream->fault();
  }

  auto video_statistics() const -> libchiaking::media::VideoStatistics {
    const libchiaking::platform::Lock lock{parts_->stream_mutex};
    return parts_->stream ? parts_->stream->statistics() : parts_->statistics;
  }

  [[nodiscard]] auto pump() -> Result<void> {
    if (parts_->gate.held_by_this_thread())
      return libchiaking::net::kakusan::fail(Error::invalid_argument);
    const libchiaking::kakusan::PumpGate::Turn turn{parts_->gate};
    std::deque<GuestEvent> ready;
    {
      const libchiaking::platform::Lock lock{parts_->mutex};
      parts_->core->pump(parts_->environment.now());
      drain();
      ready.swap(parts_->queued);
    }
    for (auto& event : ready) parts_->events.on_event(std::move(event));
    return {};
  }

  auto next_wait(std::chrono::milliseconds limit) const
      -> std::chrono::milliseconds {
    const libchiaking::platform::Lock lock{parts_->mutex};
    return libchiaking::kakusan::PumpWait::remaining_wait(
        !parts_->queued.empty(), parts_->core->next_deadline_us(),
        parts_->environment.now().now_us, limit);
  }
};

}  // namespace manual
