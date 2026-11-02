#include "src/core/kakusan/adapters/protocol_refusal.hpp"
#include "src/core/kakusan/usecases/host.hpp"

#include "src/core/kakusan/adapters/controller_map.hpp"
#include "src/core/kakusan/model/pump_wait.hpp"
#include "src/core/kakusan/shared_session_hosting.hpp"
#include "src/core/net/kakusan/result.hpp"

#include <algorithm>
#include <string>
#include <type_traits>
#include <utility>

namespace libchiaking::kakusan::usecases {

using platform::LogSeverity;

auto Host::milliseconds(std::chrono::milliseconds duration) -> std::string {
  return std::to_string(duration.count());
}

Host::Host(SharedSession::Hosting& hosting, Surroundings& surroundings,
           SharedSession::Core& machine, SharedSession::Observer& observer,
           SharedSession::Pacing pacing) noexcept
    : hosting_(hosting),
      surroundings_(surroundings),
      machine_(machine),
      observer_(observer),
      pacing_(pacing) {}

auto Host::open(std::shared_ptr<Surroundings> surroundings,
                play::SessionShare& session, const SharingRules& rules,
                SharedSession::Observer& observer, SharedSession::Pacing pacing)
    -> Expected<std::unique_ptr<SharedSession>, Refusal> {
  const auto refused = [&surroundings](Refusal refusal) {
    surroundings->log(LogSeverity::warning,
                      "Kakusan is unavailable for this session: " +
                          std::string{refusal.name()});
    return unexpected(refusal);
  };
  if (const auto shareable = session.can_be_shared(); !shareable)
    return refused(unshareable(shareable.error()));
  auto connections = surroundings->connections();
  auto machine = SharedSession::Core::create(
      SharedSession::Core::Dependencies{
          surroundings->crypto(), surroundings->random(),
          connections->factory(), surroundings->threading()},
      surroundings->identity().clone(), rules);
  if (!machine) return refused(ProtocolRefusal::of(machine.error()));
  auto hosting = std::make_unique<SharedSession::Hosting>(
      surroundings, std::move(connections), std::move(machine).value(),
      observer, pacing);
  {
    const platform::Lock lock{hosting->mutex()};
    hosting->tap(&hosting->machine());
    auto begun = session.begin(hosting->host());
    if (!begun) {
      hosting->never_began();
      return refused(unshareable(begun.error()));
    }
    hosting->began(std::move(begun).value(), session);
  }
  surroundings->log(LogSeverity::info, "Kakusan is open for this session");
  return std::make_unique<SharedSession>(std::move(hosting));
}

void Host::log(LogSeverity severity, std::string_view message) const noexcept {
  surroundings_.log(severity, message);
}

void Host::wake() const noexcept {
  if (auto* session = hosting_.session()) session->wake();
}

void Host::stop_sharing() {
  std::optional<SharedSession::Hosting::Closed> closed;
  {
    const platform::Lock lock{hosting_.mutex()};
    closed = hosting_.close();
    if (!closed) return;
  }
  closed->seated.clear();
  if (closed->sharing) closed->sharing->stop();
  hosting_.tap(nullptr);
  if (closed->streaming) {
    const platform::Lock lock{hosting_.mutex()};
    machine_.source_ended(surroundings_.now().now_us);
    drain();
  }
}

auto Host::invite_player() -> Expected<GuestId, Refusal> {
  const platform::Lock lock{hosting_.mutex()};
  if (!hosting_.live()) return unexpected(Refusal{Refusal::Kind::not_sharing});
  const auto invited = machine_.invite(surroundings_.now());
  drain();
  if (!invited && !hosting_.streaming() &&
      invited.error() == net::kakusan::Error::source_ended)
    return unexpected(Refusal{Refusal::Kind::not_sharing});
  wake();
  if (!invited) return unexpected(ProtocolRefusal::of(invited.error()));
  return GuestId{invited.value()};
}

auto Host::accept_reply(std::string_view pasted) -> Expected<GuestId, Refusal> {
  if (pasted.empty() || pasted.size() > Invitation::most_text)
    return unexpected(Refusal{Refusal::Kind::invalid_argument});
  const platform::Lock lock{hosting_.mutex()};
  if (!hosting_.live()) return unexpected(Refusal{Refusal::Kind::not_sharing});
  const auto imported = machine_.import_reply(pasted, surroundings_.now());
  drain();
  wake();
  if (!imported) return unexpected(ProtocolRefusal::of(imported.error()));
  return GuestId{imported.value()};
}

auto Host::verify(const GuestId& guest, const VerificationCode& theirs)
    -> Expected<void, Refusal> {
  const platform::Lock lock{hosting_.mutex()};
  if (!hosting_.live()) return unexpected(Refusal{Refusal::Kind::not_sharing});
  const auto entered = machine_.enter_code(guest.bytes(), theirs.digits(),
                                           surroundings_.now().now_us);
  drain();
  wake();
  if (!entered) return unexpected(ProtocolRefusal::of(entered.error()));
  return {};
}

auto Host::give_seat(const GuestId& guest, const GuestSeating& seating)
    -> Expected<void, Refusal> {
  const platform::Lock lock{hosting_.mutex()};
  if (!hosting_.live()) return unexpected(Refusal{Refusal::Kind::not_sharing});
  const auto approved =
      machine_.approve(guest.bytes(), surroundings_.now().now_us);
  if (approved) hosting_.seat_as(guest, seating);
  drain();
  wake();
  if (!approved) return unexpected(ProtocolRefusal::of(approved.error()));
  return {};
}

auto Host::take_back_seat(const GuestId& guest) -> Expected<void, Refusal> {
  const platform::Lock lock{hosting_.mutex()};
  if (!hosting_.live()) return unexpected(Refusal{Refusal::Kind::not_sharing});
  const auto removed =
      machine_.remove(guest.bytes(), surroundings_.now().now_us);
  drain();
  wake();
  if (!removed) return unexpected(ProtocolRefusal::of(removed.error()));
  return {};
}

auto Host::change(const SharingRules& rules) -> Expected<void, Refusal> {
  const platform::Lock lock{hosting_.mutex()};
  if (!hosting_.live()) return unexpected(Refusal{Refusal::Kind::not_sharing});
  if (rules.allowed_buttons() != machine_.allowed_buttons())
    return unexpected(Refusal{Refusal::Kind::invalid_argument});
  const auto now = surroundings_.now().now_us;
  const bool growing = rules.capacity() >= machine_.seats().capacity;
  auto changed = growing ? machine_.set_capacity(rules.capacity(), now)
                         : machine_.set_local_seats(rules.local_seats(), now);
  if (changed)
    changed = growing ? machine_.set_local_seats(rules.local_seats(), now)
                      : machine_.set_capacity(rules.capacity(), now);
  drain();
  wake();
  if (!changed) return unexpected(ProtocolRefusal::of(changed.error()));
  return {};
}

auto Host::tune(const Tuning& tuning) -> Expected<void, Refusal> {
  SharedSession::Core::Tuning applied;
  applied.media.queue_age_min_us = microseconds(tuning.queue_age_min);
  applied.media.queue_age_limit_us = microseconds(tuning.queue_age_max);
  applied.media.queue_age_round_trips = tuning.queue_age_round_trips;
  applied.media.startup_grace_us = microseconds(tuning.startup_grace);
  applied.media.drop_windows_to_fail = tuning.drop_seconds_to_end;
  applied.media.progress_timeout_us =
      microseconds(tuning.acknowledgement_timeout);
  applied.media.queue_bytes = tuning.queue_bytes;
  applied.media.queue_packets = tuning.queue_packets;
  applied.keyframe_interval_us = microseconds(tuning.keyframe_interval);
  {
    const platform::Lock lock{hosting_.mutex()};
    if (!hosting_.live())
      return unexpected(Refusal{Refusal::Kind::not_sharing});
    machine_.set_tuning(applied);
    drain();
    wake();
  }
  log(LogSeverity::info,
      "Kakusan media tuning: queue wait " + milliseconds(tuning.queue_age_min) +
          " to " + milliseconds(tuning.queue_age_max) + " ms at " +
          std::to_string(tuning.queue_age_round_trips) +
          " round trips, grace " + milliseconds(tuning.startup_grace) +
          " ms, end after " + std::to_string(tuning.drop_seconds_to_end) +
          " s of heavy shedding, acknowledgement timeout " +
          milliseconds(tuning.acknowledgement_timeout) + " ms, queue " +
          std::to_string(tuning.queue_bytes) + " B or " +
          std::to_string(tuning.queue_packets) +
          " packets, keyframes at most every " +
          milliseconds(tuning.keyframe_interval) + " ms");
  return {};
}

void Host::drain() {
  while (auto output = machine_.take_output())
    std::visit([this](auto& item) { translate(item); }, *output);
  hosting_.forward_datagrams(hosting_.guests_forwarding());
}

void Host::translate(SharedSession::Core::InvitationReady& ready) {
  hosting_.queue(SharedSession::Event{SharedSession::InvitationReady{
      GuestId{ready.guest}, hosting_.invitation(ready)}});
}

void Host::translate(const SharedSession::Core::GuestIdentified& bound) {
  const GuestId guest{bound.guest};
  const IdentityKey device{bound.identity};
  const Fingerprint fingerprint{bound.fingerprint};
  hosting_.identified(guest, device, fingerprint);
  hosting_.queue(SharedSession::Event{SharedSession::PlayerIdentified{
      guest, device, fingerprint,
      surroundings_.remembered().recognise(device)}});
}

void Host::translate(const SharedSession::Core::CodeReady& code) {
  hosting_.queue(SharedSession::Event{SharedSession::CodeToReadOut{
      GuestId{code.guest},
      VerificationCode{std::span<const char, VerificationCode::size>{
          code.code.data(), VerificationCode::size}}}});
}

void Host::translate(const SharedSession::Core::GuestState& state) {
  PlayerSummary player{GuestId{state.guest}, state.state};
  if (state.error) player.refusal = ProtocolRefusal::of(*state.error);
  if (state.reason) player.ended = EndReason{*state.reason};
  player.by_them = state.by_peer;
  if (state.seat)
    player.grant = SeatGrant{state.seat->seat, state.seat->generation};
  hosting_.queue(SharedSession::Event{
      SharedSession::PlayerChanged{hosting_.changed(std::move(player))}});
}

void Host::translate(const SharedSession::Core::JoinRequested& join) {
  hosting_.queue(Joining{GuestId{join.guest},
                         SeatGrant{join.seat.seat, join.seat.generation}});
}

void Host::translate(const SharedSession::Core::RemovalRequested& removal) {
  hosting_.queue(Removal{GuestId{removal.guest}});
}

void Host::translate(const SharedSession::Core::ControllerInput& input) {
  hosting_.queue(Input{GuestId{input.guest}, input.state});
}

void Host::translate(const SharedSession::Core::IdrRequested&) {
  hosting_.queue(Keyframe{});
}

void Host::translate(const SharedSession::Core::SeatsChanged& changed) {
  hosting_.queue(
      SharedSession::Event{SharedSession::SeatsChanged{changed.seats}});
}

void Host::translate(SharedSession::Core::Diagnostic& diagnostic) {
  hosting_.queue(Note{std::move(diagnostic.text)});
}

void Host::pump() {
  hosting_.gate().take_turn(
      hosting_.mutex(),
      [this] {
        hosting_.datagrams_pumped();
        machine_.pump(surroundings_.now());
        drain();
        return hosting_.take_queued();
      },
      [this](Step& step) {
        std::visit([this](auto& taken) { deliver(std::move(taken)); }, step);
      });
}

auto Host::service(std::chrono::milliseconds wait)
    -> std::chrono::milliseconds {
  {
    const platform::Lock lock{hosting_.mutex()};
    if (!hosting_.live()) return wait;
  }
  pump();
  const platform::Lock lock{hosting_.mutex()};
  const auto cap = hosting_.guests_admitted() && wait > pacing_.admitted_wait
                       ? pacing_.admitted_wait
                       : wait;
  return PumpWait::remaining_wait(
      hosting_.has_queued() || hosting_.datagrams_pending(),
      machine_.next_deadline_us(), surroundings_.now().now_us, cap);
}

void Host::session_ended() {
  std::vector<SharedSession::Hosting::Seated> seated;
  bool live = false;
  {
    const platform::Lock lock{hosting_.mutex()};
    live = hosting_.end();
    if (live && hosting_.streaming()) {
      machine_.source_ended(surroundings_.now().now_us);
      drain();
    }
    seated = hosting_.leave_session();
  }
  seated.clear();
  if (!live) return;
  pump();
  observer_.on_event(SharedSession::SharingEnded{});
}

void Host::stream_started(const play::SharedStream& stream,
                          play::ReceiveKeys keys) {
  const auto fps = stream.frames_per_second;
  const auto& description = stream.description;
  net::kakusan::Result<void> started{};
  {
    const platform::Lock lock{hosting_.mutex()};
    if (!hosting_.live()) return;
    machine_.source_keys(stream.protocol_version, std::move(keys));
    const auto oversized = [](const media::VideoProfile& profile) {
      return profile.header.size() >
             net::kakusan::control::max_profile_header_size;
    };
    if (description.profiles.size() >
            net::kakusan::control::max_video_profiles ||
        std::ranges::any_of(description.profiles, oversized))
      started = net::kakusan::fail(net::kakusan::Error::source_format);
    else
      started =
          machine_.source_started(description, fps, surroundings_.now().now_us);
    drain();
    if (started) hosting_.stream_shared();
  }
  if (started) {
    log(LogSeverity::info,
        "Kakusan is sharing the stream at " + std::to_string(fps) + " fps");
    observer_.on_event(SharedSession::StreamShared{fps});
    return;
  }
  const auto* first =
      description.profiles.empty() ? nullptr : &description.profiles.front();
  log(LogSeverity::warning,
      "Kakusan could not share the stream: " +
          std::string{net::kakusan::error_name(started.error())} + "; " +
          std::to_string(fps) + " fps, " +
          std::to_string(description.profiles.size()) + " profiles, first " +
          std::to_string(first != nullptr ? first->width : 0) + "x" +
          std::to_string(first != nullptr ? first->height : 0) + " with a " +
          std::to_string(first != nullptr ? first->header.size() : 0) +
          " byte header, granted codec " +
          std::to_string(description.picture
                             ? static_cast<int>(description.picture->codec)
                             : -1) +
          ", audio " + std::to_string(description.audio.channels) + " ch " +
          std::to_string(description.audio.bits) + " bit " +
          std::to_string(description.audio.rate) + " Hz frame " +
          std::to_string(description.audio.frame_size));
  observer_.on_event(SharedSession::SharingFailed{ProtocolRefusal::of(started.error())});
}

void Host::encrypted_datagram(const play::SharedDatagram& datagram) noexcept {
  if (auto* machine = hosting_.tapped()) {
    machine->source_datagram(datagram);
    hosting_.datagram_forwarded();
  }
}

void Host::feedback(const controllers::ControllerFeedback& feedback) {
  if (const auto* intensity = feedback.get<controllers::HapticIntensity>()) {
    const platform::Lock lock{hosting_.mutex()};
    haptic_intensity_ = intensity->level;
    return;
  }
  if (feedback.kind() != controllers::ControllerFeedback::Kind::rumble) return;
  const auto rumble = ControllerMap::rumble_from_feedback(feedback);
  if (!rumble) return;
  const platform::Lock lock{hosting_.mutex()};
  if (!hosting_.live() || !hosting_.streaming()) return;
  machine_.source_rumble(ControllerMap::scaled(*rumble, haptic_intensity_),
                         surroundings_.now().now_us);
  drain();
}

void Host::join_failed(const SeatGrant& grant) {
  const platform::Lock lock{hosting_.mutex()};
  if (!hosting_.live()) return;
  static_cast<void>(machine_.source_join_failed(
      net::kakusan::SeatBinding{grant.number, grant.generation},
      surroundings_.now().now_us));
  drain();
}

void Host::guest_seated(controllers::SeatNumber seat) {
  GuestId guest;
  net::kakusan::Result<void> joined{};
  {
    const platform::Lock lock{hosting_.mutex()};
    auto* found = hosting_.awaiting(seat);
    if (!hosting_.live() || found == nullptr) return;
    found->joined = true;
    guest = found->guest;
    joined = machine_.source_joined(
        net::kakusan::SeatBinding{found->grant.number, found->grant.generation},
        seat, surroundings_.now().now_us);
    drain();
  }
  if (!joined) {
    log(LogSeverity::warning,
        "Kakusan could not seat a guest: " +
            std::string{net::kakusan::error_name(joined.error())});
    std::optional<SharedSession::Hosting::Seated> gone;
    {
      const platform::Lock lock{hosting_.mutex()};
      gone = hosting_.take(guest);
    }
    return;
  }
  observer_.on_event(SharedSession::PlayerSeated{guest, seat});
}

void Host::guest_seat_refused(const play::SeatRefusal& refusal) {
  std::optional<SharedSession::Hosting::Seated> refused;
  {
    const platform::Lock lock{hosting_.mutex()};
    if (const auto seat = refusal.seat())
      refused = hosting_.take_awaiting(*seat);
    if (!refused) return;
  }
  join_failed(refused->grant);
}

void Host::guest_unseated(controllers::SeatNumber seat) {
  std::optional<SharedSession::Hosting::Seated> dropped;
  {
    const platform::Lock lock{hosting_.mutex()};
    dropped = hosting_.take(seat);
    if (!dropped) return;
    if (dropped->joined && hosting_.live()) {
      machine_.source_unseated(seat, surroundings_.now().now_us);
      drain();
    }
  }
  if (!dropped->joined) return join_failed(dropped->grant);
  observer_.on_event(SharedSession::PlayerUnseated{dropped->guest, seat});
}

void Host::became_unshareable(play::NotShareable) {
  log(LogSeverity::warning,
      "Kakusan cannot share this session: its protocol version is not "
      "forwarded");
  observer_.on_event(SharedSession::SharingFailed{
      Refusal{Refusal::Kind::unsupported_version}});
}

void Host::deliver(SharedSession::Event event) {
  std::optional<IdentityKey> verified;
  {
    const platform::Lock lock{hosting_.mutex()};
    if (hosting_.stopped()) return;
  }
  if (const auto* changed = std::get_if<SharedSession::PlayerChanged>(&event)) {
    const auto& player = changed->player;
    log(LogSeverity::info,
        "Kakusan guest progress " +
            std::to_string(static_cast<int>(player.progress)) + " refusal " +
            std::string{player.refusal ? player.refusal->name() : "OK"} +
            " by them " + std::to_string(player.by_them ? 1 : 0));
    if (player.progress == Progress::waiting_for_seat) verified = player.device;
  }
  if (verified) hosting_.seen(*verified);
  observer_.on_event(std::move(event));
}

void Host::deliver(const Joining& joining) {
  std::optional<PlayerSummary> reserved;
  {
    const platform::Lock lock{hosting_.mutex()};
    if (!hosting_.live()) return;
    if (auto* session = hosting_.session()) {
      const auto* known = hosting_.row(joining.guest);
      const auto seating = known != nullptr ? known->seating : GuestSeating{};
      auto seated =
          session->seat_guest({hosting_.next_guest_tag(), seating.controller,
                               seating.as, seating.preferred});
      if (seated) {
        hosting_.reserve(joining.guest, joining.grant,
                         std::move(seated).value());
        if (known == nullptr) return;
        reserved = hosting_.shown(known->player);
      }
    }
  }
  if (reserved)
    return observer_.on_event(
        SharedSession::PlayerChanged{std::move(*reserved)});
  log(LogSeverity::warning, "Kakusan found no console seat for a guest");
  join_failed(joining.grant);
}

void Host::deliver(const Removal& removal) {
  {
    const platform::Lock lock{hosting_.mutex()};
    if (hosting_.stopped()) return;
  }
  std::optional<SharedSession::Hosting::Seated> gone;
  {
    const platform::Lock lock{hosting_.mutex()};
    gone = hosting_.take(removal.guest);
  }
  if (gone && gone->joined)
    observer_.on_event(
        SharedSession::PlayerUnseated{gone->guest, gone->console.number()});
}

void Host::deliver(const Input& input) {
  const platform::Lock lock{hosting_.mutex()};
  auto* found = hosting_.seat_of(input.guest);
  if (found != nullptr && found->joined)
    static_cast<void>(found->console.send(input.state));
}

void Host::deliver(const Keyframe&) {
  const platform::Lock lock{hosting_.mutex()};
  if (auto* session = hosting_.session()) session->request_keyframe();
}

void Host::deliver(const Note& note) {
  if (!note.text.empty() && note.text.size() <= SharedSession::diagnostic_limit)
    log(LogSeverity::info, "Kakusan " + note.text);
}

}  // namespace libchiaking::kakusan::usecases
