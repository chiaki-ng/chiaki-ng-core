#include "src/core/play/adapters/links/reconnecting_link.hpp"

#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/play/adapters/links/link_evidence.hpp"

#include <algorithm>
#include <functional>
#include <utility>
#include <variant>

namespace libchiaking::play::reconnection {

auto Progress::stage() const noexcept -> Stage {
  using boost::sml::state;
  if (machine_.is(state<Attempting>)) return Stage::attempting;
  if (machine_.is(state<Retiring>)) return Stage::retiring;
  if (machine_.is(state<Resting>)) return Stage::resting;
  if (machine_.is(state<FinalAttempt>)) return Stage::final_attempt;
  if (machine_.is(state<Parking>)) return Stage::parking;
  if (machine_.is(state<Parked>)) return Stage::parked;
  if (machine_.is(state<Halted>)) return Stage::halted;
  if (machine_.is(state<Unopened>)) return Stage::unopened;
  return Stage::unstarted;
}

auto Progress::idle_state() const noexcept
    -> net::protocol::remote_play::StreamState {
  using net::protocol::remote_play::StreamState;
  switch (stage()) {
    case Stage::retiring:
    case Stage::resting:
      return StreamState::establishing_route;
    case Stage::parking:
    case Stage::parked:
      return context_.parked;
    case Stage::halted:
      return StreamState::stopped;
    case Stage::unopened:
      return StreamState::failed;
    case Stage::unstarted:
    case Stage::attempting:
    case Stage::final_attempt:
      break;
  }
  return StreamState::idle;
}

}  // namespace libchiaking::play::reconnection

