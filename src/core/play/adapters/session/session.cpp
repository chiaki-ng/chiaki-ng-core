#include <libchiaking/platform/runtime/threading.hpp>
#include "src/core/play/adapters/session/session_core.hpp"

#include "src/core/play/adapters/links/cloud/cloud_messaging.hpp"
#include "src/core/play/adapters/links/link_evidence.hpp"
#include "src/core/play/adapters/links/link_failure.hpp"
#include "src/core/play/adapters/links/link_stages.hpp"
#include "src/core/play/adapters/links/session_diagnostics.hpp"

#include <bit>
#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <utility>
#include <variant>

namespace libchiaking::play {

static_assert(LinkStatistics::dscp_count ==
              net::protocol::remote_play::stream_dscp_count);
static_assert(LinkStatistics::ecn_count ==
              net::protocol::remote_play::stream_ecn_count);

using platform::LogSeverity;

auto Session::Core::core_settings() -> SessionSettings {
  SessionSettings settings;
  settings.controller_depth = Controllers::default_depth;
  settings.seat_depth = Seats::default_depth;
  return settings;
}

auto Session::Core::named_for(const media::MediaSink& media,
                              SessionSettings settings) -> SessionSettings {
  const std::string decoder{media.decoder_name()};
  settings.stages.format_setup = decoder + " format setup";
  settings.stages.submission = decoder + " submission";
  settings.stages.decoder_output = decoder + " output";
  return settings;
}

auto Session::Core::open(Opening opening)
    -> Expected<std::unique_ptr<Core>, EndReason::Cause> {
  if (!opening.player || !opening.link)
    return unexpected(EndReason::Cause::invalid_preferences);
  if ((opening.where == Where::cloud) != opening.service.has_value())
    return unexpected(EndReason::Cause::invalid_preferences);
  auto journal = opening.journal
                     ? std::move(opening.journal)
                     : std::make_shared<diagnostics::SessionJournal>(
                           opening.threading, opening.clock, 1);
  return std::unique_ptr<Core>{new Core{opening, std::move(journal)}};
}

Session::Core::Core(Opening& opening,
                    std::shared_ptr<diagnostics::SessionJournal> journal)
    : threading_(opening.threading),
      clock_(opening.clock),
      player_(std::move(opening.player)),
      console_(std::move(opening.console)),
      where_(opening.where),
      service_(opening.service),
      events_(opening.events),
      journal_(std::move(journal)),
      share_(opening.share
                 ? std::move(opening.share)
                 : SessionShare::Core::create(opening.threading, opening.clock,
                                              {where_, 0})),
      settings_(named_for(opening.media, opening.settings)),
      lifecycle_(opening.threading),
      driver_(std::move(opening.link), std::move(opening.stop),
              opening.threading, *this, *journal_, Venue::of(where_, service_)),
      route_(driver_, lifecycle_, *journal_, Venue::of(where_, service_)),
      controller_rules_(opening.threading, route_, settings_.controller_depth,
                        opening.controller_waking),
      seat_rules_(opening.threading, route_, controller_rules_,
                  opening.first_controller, settings_.seat_depth),
      commands_(opening.threading, route_, lifecycle_, opening.clock,
                seat_rules_, settings_.commands),
      media_(share_->watch(opening.media), opening.threading, opening.clock,
             *journal_,
             MediaSettings{settings_.video, settings_.stages,
                           settings_.audio_failure_ends_run}),
      seats_(seat_rules_),
      controllers_(controller_rules_),
      login_pin_(commands_.pin()),
      keyboard_(commands_.keyboard()),
      microphone_(commands_.microphone()),
      mutex_(opening.threading),
      changed_(opening.threading, opening.clock) {
  share_->bind(*this);
}

Session::Core::~Core() {
  cancel();
  if (thread_.joinable()) static_cast<void>(thread_.join());
  share_->close();
  journal_->retain();
}

auto Session::Core::attach(LinkReports& reports) -> bool {
  const platform::Lock lock(mutex_);
  if (begun_) return false;
  reports_ = &reports;
  return true;
}

void Session::Core::starting_again(std::uint32_t attempt, bool waking,
                                   bool reconnecting, AttemptFailure failure,
                                   std::chrono::milliseconds rest) {
  attempt_ = attempt;
  previous_failure_ = failure;
  if (reconnecting) link_lost();
  const auto first_step = where_ == Where::internet
                              ? ConnectStep::opening_account_session
                              : ConnectStep::finding_console;
  ConnectProgress progress{waking ? ConnectStep::waking_console : first_step,
                           {},
                           attempt,
                           reconnecting_};
  progress.wait =
      progress::Wait{waking ? progress::WaitReason::console_waking
                            : progress::WaitReason::connection_retry,
                     rest};
  progress.previous_failure = previous_failure_;
  events_.on_connect_progress(progress);
  publish();
}

void Session::Core::link_lost() {
  reconnecting_ = true;
  {
    const platform::Lock lock(mutex_);
    if (state_.load(std::memory_order_relaxed) == SessionState::playing) {
      state_.store(SessionState::connecting, std::memory_order_release);
      unpublished_.push_back(SessionState::connecting);
    }
  }
  publish();
  commands_.forget_seats();
  for (const auto seat : seat_rules_.link_lost()) {
    events_.on_seat_left(seat);
    share_->seat_left(seat);
  }
}

auto Session::Core::attach(CloudLaunch& launch) -> bool {
  const platform::Lock lock(mutex_);
  if (begun_ || where_ != Where::cloud) return false;
  launch_ = &launch;
  return true;
}

auto Session::Core::claim() -> bool { return !std::exchange(begun_, true); }

auto Session::Core::begin() -> Expected<void, NotBegun> {
  const platform::Lock lock(mutex_);
  if (!claim()) return unexpected(NotBegun::already_begun);
  auto started = platform::Thread::start(
      threading_, "session", platform::ThreadRole::session, [this] { run(); });
  if (!started) return unexpected(NotBegun::no_thread);
  thread_ = std::move(started.value());
  return {};
}

auto Session::Core::run_on_this_thread() -> bool {
  {
    const platform::Lock lock(mutex_);
    if (!claim()) return false;
  }
  run();
  return true;
}

auto Session::Core::state() const noexcept -> SessionState {
  return state_.load(std::memory_order_acquire);
}

auto Session::Core::on_core_thread() const noexcept -> bool {
  const platform::Lock lock(mutex_);
  return runner_ == platform::ThreadIdentity::current();
}

void Session::Core::advance(SessionState next) {
  {
    const platform::Lock lock(mutex_);
    if (next <= state_.load(std::memory_order_relaxed)) return;
    state_.store(next, std::memory_order_release);
    unpublished_.push_back(next);
  }
  seat_rules_.session_is(next);
}

void Session::Core::publish() {
  for (;;) {
    SessionState next;
    {
      const platform::Lock lock(mutex_);
      if (unpublished_.empty()) return;
      next = unpublished_.front();
      unpublished_.pop_front();
    }
    events_.on_state(next);
  }
}

void Session::Core::run() {
  {
    const platform::Lock lock(mutex_);
    runner_ = platform::ThreadIdentity::current();
  }
  publish();
  if (const auto version = driver_.takion_version())
    log(LogSeverity::info,
        "takion version: negotiated=" + std::to_string(*version));
  if (lifecycle_.cancelled() && !lifecycle_.sleeping()) driver_.cancel();
  LoopEnd end;
  if (const auto started = driver_.start(); started) {
    lifecycle_.begin();
    end = loop();
  } else {
    end = {Outcome::Phase::start, started.error()};
  }
  auto outcome = finish(end);
  advance(SessionState::ended);
  const Outcome* settled = nullptr;
  {
    const platform::Lock lock(mutex_);
    settled = &outcome_.emplace(std::move(outcome));
  }
  publish();
  events_.on_ended(*settled);
  {
    const platform::Lock lock(mutex_);
    delivered_ = true;
  }
  changed_.notify_all();
}

auto Session::Core::loop() -> LoopEnd {
  unsigned transients = 0;
  for (;;) {
    if (media_.fault()) return {Outcome::Phase::media, std::nullopt};
    if (lifecycle_.sleeping()) return {};
    if (const auto range = media_.video().take_loss_report()) {
      if (const auto reported =
              driver_.report_video_loss(range->first, range->last);
          !reported) {
        media_.recovery_failed(static_cast<int>(
            std::to_underlying(LinkEvidence::cause(reported.error()))));
        return {Outcome::Phase::media, std::nullopt};
      }
    }
    if (media_.video().drop_due()) {
      const auto dropped = driver_.drop_video_frame();
      log(dropped ? LogSeverity::info : LogSeverity::warning,
          "video frame drop requested: result=" +
              std::string{dropped
                              ? std::string_view{"ok"}
                              : CommandRoute::result_name(dropped.error())});
    }
    if (media_.video().idr_due()) {
      const auto requested = driver_.request_idr();
      const auto requests = media_.video().statistics().keyframe_requests;
      if (!requested || std::has_single_bit(requests))
        log(requested ? LogSeverity::info : LogSeverity::warning,
            "video keyframe request: requests=" + std::to_string(requests) +
                " result=" +
                std::string{
                    requested ? std::string_view{"ok"}
                              : CommandRoute::result_name(requested.error())});
    }
    const auto state = driver_.state();
    if (state == net::protocol::remote_play::StreamState::failed) {
      const auto& failure = driver_.failure();
      if (failure) return {Outcome::Phase::session, failure->result()};
      return {Outcome::Phase::session_unknown, std::nullopt};
    }
    if (!active(state)) return {};
    const bool streaming =
        state == net::protocol::remote_play::StreamState::streaming;
    if (streaming) static_cast<void>(controller_rules_.flush());
    auto service = commands_.service(state, settings_.idle_wait);
    for (const auto& refusal : service.refusals) {
      events_.on_seat_refused(refusal);
      share_->seat_refused(refusal);
    }
    if (service.rest_finished) {
      lifecycle_.conclude(service.for_sleep);
      commands_.interrupt();
      return {};
    }
    const auto pumped = driver_.pump_once();
    snapshot(!pumped);
    if (!pumped) {
      const auto after = driver_.state();
      const bool transient =
          pumped.error() !=
              net::protocol::remote_play::StreamError::invalid_argument &&
          active(after);
      if (!transient || ++transients > settings_.pump_error_tolerance)
        return {Outcome::Phase::session, pumped.error()};
      if (std::has_single_bit(transients))
        log(LogSeverity::warning,
            "pump transient result=" +
                std::string{CommandRoute::result_name(pumped.error())} +
                " count=" + std::to_string(transients));
    } else {
      transients = 0;
    }
    publish();
    const auto wait = std::min(service.wait, share_->service(service.wait));
    if (wait.count() <= 0 || media_.video().work_pending() ||
        commands_.pending(streaming) ||
        (streaming && controller_rules_.pending() != 0))
      continue;
    if (const auto waited = driver_.wait(wait); !waited)
      return {Outcome::Phase::session, waited.error()};
  }
}

auto Session::Core::finish(const LoopEnd& end) -> Outcome {
  const bool resting = commands_.rest().flushing();
  if (resting) {
    lifecycle_.conclude(commands_.rest().for_sleep());
    if (const auto pardoned = media_.pardon())
      log(LogSeverity::info,
          "Media stopped after the rest mode request: " + pardoned->stage);
  }
  Outcome::Evidence evidence;
  evidence.left = lifecycle_.cancelled();
  evidence.rest = LinkEvidence::rest(commands_.rest().outcome());
  evidence.media = media_.fault();
  media_.seal();
  lifecycle_.finish();
  commands_.conclude();
  share_->session_over();

  auto primary = driver_.failure();
  const bool sleeping = lifecycle_.sleeping();
  if (sleeping)
    log(LogSeverity::info, "System sleep: stopping " +
                               std::string{route_.name()} +
                               " session without PSN cleanup");
  const auto stopped = sleeping ? driver_.stop_for_sleep() : driver_.stop();
  auto cleanup = driver_.cleanup_failure();
  auto quit = driver_.quit();
  std::optional<net::protocol::remote_play::StreamError> stop_error;
  if (!stopped) stop_error = stopped.error();
  copy_statistics(driver_.state(), timing::Deadline::now(clock_));

  if (resting) {
    if (end.phase != Outcome::Phase::none || primary || cleanup || !stopped ||
        quit.reason != net::protocol::remote_play::QuitReason::none)
      log(LogSeverity::info,
          "Stream ended after the rest mode request: stage=" +
              std::to_string(static_cast<unsigned>(end.phase)) +
              " primary=" + std::to_string(primary.has_value() ? 1 : 0) +
              " cleanup=" + std::to_string(cleanup.has_value() ? 1 : 0) +
              " quit=" + std::to_string(static_cast<unsigned>(quit.reason)) +
              " stop=" +
              std::string{stopped
                              ? std::string_view{"ok"}
                              : CommandRoute::result_name(stopped.error())});
    primary.reset();
    cleanup.reset();
    stop_error.reset();
    quit = {};
  } else {
    evidence.phase = end.phase;
    if (end.error)
      evidence.phase_cause = primary && primary->result() == *end.error
                                 ? primary->end_cause()
                                 : LinkEvidence::cause(*end.error);
  }
  if (primary) {
    log(LogSeverity::error,
        std::format("session failure: route={} stage={} result={}",
                    primary->route() ? primary->route()->name()
                                     : std::string_view{"unknown"},
                    LinkStages::name(primary->stage()),
                    magic_enum::enum_name(primary->result())));
    if (!primary->cancelled())
      log(LogSeverity::error, "session ended: " + primary->sentence());
  }
  if (cleanup)
    log(LogSeverity::error, "session cleanup failed: " + cleanup->sentence());
  evidence.trace = SessionDiagnostics::outcome_trace(primary, cleanup, quit);
  if (primary) {
    std::optional<LaunchRefused> refused;
    if (reports_ != nullptr && !primary->cancelled())
      refused = reports_->launch_refused();
    evidence.primary = primary->end_reason(std::move(refused));
  }
  if (cleanup) evidence.cleanup = cleanup->end_reason();
  if (stop_error) evidence.stop_cause = LinkEvidence::cause(*stop_error);
  evidence.quit = LinkEvidence::quit(quit);
  journal_->retain();
  return Outcome{std::move(evidence)};
}

auto Session::Core::active(
    net::protocol::remote_play::StreamState state) noexcept -> bool {
  return state == net::protocol::remote_play::StreamState::establishing_route ||
         state == net::protocol::remote_play::StreamState::negotiating_stream ||
         state == net::protocol::remote_play::StreamState::streaming;
}

auto Session::Core::describe(
    const net::protocol::remote_play::event::StreamInfoReady& info)
    -> std::string {
  std::string text =
      "stream negotiated: " + std::to_string(info.profiles.size()) +
      " video profiles (";
  for (std::size_t index = 0; index < info.profiles.size(); ++index) {
    if (index != 0) text += ", ";
    text += std::to_string(info.profiles[index].width) + "x" +
            std::to_string(info.profiles[index].height);
  }
  text += "); audio " + std::to_string(info.audio.channels) + " ch " +
          std::to_string(info.audio.rate) + " Hz";
  return text;
}

void Session::Core::log(LogSeverity severity,
                        std::string_view message) const noexcept {
  journal_->write({severity, message});
}

void Session::Core::cancel() noexcept {
  lifecycle_.cancel();
  commands_.interrupt();
  if (!lifecycle_.sleeping()) driver_.cancel();
}

void Session::Core::stop_for_host_sleep() noexcept {
  if (!lifecycle_.sleep()) {
    cancel();
    return;
  }
  commands_.interrupt();
  driver_.wake();
}

auto Session::Core::stop_with_rest(RestRequest::Reason reason) -> bool {
  if (commands_.rest().request(reason)) return true;
  if (reason == RestRequest::Reason::host_sleep)
    stop_for_host_sleep();
  else
    cancel();
  return false;
}

void Session::Core::wake() noexcept { driver_.wake(); }

auto Session::Core::select_datacenter(std::string_view name)
    -> Expected<void, net::protocol::remote_play::StreamError> {
  if (lifecycle_.cancelled() ||
      lifecycle_.phase() == SessionLifecycle::Phase::finished)
    return unexpected(net::protocol::remote_play::StreamError::invalid_state);
  return driver_.select_datacenter(name);
}

auto Session::Core::link_statistics(
    const net::protocol::remote_play::StreamDiagnostics& wired)
    -> LinkStatistics {
  LinkStatistics link;
  link.available = wired.available;
  link.video_units = wired.video_units;
  link.rejected_packets = wired.rejected_packets;
  link.crypto_rejections = wired.crypto_rejections;
  link.assembled_video = wired.assembled_video;
  link.dependency_rejections = wired.dependency_rejections;
  link.assembly_rejections = wired.assembly_rejections;
  link.fec_repaired_frames = wired.video_fec_repaired;
  link.video_stale = wired.video_stale;
  link.video_metadata = wired.video_metadata;
  link.video_duplicate = wired.video_duplicate;
  link.video_invalid = wired.video_invalid;
  link.dropped_posts = wired.dropped_posts;
  link.reorder_skipped = wired.reorder_skipped;
  link.send_dropped = wired.send_dropped;
  link.reorder_dropped = wired.reorder_dropped;
  link.protocol_features = wired.takion_features;
  const auto& spread = wired.round_trip.spread;
  link.round_trip = {wired.round_trip.present,
                     wired.round_trip.smoothed_us,
                     wired.round_trip.variation_us,
                     spread.samples,
                     spread.median_us,
                     spread.p90_us,
                     spread.p99_us,
                     spread.max_us,
                     spread.recent_max_us};
  link.reception = {wired.extension.present, wired.extension.jitter_us,
                    wired.extension.received, wired.extension.lost,
                    wired.extension.loss_ratio};
  const auto& metrics = wired.video_metrics;
  link.video_metrics = {metrics.present,          metrics.reports_present,
                        metrics.throughput_bps,   metrics.loss_ratio,
                        metrics.send_interval_us, metrics.receive_interval_us,
                        metrics.transfer_time_us, metrics.transmission_time_us,
                        metrics.frame_bytes,      metrics.frame_number,
                        metrics.frame_estimated,  metrics.reports_sent};
  const auto& marks = wired.tos;
  link.service_marks = {marks.present,     marks.last_present, marks.last_dscp,
                        marks.last_ecn,    marks.packets,      marks.reports,
                        marks.dscp_counts, marks.ecn_counts};
  const auto& server = wired.service;
  link.server = {server.present,
                 server.reports,
                 server.available_down_kbps,
                 server.used_down_kbps,
                 server.used_up_kbps,
                 server.round_trip_ms,
                 server.loss_percent,
                 server.upstream_loss_percent,
                 server.gateway_round_trip_us,
                 server.audio_channels,
                 server.width,
                 server.height,
                 server.corrupt_frames,
                 server.dynamic_range,
                 server.video_codec};
  return link;
}

auto Session::Core::statistics() const -> Statistics {
  Statistics statistics;
  {
    const platform::Lock lock(mutex_);
    statistics.link = link_statistics(link_);
    statistics.protocol_version = takion_version_;
    statistics.loop_errors = pump_errors_;
  }
  statistics.video = media_.video().statistics();
  statistics.audio_failures = media_.audio_failures();
  statistics.microphone = commands_.microphone().statistics();
  statistics.controller_pending = controller_rules_.pending();
  return statistics;
}

void Session::Core::snapshot(bool pump_failed) {
  if (pump_failed) {
    const platform::Lock lock(mutex_);
    ++pump_errors_;
  }
  const auto state = driver_.state();
  const auto now = timing::Deadline::now(clock_);
  if (state != statistics_state_ || statistics_due_.expired(now))
    copy_statistics(state, now);
}

void Session::Core::copy_statistics(
    net::protocol::remote_play::StreamState state,
    timing::Deadline::Instant now) {
  statistics_state_ = state;
  statistics_due_ = timing::Deadline::after(
      now, std::chrono::duration_cast<timing::Deadline::Duration>(
               settings_.statistics_interval));
  const auto diagnostics = driver_.diagnostics();
  const auto version = driver_.takion_version();
  const platform::Lock lock(mutex_);
  if (diagnostics.available) link_ = diagnostics;
  if (version) takion_version_ = version;
}

void Session::Core::on_video_frame(media::VideoFrame frame) {
  media_.deliver(std::move(frame));
}

void Session::Core::on_event(net::protocol::remote_play::StreamEvent event) {
  if (auto* media =
          std::get_if<net::protocol::remote_play::event::EncodedMedia>(
              &event)) {
    const bool profile =
        media->kind ==
        net::protocol::remote_play::EncodedMediaKind::audio_profile;
    media_.deliver(std::move(*media));
    if (profile) commands_.audio_profile_changed();
    return;
  }
  if (const auto* info =
          std::get_if<net::protocol::remote_play::event::StreamInfoReady>(
              &event)) {
    if (media_.configure(*info)) {
      log(LogSeverity::info, describe(*info));
      if (const auto version = driver_.takion_version())
        log(LogSeverity::info,
            "takion version: streaming=" + std::to_string(*version));
    }
    commands_.observe(event);
    return;
  }
  if (std::holds_alternative<net::protocol::remote_play::event::Failed>(event))
    media_.seal();
  if (const auto* holepunch =
          std::get_if<net::protocol::remote_play::event::Holepunch>(&event))
    log(LogSeverity::info,
        holepunch->finished ? "holepunch finished" : "holepunch started");
  commands_.observe(event);
  std::visit(
      [this](const auto& reported) {
        if constexpr (requires { this->report(reported); }) report(reported);
      },
      event);
  publish();
}

void Session::Core::report(
    const net::protocol::remote_play::event::StateChanged& changed) {
  switch (changed.state) {
    case net::protocol::remote_play::StreamState::streaming:
      advance(SessionState::playing);
      return;
    case net::protocol::remote_play::StreamState::stopping:
      advance(SessionState::leaving);
      return;
    case net::protocol::remote_play::StreamState::idle:
    case net::protocol::remote_play::StreamState::establishing_route:
    case net::protocol::remote_play::StreamState::negotiating_stream:
    case net::protocol::remote_play::StreamState::stopped:
    case net::protocol::remote_play::StreamState::failed:
      return;
  }
}

void Session::Core::report(
    const net::protocol::remote_play::event::PinRequired& required) {
  events_.on_login_pin_needed(required.incorrect);
}

void Session::Core::report(
    const net::protocol::remote_play::event::SetupProgress& progress) {
  if (is_launch_stage(progress.stage)) {
    if (reports_ != nullptr) reports_->launch_progress(progress);
    return;
  }
  const auto step = connect_step(progress.stage);
  if (!step) return;
  ConnectProgress reported{
      *step,
      std::chrono::microseconds{
          static_cast<std::chrono::microseconds::rep>(progress.elapsed_us)},
      attempt_, reconnecting_};
  reported.previous_failure = previous_failure_;
  reported.route = progress.route;
  if (progress.route && progress.route->step ==
                            progress::InternetRoute::Step::waiting_for_console)
    reported.wait = progress::Wait{progress::WaitReason::console_joining};
  events_.on_connect_progress(reported);
}

void Session::Core::report(
    const net::protocol::remote_play::event::DisplayProhibited& display) {
  events_.on_picture_blocked(display.prohibited);
}

void Session::Core::report(
    const net::protocol::remote_play::event::ConnectionWaiting& waiting) {
  events_.on_connection_waiting(waiting.waiting);
}

void Session::Core::report(
    const net::protocol::remote_play::event::PlayTimeLeft& left) {
  using Reason = net::protocol::takion::PlayTimeReason;
  const auto limit =
      left.left.reason == Reason::session_limit ? PlayTimeLimit::session_limit
      : left.left.reason == Reason::maintenance ? PlayTimeLimit::maintenance
                                                : PlayTimeLimit::other;
  events_.on_play_time_left(std::chrono::minutes{left.left.minutes}, limit);
}

void Session::Core::report(
    const net::protocol::remote_play::event::CloudMessage& message) {
  if (message.address == CloudMessaging::notification_address) {
    if (const auto notice = CloudMessaging::notice(message.body))
      events_.on_cloud_notice(*notice);
  } else if (message.address == CloudMessaging::banner_address) {
    if (const auto banner = CloudMessaging::recording_banner(message.body))
      events_.on_recording_banner(*banner);
  } else if (message.address == CloudMessaging::capture_result_address) {
    const auto received = captures_.receive(message.body);
    if (received.thumbnail) events_.on_capture_thumbnail(*received.thumbnail);
    if (received.uploaded) events_.on_capture_uploaded(*received.uploaded);
  }
}

void Session::Core::report(
    const net::protocol::remote_play::event::KeyboardOpen& keyboard) {
  events_.on_keyboard_opened(keyboard.text);
}

void Session::Core::report(
    const net::protocol::remote_play::event::KeyboardTextChange& keyboard) {
  events_.on_keyboard_text(keyboard.text);
}

void Session::Core::report(
    const net::protocol::remote_play::event::KeyboardRemoteClose&) {
  events_.on_keyboard_closed();
}

void Session::Core::report(
    const net::protocol::remote_play::event::PadPasscodeRequest& request) {
  const auto seat = controllers::SeatNumber::masked(request.index);
  seat_rules_.passcode_needed(seat);
  events_.on_seat_passcode_needed(seat, request.retry);
}

void Session::Core::report(
    const net::protocol::remote_play::event::PadJoinFailed& failed) {
  const auto seat = controllers::SeatNumber::masked(failed.index);
  seat_rules_.emptied(seat);
  const auto refusal = SeatRefusal::by_console(seat, failed.status);
  events_.on_seat_refused(refusal);
  share_->seat_refused(refusal);
}

void Session::Core::report(
    const net::protocol::remote_play::event::PadAnnounced& announced) {
  const auto seat = controllers::SeatNumber::masked(announced.index);
  seat_rules_.joined(seat);
  events_.on_seat_joined(
      seat, {announced.led[0], announced.led[1], announced.led[2]});
  share_->seat_joined(seat);
}

void Session::Core::report(
    const net::protocol::remote_play::event::PadDropped& dropped) {
  const auto seat = controllers::SeatNumber::masked(dropped.index);
  if (!seat_rules_.dropped(seat)) return;
  events_.on_seat_left(seat);
  share_->seat_left(seat);
}

void Session::Core::report(
    const net::protocol::remote_play::event::PadAccountUnresolvable&
        unresolvable) {
  const auto seat = controllers::SeatNumber::masked(unresolvable.index);
  seat_rules_.emptied(seat);
  const auto refusal = SeatRefusal::account_not_on_console(seat);
  events_.on_seat_refused(refusal);
  share_->seat_refused(refusal);
}

void Session::Core::report(
    const net::protocol::remote_play::event::CloudDatacenters& datacenters) {
  if (reports_ != nullptr) reports_->datacenters_measured(datacenters);
}

void Session::Core::report(
    const net::protocol::remote_play::event::CloudDatacenterSelected&
        selected) {
  if (reports_ != nullptr) reports_->datacenter_selected(selected);
}

void Session::Core::report(
    const net::protocol::remote_play::event::NicknameReceived& named) {
  if (reports_ != nullptr) reports_->console_named(named.nickname);
}

auto Session::Core::is_launch_stage(
    net::protocol::remote_play::FailureStage stage) noexcept -> bool {
  switch (stage) {
    case net::protocol::remote_play::FailureStage::cloud_provision:
    case net::protocol::remote_play::FailureStage::cloud_identity:
    case net::protocol::remote_play::FailureStage::cloud_client_ids:
    case net::protocol::remote_play::FailureStage::cloud_config:
    case net::protocol::remote_play::FailureStage::cloud_start:
    case net::protocol::remote_play::FailureStage::cloud_oauth:
    case net::protocol::remote_play::FailureStage::cloud_authorize:
    case net::protocol::remote_play::FailureStage::cloud_lock:
    case net::protocol::remote_play::FailureStage::cloud_datacenter:
    case net::protocol::remote_play::FailureStage::cloud_allocate:
    case net::protocol::remote_play::FailureStage::cloud_title:
    case net::protocol::remote_play::FailureStage::cloud_launch_check:
      return true;
    default:
      return false;
  }
}

auto Session::Core::connect_step(
    net::protocol::remote_play::FailureStage stage) noexcept
    -> std::optional<ConnectStep> {
  switch (stage) {
    case net::protocol::remote_play::FailureStage::resolve:
      return ConnectStep::finding_console;
    case net::protocol::remote_play::FailureStage::psn_open:
    case net::protocol::remote_play::FailureStage::psn_start:
      return ConnectStep::opening_account_session;
    case net::protocol::remote_play::FailureStage::registration:
      return ConnectStep::pairing;
    case net::protocol::remote_play::FailureStage::control_holepunch:
      return ConnectStep::reaching_console;
    case net::protocol::remote_play::FailureStage::data_holepunch:
    case net::protocol::remote_play::FailureStage::route_switch:
      return ConnectStep::opening_stream_route;
    case net::protocol::remote_play::FailureStage::session_init:
      return ConnectStep::requesting_session;
    case net::protocol::remote_play::FailureStage::ctrl_startup:
    case net::protocol::remote_play::FailureStage::ctrl_session_id:
    case net::protocol::remote_play::FailureStage::ctrl_features:
    case net::protocol::remote_play::FailureStage::ctrl_receive:
      return ConnectStep::opening_control;
    case net::protocol::remote_play::FailureStage::calibration:
      return ConnectStep::measuring_connection;
    case net::protocol::remote_play::FailureStage::active_prepare:
    case net::protocol::remote_play::FailureStage::active_connect:
    case net::protocol::remote_play::FailureStage::active_bang:
    case net::protocol::remote_play::FailureStage::active_crypto:
    case net::protocol::remote_play::FailureStage::stream_info:
      return ConnectStep::starting_stream;
    default:
      return std::nullopt;
  }
}

auto Session::Core::go_home() -> Expected<void, InputRefusal> {
  if (const auto asked = commands_.go_home(); !asked)
    return unexpected(asked.error());
  return {};
}

auto Session::Core::set_audio_3d(bool enabled) -> Expected<void, InputRefusal> {
  return commands_.set_audio_3d(enabled);
}

auto Session::Core::press_create(Session::CreatePress press)
    -> Expected<void, InputRefusal> {
  return commands_.press_create(press);
}

auto Session::Core::open_saved_data() -> Expected<void, InputRefusal> {
  return commands_.open_saved_data();
}

auto Session::Core::join_invitation(std::string invitation_id,
                                    std::string session_id)
    -> Expected<void, InputRefusal> {
  return commands_.join_invitation(std::move(invitation_id),
                                   std::move(session_id));
}

auto Session::Core::set_system_ui(Session::SystemUi state)
    -> Expected<void, InputRefusal> {
  return commands_.set_system_ui(state);
}

auto Session::Core::set_accessibility(const AccessibilityRequest& accessibility)
    -> Expected<void, InputRefusal> {
  return commands_.set_accessibility(accessibility);
}

auto Session::Core::send_keyboard(const KeyboardState& state)
    -> Expected<void, InputRefusal> {
  return commands_.send_keyboard(state);
}

auto Session::Core::audio_3d() const noexcept -> bool {
  return commands_.audio_3d();
}

auto Session::Core::video_presented() noexcept
    -> std::optional<media::PresentationLoss> {
  const auto loss = media_.presented();
  const auto now = state();
  if (now != SessionState::connecting && now != SessionState::playing) {
    return std::nullopt;
  }
  return loss;
}

void Session::Core::video_rejected(std::uint16_t frame, int cause) {
  media_.rejected(frame, cause);
  driver_.wake();
}

void Session::Core::video_output_failed(int cause) {
  media_.output_failed(cause);
  driver_.wake();
}

auto Session::Core::request_keyframe() -> bool {
  if (lifecycle_.phase() != SessionLifecycle::Phase::running) return false;
  media_.video().request_idr();
  driver_.wake();
  return true;
}

auto Session::Core::drop_video_frame() -> bool {
  if (lifecycle_.phase() != SessionLifecycle::Phase::running) return false;
  media_.video().request_drop();
  driver_.wake();
  return true;
}

auto Session::Core::leave(Leave how) -> bool {
  {
    const platform::Lock lock(mutex_);
    if (state_.load(std::memory_order_relaxed) == SessionState::ended)
      return false;
  }
  advance(SessionState::leaving);
  if (how == Leave::rest_console)
    return stop_with_rest(RestRequest::Reason::leaving);
  cancel();
  return true;
}

void Session::Core::leave_for_host_sleep(bool rest_console) noexcept {
  advance(SessionState::leaving);
  if (rest_console)
    static_cast<void>(stop_with_rest(RestRequest::Reason::host_sleep));
  else
    stop_for_host_sleep();
}

auto Session::Core::outcome() const -> std::optional<Outcome> {
  const platform::Lock lock(mutex_);
  return outcome_;
}

auto Session::Core::wait_until_ended() -> const Outcome& {
  platform::Lock lock(mutex_);
  const bool inside = runner_ == platform::ThreadIdentity::current();
  changed_.wait(lock, [this, inside] {
    return delivered_ || (inside && outcome_.has_value());
  });
  return *outcome_;
}

Session::Session(std::unique_ptr<Core> core) noexcept
    : core_(std::move(core)) {}

Session::~Session() = default;

auto Session::player() const noexcept -> const users::PsUser& {
  return core_->player();
}

auto Session::where() const noexcept -> Where { return core_->where(); }

auto Session::state() const noexcept -> SessionState { return core_->state(); }

auto Session::console() const noexcept -> const consoles::Console* {
  return core_->console();
}

auto Session::launch() noexcept -> CloudLaunch* { return core_->launch(); }

auto Session::journal() const noexcept
    -> const std::shared_ptr<diagnostics::SessionJournal>& {
  return core_->journal();
}

auto Session::controllers() noexcept -> Controllers& {
  return core_->controllers();
}

auto Session::seats() noexcept -> Seats& { return core_->seats(); }

auto Session::login_pin() noexcept -> LoginPin& { return core_->login_pin(); }

auto Session::keyboard() noexcept -> ConsoleKeyboard& {
  return core_->keyboard();
}

auto Session::microphone() noexcept -> Microphone& {
  return core_->microphone();
}

auto Session::share() noexcept -> SessionShare& { return *core_->share(); }

auto Session::go_home() -> Expected<void, InputRefusal> {
  return core_->go_home();
}

auto Session::send_keyboard(const KeyboardState& state)
    -> Expected<void, InputRefusal> {
  return core_->send_keyboard(state);
}

auto Session::press_create(CreatePress press) -> Expected<void, InputRefusal> {
  return core_->press_create(press);
}

auto Session::open_saved_data() -> Expected<void, InputRefusal> {
  return core_->open_saved_data();
}

auto Session::join_invitation(std::string invitation_id, std::string session_id)
    -> Expected<void, InputRefusal> {
  return core_->join_invitation(std::move(invitation_id),
                                std::move(session_id));
}

auto Session::set_system_ui(SystemUi state) -> Expected<void, InputRefusal> {
  return core_->set_system_ui(state);
}

auto Session::set_accessibility(const AccessibilityRequest& accessibility)
    -> Expected<void, InputRefusal> {
  return core_->set_accessibility(accessibility);
}

auto Session::set_audio_3d(bool enabled) -> Expected<void, InputRefusal> {
  return core_->set_audio_3d(enabled);
}

auto Session::audio_3d() const noexcept -> bool { return core_->audio_3d(); }

auto Session::video_presented() noexcept
    -> std::optional<media::PresentationLoss> {
  return core_->video_presented();
}

void Session::video_rejected(std::uint16_t frame, int cause) {
  core_->video_rejected(frame, cause);
}

void Session::video_output_failed(int cause) {
  core_->video_output_failed(cause);
}

auto Session::request_keyframe() -> bool { return core_->request_keyframe(); }

auto Session::drop_video_frame() -> bool { return core_->drop_video_frame(); }

auto Session::leave(Leave how) -> bool { return core_->leave(how); }

void Session::leave_for_host_sleep(bool rest_console) noexcept {
  core_->leave_for_host_sleep(rest_console);
}

auto Session::statistics() const -> Statistics { return core_->statistics(); }

auto Session::outcome() const -> std::optional<Outcome> {
  return core_->outcome();
}

auto Session::wait_until_ended() -> const Outcome& {
  return core_->wait_until_ended();
}

}  // namespace libchiaking::play
