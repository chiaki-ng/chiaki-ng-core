#include "src/core/kakusan/adapters/protocol_refusal.hpp"
#include "src/core/kakusan/usecases/join.hpp"

#include "src/core/kakusan/adapters/controller_map.hpp"
#include "src/core/kakusan/guest_visit_visiting.hpp"
#include "src/core/kakusan/model/pump_wait.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace libchiaking::kakusan::usecases {

using platform::LogSeverity;

Join::Join(GuestVisit::Visiting& visiting, Surroundings& surroundings,
           GuestVisit::Observer& observer, media::MediaSink& media,
           GuestVisit::Pause& pause, GuestVisit::Pacing pacing) noexcept
    : visiting_(visiting),
      surroundings_(surroundings),
      observer_(observer),
      media_(media),
      pause_(pause),
      pacing_(pacing) {}

auto Join::open(std::shared_ptr<Surroundings> surroundings,
                const Invitation& invitation, GuestVisit::Ports ports,
                GuestVisit::Pacing pacing)
    -> Expected<std::unique_ptr<GuestVisit>, Refusal> {
  const auto refused = [&surroundings](net::kakusan::Error error) {
    surroundings->log(LogSeverity::warning,
                      "Kakusan visit could not start: " +
                          std::string{net::kakusan::error_name(error)});
    return unexpected(ProtocolRefusal::of(error));
  };
  auto connections = surroundings->connections();
  auto machine = GuestVisit::Core::create(
      GuestVisit::Core::Dependencies{surroundings->stream_platform(),
                                     surroundings->random(),
                                     connections->factory(), ports.media},
      surroundings->identity().clone());
  if (!machine) return refused(machine.error());
  auto accepted = machine.value()->accept_invitation(invitation.text(),
                                                     surroundings->now());
  if (!accepted) return refused(accepted.error());
  auto host = std::move(accepted).value();
  host.recognition = surroundings->remembered().recognise(host.host_hint);
  return std::make_unique<GuestVisit>(std::make_unique<GuestVisit::Visiting>(
      surroundings, std::move(connections), std::move(machine).value(),
      std::move(host), ports, pacing));
}

void Join::log(LogSeverity severity, std::string_view message) const noexcept {
  surroundings_.log(severity, message);
}

void Join::cancel() noexcept {
  visiting_.cancel();
  pause_.wake();
}

auto Join::verify(const VerificationCode& theirs) -> Expected<void, Refusal> {
  const platform::Lock lock{visiting_.mutex()};
  if (!visiting_.open())
    return unexpected(Refusal{Refusal::Kind::not_visiting});
  const auto entered = visiting_.machine().enter_code(
      theirs.digits(), surroundings_.now().now_us);
  drain();
  pause_.wake();
  if (!entered) return unexpected(ProtocolRefusal::of(entered.error()));
  return {};
}

auto Join::send(const controllers::ControllerState& state)
    -> Expected<void, Refusal> {
  const platform::Lock lock{visiting_.mutex()};
  if (!visiting_.open())
    return unexpected(Refusal{Refusal::Kind::not_visiting});
  const auto sent = visiting_.machine().set_controller(
      ControllerMap::from_source_state(state), surroundings_.now().now_us);
  drain();
  pause_.wake();
  if (!sent) return unexpected(ProtocolRefusal::of(sent.error()));
  return {};
}

void Join::begin_leave() {
  const auto now = surroundings_.now().now_us;
  visiting_.machine().leave(now);
  drain();
  visiting_.leave_by(timing::Deadline::after_us(
      now, static_cast<std::uint64_t>(
               std::chrono::microseconds{pacing_.leave_deadline}.count())));
  pause_.wake();
}

auto Join::leave() -> Expected<void, Refusal> {
  const platform::Lock lock{visiting_.mutex()};
  if (visiting_.leaving()) return {};
  if (!visiting_.open())
    return unexpected(Refusal{Refusal::Kind::not_visiting});
  begin_leave();
  return {};
}

void Join::drain() {
  while (auto output = visiting_.machine().take_output())
    std::visit([this](auto& item) { translate(item); }, *output);
  visiting_.follow_stream();
}

void Join::translate(GuestVisit::Core::ReplyReady& ready) {
  visiting_.queue(GuestVisit::Event{GuestVisit::ReplyReady{
      GuestVisit::Visiting::reply(std::move(ready.block))}});
}

void Join::translate(const GuestVisit::Core::HostIdentified& host) {
  const IdentityKey key{host.key};
  visiting_.host_identified(key);
  visiting_.queue(GuestVisit::Event{
      GuestVisit::HostIdentified{key, Fingerprint{host.fingerprint}, host.seats,
                                 surroundings_.remembered().recognise(key)}});
}

void Join::translate(const GuestVisit::Core::CodeReady& code) {
  visiting_.queue(GuestVisit::Event{GuestVisit::CodeToReadOut{
      VerificationCode{std::span<const char, VerificationCode::size>{
          code.code.data(), VerificationCode::size}}}});
}

void Join::translate(const GuestVisit::Core::State& state) {
  GuestVisit::VisitChanged changed{state.state};
  if (state.error) changed.refusal = ProtocolRefusal::of(*state.error);
  if (state.reason) changed.ended = EndReason{*state.reason};
  changed.by_them = state.by_peer;
  if (state.seat)
    changed.grant = SeatGrant{state.seat->seat, state.seat->generation};
  const auto noted = visiting_.note(changed);
  visiting_.queue(GuestVisit::Event{changed});
  if (noted.connected)
    visiting_.queue(GuestVisit::Event{GuestVisit::Connected{}});
  if (noted.finished)
    visiting_.queue(GuestVisit::Event{GuestVisit::Disconnected{}});
}