namespace libchiaking::play {

ReconnectingLink::ReconnectingLink(Where where, Opener opener,
                                   platform::StopSource stop,
                                   const platform::ClockProvider& clock,
                                   platform::ThreadingProvider& threading,
                                   Watcher& watcher, ConsoleHandover handover,
                                   bool reconnect)
    : Link(Venue::of(where, std::nullopt)),
      opener_(std::move(opener)),
      stop_(std::move(stop)),
      clock_(clock),
      watcher_(watcher),
      policy_(where, handover, reconnect),
      mutex_(threading),
      woken_(threading, clock) {}

ReconnectingLink::~ReconnectingLink() = default;

auto ReconnectingLink::failure_of(const Link& link) -> StartPolicy::Failure {
  StartPolicy::Failure failure;
  if (const auto& detail = link.failure()) {
    failure.cause = detail->end_cause();
    failure.left = detail->cancelled();
  }
  if (const auto quit = LinkEvidence::quit(link.quit()))
    failure.quit = quit->reason();
  return failure;
}

void ReconnectingLink::set_event_sink(LinkEventSink& sink) noexcept {
  sink_ = &sink;
}

auto ReconnectingLink::passing_events_through() const noexcept -> bool {
  const auto stage = progress_.stage();
  return stage != reconnection::Stage::retiring &&
         stage != reconnection::Stage::parking;
}

void ReconnectingLink::on_event(net::protocol::remote_play::StreamEvent event) {
  if (!passing_events_through() || sink_ == nullptr) return;
  if (progress_.stage() == reconnection::Stage::attempting) {
    if (const auto* changed =
            std::get_if<net::protocol::remote_play::event::StateChanged>(
                &event)) {
      if (changed->state ==
          net::protocol::remote_play::StreamState::streaming) {
        progress_.process(reconnection::Streamed{clock_.now_microseconds()});
      } else if (changed->state ==
                 net::protocol::remote_play::StreamState::failed) {
        held_.push_back(std::move(event));
        return;
      }
    } else if (std::holds_alternative<
                   net::protocol::remote_play::event::Failed>(event)) {
      held_.push_back(std::move(event));
      return;
    }
  }
  sink_->on_event(std::move(event));
}

void ReconnectingLink::release() {
  auto held = std::exchange(held_, {});
  for (auto& event : held) sink_->on_event(std::move(event));
}

void ReconnectingLink::mirror() {
  if (!link_) return;
  ending().record_quit(link_->quit().reason, link_->quit().text);
  if (const auto& failed = link_->failure(); failed && !failure())
    ending().record_failure(*failed);
  if (const auto& cleanup = link_->cleanup_failure();
      cleanup && !cleanup_failure())
    ending().record_cleanup_failure(*cleanup);
}

void ReconnectingLink::conclude() {
  progress_.process(reconnection::GiveUp{});
  release();
  mirror();
}

void ReconnectingLink::retire() {
  progress_.process(reconnection::TryAgain{});
  static_cast<void>(link_->stop());
  std::unique_ptr<Link> retired;
  {
    const platform::Lock lock(mutex_);
    retired = std::move(link_);
  }
  retired.reset();
  progress_.process(reconnection::AttemptRetired{});
}

auto ReconnectingLink::attempt_failed(const Result& result) const noexcept
    -> bool {
  const auto state = link_->state();
  bool failed =
      state == net::protocol::remote_play::StreamState::failed ||
      (!result && state == net::protocol::remote_play::StreamState::idle);
  if (state == net::protocol::remote_play::StreamState::stopped) {
    const auto reason = link_->quit().reason;
    failed = reason != net::protocol::remote_play::QuitReason::none &&
             net::protocol::remote_play::quit_reason_is_error(reason);
  }
  return failed && !stop_.stop_requested() && !watcher_.leaving();
}

auto ReconnectingLink::settled(Result result) -> Result {
  if (progress_.stage() != reconnection::Stage::attempting) {
    mirror();
    return result;
  }
  if (!attempt_failed(result)) {
    release();
    mirror();
    return result;
  }
  auto failure = failure_of(*link_);
  const auto now = clock_.now_microseconds();
  const auto& detail = link_->failure();
  progress_.process(
      reconnection::AttemptFailed{now, detail && detail->offline()});
  if (const auto since = progress_.offline_since_us(); since && now >= *since)
    failure.offline_for = std::chrono::microseconds{now - *since};
  if (const auto streamed = progress_.attempt_streamed_us();
      streamed && now >= *streamed)
    failure.played_for = std::chrono::microseconds{now - *streamed};
  if (const auto trouble = link_->diagnostics().network_trouble_us;
      trouble && now >= *trouble)
    failure.since_trouble = std::chrono::microseconds{now - *trouble};
  const auto next = policy_.after(failure);
  const bool waking = next == StartPolicy::Next::wake_then_try_again;
  if (next == StartPolicy::Next::give_up ||
      (waking && !watcher_.wake_console())) {
    conclude();
    return result;
  }
  held_.clear();
  retire();
  rest_over_ =
      timing::Deadline::after(clock_, StartPolicy::rest_between_attempts);
  watcher_.trying_again(policy_.attempt(), waking, progress_.played(), failure,
                        StartPolicy::rest_between_attempts);
  return {};
}

auto ReconnectingLink::open_next() -> Result {
  auto next = opener_();
  if (!next) {
    ending().record_failure(LinkFailure{
        net::protocol::remote_play::FailureStage::resolve, next.error()});
    progress_.process(reconnection::OpenRefused{});
    sink_->on_event(net::protocol::remote_play::event::StateChanged{
        progress_.idle_state()});
    return unexpected(next.error());
  }
  {
    const platform::Lock lock(mutex_);
    link_ = std::move(next.value());
  }
  progress_.process(reconnection::AttemptOpened{});
  link_->set_event_sink(*this);
  return settled(link_->start());
}

auto ReconnectingLink::halted() -> Result {
  if (!progress_.process(reconnection::Halt{})) return {};
  auto stopped = ending().stopped_event();
  if (sink_ != nullptr) sink_->on_event(std::move(stopped));
  return {};
}

auto ReconnectingLink::parked(Result stopped) -> Result {
  release();
  mirror();
  progress_.process(reconnection::Park{link_->state()});
  last_diagnostics_ = link_->diagnostics();
  last_version_ = link_->takion_version();
  std::unique_ptr<Link> retired;
  {
    const platform::Lock lock(mutex_);
    retired = std::move(link_);
  }
  retired.reset();
  progress_.process(reconnection::AttemptRetired{});
  return stopped;
}

auto ReconnectingLink::start() -> Result {
  if (link_ || progress_.stage() != reconnection::Stage::unstarted)
    return unexpected(net::protocol::remote_play::StreamError::invalid_state);
  if (sink_ == nullptr)
    return unexpected(
        net::protocol::remote_play::StreamError::invalid_configuration);
  return open_next();
}

auto ReconnectingLink::pump_once() -> Result {
  if (link_) return settled(link_->pump_once());
  if (!between_attempts())
    return unexpected(net::protocol::remote_play::StreamError::invalid_state);
  if (stop_.stop_requested()) return halted();
  if (!rest_over_.expired(clock_)) return {};
  return open_next();
}

auto ReconnectingLink::wait(std::chrono::milliseconds timeout) -> Result {
  if (link_) return link_->wait(timeout);
  if (!between_attempts() || timeout.count() <= 0) return {};
  const auto remaining = std::chrono::floor<std::chrono::milliseconds>(
      rest_over_.remaining(timing::Deadline::now(clock_))
          .value_or(timing::Deadline::Duration{}));
  platform::Lock lock(mutex_);
  static_cast<void>(woken_.wait_for(
      lock,
      std::min(timeout, std::max(remaining, std::chrono::milliseconds{1})),
      [this] { return wake_pending_; }));
  wake_pending_ = false;
  return {};
}

void ReconnectingLink::wake() noexcept {
  {
    const platform::Lock lock(mutex_);
    wake_pending_ = true;
    if (link_) link_->wake();
  }
  woken_.notify_all();
}

auto ReconnectingLink::stop() -> Result {
  if (!link_) return halted();
  return parked(link_->stop());
}

auto ReconnectingLink::stop_for_sleep() -> Result {
  if (!link_) return halted();
  return parked(link_->stop_for_sleep());
}

auto ReconnectingLink::state() const noexcept
    -> net::protocol::remote_play::StreamState {
  return link_ ? link_->state() : progress_.idle_state();
}

auto ReconnectingLink::forwarded(const std::function<Result(LinkInputs&)>& call)
    -> Result {
  return link_ ? call(*link_)
               : Result{unexpected(
                     net::protocol::remote_play::StreamError::invalid_state)};
}

auto ReconnectingLink::select_datacenter(std::string_view name) -> Result {
  if (!link_)
    return unexpected(net::protocol::remote_play::StreamError::invalid_state);
  return link_->select_datacenter(name);
}

auto ReconnectingLink::send_controller_state(
    const net::protocol::remote_play::ControllerState& state) -> Result {
  return forwarded(
      [&](LinkInputs& link) { return link.send_controller_state(state); });
}

auto ReconnectingLink::submit_pin(std::span<const std::byte> pin) -> Result {
  return forwarded([&](LinkInputs& link) { return link.submit_pin(pin); });
}

auto ReconnectingLink::report_video_loss(std::uint16_t first,
                                         std::uint16_t last) -> Result {
  if (!link_ && between_attempts()) return {};
  return forwarded(
      [&](LinkInputs& link) { return link.report_video_loss(first, last); });
}

auto ReconnectingLink::execute(
    const net::protocol::remote_play::StreamCommand& command) -> Result {
  return forwarded([&](LinkInputs& link) { return link.execute(command); });
}

auto ReconnectingLink::send_create(CloudMessaging::CapturePress press)
    -> Result {
  return forwarded(
      [press](LinkInputs& link) { return link.send_create(press); });
}

auto ReconnectingLink::send_deep_link(std::string text) -> Result {
  return forwarded([&](LinkInputs& link) { return link.send_deep_link(text); });
}

auto ReconnectingLink::send_invitation(std::string invitation_id,
                                       std::string session_id) -> Result {
  return forwarded([&](LinkInputs& link) {
    return link.send_invitation(invitation_id, session_id);
  });
}

auto ReconnectingLink::send_microphone(std::span<const std::byte> frame)
    -> Result {
  return forwarded(
      [&](LinkInputs& link) { return link.send_microphone(frame); });
}

auto ReconnectingLink::open_saved_data() -> Result {
  return forwarded([](LinkInputs& link) { return link.open_saved_data(); });
}

auto ReconnectingLink::toggle_system_menu(std::uint32_t pad) -> Result {
  return forwarded(
      [pad](LinkInputs& link) { return link.toggle_system_menu(pad); });
}

auto ReconnectingLink::connect_microphone() -> Result {
  return forwarded([](LinkInputs& link) { return link.connect_microphone(); });
}

auto ReconnectingLink::set_microphone_muted(bool muted) -> Result {
  return forwarded(
      [&](LinkInputs& link) { return link.set_microphone_muted(muted); });
}

auto ReconnectingLink::drop_video_frame() -> Result {
  return forwarded([](LinkInputs& link) { return link.drop_video_frame(); });
}

auto ReconnectingLink::request_idr() -> Result {
  return forwarded([](LinkInputs& link) { return link.request_idr(); });
}

auto ReconnectingLink::send_audio_state(
    net::protocol::remote_play::AudioStateKind kind,
    std::span<const std::byte> body) -> Result {
  return forwarded(
      [&](LinkInputs& link) { return link.send_audio_state(kind, body); });
}

auto ReconnectingLink::send_keyboard(const KeyboardState& state) -> Result {
  return forwarded([&](LinkInputs& link) { return link.send_keyboard(state); });
}

auto ReconnectingLink::diagnostics() const noexcept
    -> net::protocol::remote_play::StreamDiagnostics {
  return link_ ? link_->diagnostics() : last_diagnostics_;
}

auto ReconnectingLink::takion_version() const noexcept
    -> std::optional<std::uint32_t> {
  return link_ ? link_->takion_version() : last_version_;
}

}  // namespace libchiaking::play