void Join::translate(const GuestVisit::Core::StreamReady& stream) {
  visiting_.queue(GuestVisit::Event{GuestVisit::StreamReady{stream.video_fps}});
}

void Join::translate(const GuestVisit::Core::StreamStopped&) {
  visiting_.queue(GuestVisit::Event{GuestVisit::StreamStopped{}});
}

void Join::translate(const GuestVisit::Core::ControllerRequested&) {
  const auto readied =
      visiting_.machine().controller_ready(surroundings_.now().now_us);
  if (!readied)
    visiting_.queue(
        Note{"guest controller was not readied: " +
                 std::string{net::kakusan::error_name(readied.error())},
             LogSeverity::warning});
}

void Join::translate(const GuestVisit::Core::Rumble& rumble) {
  visiting_.queue(Shake{rumble.low_frequency, rumble.high_frequency});
}

void Join::translate(GuestVisit::Core::Diagnostic& diagnostic) {
  if (!diagnostic.text.empty() &&
      diagnostic.text.size() <= GuestVisit::diagnostic_limit)
    visiting_.queue(Note{std::move(diagnostic.text)});
}

void Join::pump() {
  visiting_.gate().take_turn(
      visiting_.mutex(),
      [this] {
        visiting_.machine().pump(surroundings_.now());
        drain();
        return visiting_.take_queued();
      },
      [this](Step& step) {
        std::visit([this](auto& taken) { deliver(std::move(taken)); }, step);
      });
}

void Join::deliver(GuestVisit::Event event) {
  if (const auto* changed = std::get_if<GuestVisit::VisitChanged>(&event)) {
    log(LogSeverity::info,
        "Kakusan visit progress " +
            std::to_string(static_cast<int>(changed->progress)) + " refusal " +
            std::string{changed->refusal ? changed->refusal->name() : "OK"} +
            " by them " + std::to_string(changed->by_them ? 1 : 0));
    if (changed->progress == Progress::waiting_for_seat) visiting_.host_seen();
  }
  observer_.on_event(std::move(event));
}

void Join::deliver(const Shake& shake) {
  media_.on_feedback(controllers::ControllerFeedback{
      controllers::SeatNumber::first(),
      controllers::Rumble{rumble_byte(shake.low_frequency),
                          rumble_byte(shake.high_frequency)}});
}

auto Join::rumble_byte(std::uint16_t value) noexcept -> std::uint8_t {
  return static_cast<std::uint8_t>(ControllerMap::scale_normalized(
      value, std::numeric_limits<std::uint8_t>::max()));
}

void Join::deliver(const Note& note) {
  log(note.severity, "Kakusan " + note.text);
}

auto Join::service(bool leave) -> Turn {
  {
    const platform::Lock lock{visiting_.mutex()};
    if (leave && visiting_.open()) begin_leave();
    if (!visiting_.open() && !visiting_.leaving()) return {};
  }
  pump();
  Turn next;
  const platform::Lock lock{visiting_.mutex()};
  auto cap = pacing_.idle_wait;
  const auto now = surroundings_.now().now_us;
  const bool leaving = visiting_.leaving();
  const bool expired = leaving && now >= visiting_.leave_deadline_us();
  next.finished = (!visiting_.open() && !leaving) || expired;
  if (visiting_.host_admitted()) cap = pacing_.admitted_wait;
  if (leaving && !expired) {
    cap = std::min(
        cap, PumpWait::ceil_milliseconds(visiting_.leave_deadline_us() - now));
  }
  if (!next.finished)
    next.wait = PumpWait::remaining_wait(visiting_.has_queued(),
                                         visiting_.machine().next_deadline_us(),
                                         now, cap);
  return next;
}

auto Join::run() -> VisitOutcome {
  using Status = VisitOutcome::Status;
  {
    std::unique_ptr<GuestVisit::Core> unused;
    const platform::Lock lock{visiting_.mutex()};
    if (visiting_.already_ran())
      return GuestVisit::Visiting::outcome(Status::cancelled, std::nullopt,
                                           std::nullopt);
    if (visiting_.cancelled()) {
      unused = visiting_.abandon();
      return GuestVisit::Visiting::outcome(Status::cancelled, std::nullopt,
                                           std::nullopt);
    }
  }
  for (;;) {
    const bool leave = visiting_.cancelled() || visiting_.fault().has_value();
    const auto turn = service(leave);
    if (turn.finished) break;
    if (turn.wait.count() > 0) pause_.wait(turn.wait);
  }
  GuestVisit::Visiting::Closed closed;
  {
    const platform::Lock gate{visiting_.gate().mutex()};
    const platform::Lock lock{visiting_.mutex()};
    closed = visiting_.close();
  }
  closed.machine.reset();
  auto failed = visiting_.fault();
  if (failed)
    return GuestVisit::Visiting::outcome(Status::media_failed, std::nullopt,
                                         std::move(failed));
  if (visiting_.cancelled())
    return GuestVisit::Visiting::outcome(Status::cancelled, std::nullopt,
                                         std::nullopt);
  if (closed.refusal)
    return GuestVisit::Visiting::outcome(Status::visit_failed, closed.refusal,
                                         std::nullopt);
  return GuestVisit::Visiting::outcome(Status::completed, std::nullopt,
                                       std::nullopt);
}

}  // namespace libchiaking::kakusan::usecases
