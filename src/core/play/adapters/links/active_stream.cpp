#include "src/core/play/adapters/links/active_stream.hpp"

#include "src/core/net/protocol/remote_play/stream/microphone_sender.hpp"
#include "src/core/net/protocol/remote_play/stream/public_media_event.hpp"
#include "src/core/net/protocol/remote_play/stream/stream_info.hpp"
#include "src/core/utils/text/hex.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace libchiaking::play {

namespace rp = net::protocol::remote_play;
namespace takion = net::protocol::takion;

ActiveStream::ActiveStream(Owner& owner, LinkEnding& ending,
                           platform::PlatformFactory& platform,
                           platform::StopToken stop, Profile profile)
    : owner_(owner),
      ending_(ending),
      platform_(platform),
      clock_(platform.clock()),
      trace_(platform.supplies(platform::PlatformCapability::diagnostics)
                 ? &platform.diagnostics()
                 : nullptr,
             platform.clock(), profile.component, stop),
      couch_(profile.console),
      features_(platform.supplies(platform::PlatformCapability::diagnostics)
                    ? &platform.diagnostics()
                    : nullptr),
      feedback_(rp::FeedbackDeliveryPolicy::for_mode(profile.feedback)),
      lean_(platform.network().capabilities().costly_socket_calls),
      waiter_(platform.network(), platform.threading(), platform.clock(),
              std::move(stop)),
      ctrl_open_(profile.has_ctrl) {
  if (!profile.cloud_receivers.empty())
    messaging_.emplace(profile.cloud_receivers);
}

ActiveStream::~ActiveStream() { static_cast<void>(release_media()); }

void ActiveStream::emit(rp::StreamEvent event) {
  trace_.event(event);
  if (sink_) sink_->on_event(std::move(event));
}

void ActiveStream::enter(rp::StreamState state) {
  state_ = state;
  emit(rp::event::StateChanged{state_});
}

void ActiveStream::prohibit(bool prohibited) {
  std::vector<rp::StreamEvent> events;
  features_.on_rp_prohibit(prohibited, events);
  for (auto& event : events) emit(std::move(event));
}

auto ActiveStream::running() const noexcept -> bool {
  return state_ == rp::StreamState::establishing_route ||
         state_ == rp::StreamState::negotiating_stream ||
         state_ == rp::StreamState::streaming;
}

auto ActiveStream::ended() const noexcept -> bool {
  return state_ == rp::StreamState::stopped ||
         state_ == rp::StreamState::failed;
}

void ActiveStream::at(rp::FailureStage stage) noexcept {
  stage_ = stage;
  trace_.stage(stage);
}

void ActiveStream::idle(std::chrono::microseconds remaining) {
  static_cast<void>(waiter_.wait(remaining, {}, false));
}

void ActiveStream::calibrate(
    std::unique_ptr<rp::LocalCalibratedPipeline> pipeline) {
  media_ = std::move(pipeline);
}

void ActiveStream::connect(std::unique_ptr<rp::ActiveSession> session) {
  session->set_diagnostics(&trace_);
  media_ = std::move(session);
}

auto ActiveStream::has_media() const noexcept -> bool {
  return !std::holds_alternative<std::monostate>(media_);
}

auto ActiveStream::pipeline() noexcept -> rp::LocalCalibratedPipeline* {
  auto* held =
      std::get_if<std::unique_ptr<rp::LocalCalibratedPipeline>>(&media_);
  return held ? held->get() : nullptr;
}

auto ActiveStream::active() noexcept -> rp::ActiveSession* {
  if (auto* pipeline = this->pipeline()) return pipeline->active();
  auto* held = std::get_if<std::unique_ptr<rp::ActiveSession>>(&media_);
  return held ? held->get() : nullptr;
}

auto ActiveStream::active() const noexcept -> const rp::ActiveSession* {
  if (const auto* held =
          std::get_if<std::unique_ptr<rp::LocalCalibratedPipeline>>(&media_))
    return (*held)->active();
  const auto* held = std::get_if<std::unique_ptr<rp::ActiveSession>>(&media_);
  return held ? held->get() : nullptr;
}

auto ActiveStream::media_source() noexcept -> rp::ActiveSession* {
  if (auto* pipeline = this->pipeline()) return pipeline->media_source();
  return active();
}

auto ActiveStream::streaming_session() noexcept -> rp::ActiveSession* {
  return streaming() ? active() : nullptr;
}

auto ActiveStream::couch_backend() -> Couch::Binding {
  return Couch::Binding{couch_,
                        delivery_ && streaming() ? &*delivery_ : nullptr,
                        streaming_session(), now_us()};
}

void ActiveStream::observe() {
  if (const auto* session = active()) {
    trace_.active(session->state());
    if (trace_.health_due()) trace_.health(diagnostics());
    const auto now = now_us();
    const auto last = session->last_inbound_us();
    const bool waiting = streaming() && last != 0 && now >= last &&
                         now - last >= connection_waiting_notice_us;
    if (waiting != connection_waiting_) {
      connection_waiting_ = waiting;
      emit(rp::event::ConnectionWaiting{waiting});
    }
  }
}

auto ActiveStream::pump_media() -> Progress {
  pump_error_.reset();
  if (auto* pipeline = this->pipeline()) {
    if (const auto pumped = pipeline->pump_once(); !pumped) {
      pump_error_ = pumped.error();
      return Progress::failed;
    }
    if (pipeline->state() == rp::LocalPipelineState::stopped)
      return Progress::stopped;
    return pipeline->state() == rp::LocalPipelineState::failed
               ? Progress::failed
               : Progress::running;
  }
  auto* session = active();
  if (!session || !session->pump_once()) return Progress::failed;
  if (session->state() == rp::ActiveHandshakeState::closed)
    return Progress::stopped;
  return session->state() == rp::ActiveHandshakeState::failed
             ? Progress::failed
             : Progress::running;
}

auto ActiveStream::stream_when_ready(
    std::optional<rp::event::EffectiveLaunch> launch, std::string_view note)
    -> Result {
  auto* session = active();
  if (state_ != rp::StreamState::negotiating_stream || !session ||
      !session->ready_info())
    return {};
  auto info =
      rp::StreamInfoCodec::to_public_stream_info(*session->ready_info());
  info.launch = launch;
  delivery_.emplace(*session, now_us(), feedback_);
  if (const auto begun = couch_.begin_streaming(*delivery_, now_us()); !begun)
    return begun;
  state_ = rp::StreamState::streaming;
  if (platform_.supplies(platform::PlatformCapability::diagnostics) &&
      !note.empty())
    platform_.diagnostics().write({platform::LogSeverity::info, note});
  emit(std::move(info));
  if (streaming()) emit(rp::event::StateChanged{state_});
  return {};
}

void ActiveStream::keep_streaming() {
  if (streaming() && !delivery_->refresh(now_us())) ++deferred_refreshes_;
  open_messaging();
  deliver_media();
}

void ActiveStream::open_messaging() {
  if (!messaging_ || messaging_opened_ || !streaming()) return;
  messaging_opened_ = true;
  const auto sent = send_cloud_transfer(messaging_->open());
  if (platform_.supplies(platform::PlatformCapability::diagnostics))
    platform_.diagnostics().write(
        {platform::LogSeverity::debug,
         std::format("stream: cloud messaging handshake {}",
                     sent ? "posted" : "could not be posted")});
}

auto ActiveStream::send_cloud_transfer(CloudMessaging::Transfer transfer)
    -> Result {
  auto* session = streaming_session();
  if (!session) return unexpected(rp::StreamError::invalid_state);
  return posted(*session,
                session->post_generic_data(std::move(transfer.data),
                                           std::move(transfer.metadata)));
}

auto ActiveStream::send_cloud_message(std::uint32_t address,
                                      std::uint32_t version,
                                      std::vector<std::byte> body) -> Result {
  if (!messaging_) return unexpected(rp::StreamError::invalid_configuration);
  if (!streaming() || !messaging_->allows(address))
    return unexpected(rp::StreamError::invalid_state);
  return send_cloud_transfer(
      messaging_->message(address, version, std::move(body)));
}

auto ActiveStream::receive_cloud_transfer(
    const rp::UnrecognisedMessage& message) -> bool {
  if (!messaging_ ||
      message.kind != rp::UnrecognisedMessage::Kind::generic_data)
    return false;
  auto received = messaging_->receive(message.metadata, message.bytes);
  if (received.replies.empty() && !received.message && !received.result)
    return false;
  for (auto& reply : received.replies)
    static_cast<void>(send_cloud_transfer(std::move(reply)));
  if (received.message) {
    if (received.message->address == CloudMessaging::notification_address) {
      const auto notice = CloudMessaging::notice(received.message->body);
      if (notice && notice->kind == CloudNoticeKind::trial_time_left)
        trial_warned_ = true;
    }
    emit(rp::event::CloudMessage{received.message->address,
                                 received.message->body});
  }
  if (!platform_.supplies(platform::PlatformCapability::diagnostics))
    return true;
  auto& diagnostics = platform_.diagnostics();
  if (received.result)
    diagnostics.write(
        {platform::LogSeverity::debug,
         std::format("stream: cloud message result for address {}, id {}: {}",
                     received.result->address, received.result->id,
                     received.result->result)});
  if (received.message) {
    constexpr std::size_t shown = 2000;
    const auto& body = received.message->body;
    const bool printable = std::ranges::all_of(body, [](std::byte value) {
      const auto code = std::to_integer<unsigned char>(value);
      return code == 0 || code == '\n' || code == '\t' ||
             (code >= 0x20 && code < 0x7f);
    });
    std::string shown_body;
    if (printable) {
      for (const auto value :
           std::span{body}.first(std::min(body.size(), shown)))
        if (value != std::byte{0})
          shown_body.push_back(static_cast<char>(value));
    } else {
      text::append_hex(shown_body,
                       std::span{body}.first(std::min(body.size(), shown)));
    }
    diagnostics.write(
        {received.message->address == CloudMessaging::game_intent_address
             ? platform::LogSeverity::info
             : platform::LogSeverity::debug,
         std::format("stream: cloud message to address {}, version {}, id {}, "
                     "{} bytes: {}",
                     received.message->address, received.message->version,
                     received.message->id, body.size(), shown_body)});
  }
  if (!received.replies.empty())
    diagnostics.write(
        {platform::LogSeverity::debug,
         std::format("stream: cloud messaging handshake, {} replies sent",
                     received.replies.size())});
  return true;
}

void ActiveStream::deliver_media() {
  for (std::size_t count = 0; count < frames_per_delivery && running();
       ++count) {
    auto* source = media_source();
    if (!source) return;
    std::optional<rp::ActiveMediaEvent> media;
    if (streaming() && source->ready_info()) media = source->take_media_event();
    auto feedback = source->take_feedback_event();
    if (const auto left = source->take_play_time_left()) {
      record_play_time_left(*left);
      emit(rp::event::PlayTimeLeft{*left});
    }
    while (auto unknown = source->take_unrecognised())
      if (!receive_cloud_transfer(*unknown)) record_unrecognised(*unknown);
    if (!media && !feedback) break;
    if (media) emit(rp::PublicMediaEvent::from(std::move(*media)));
    if (feedback && running())
      emit(rp::PublicMediaEvent::from_feedback(std::move(*feedback)));
  }
}

void ActiveStream::record_play_time_left(
    const net::protocol::takion::PlayTimeLeft& left) noexcept {
  try {
    if (platform_.supplies(platform::PlatformCapability::diagnostics))
      platform_.diagnostics().write(
          {platform::LogSeverity::info,
           std::format("stream: play time left {} min, reason {} \"{}\"",
                       left.minutes, magic_enum::enum_name(left.reason),
                       left.reason_text)});
  } catch (...) {
  }
}

void ActiveStream::record_unrecognised(
    const rp::UnrecognisedMessage& message) noexcept {
  try {
    if (!platform_.supplies(platform::PlatformCapability::diagnostics))
      return;
    auto& diagnostics = platform_.diagnostics();
    constexpr std::size_t shown_bytes = 256;
    constexpr std::size_t shown_metadata = 200;
    constexpr std::size_t shown_text = 2000;
    if (message.kind == rp::UnrecognisedMessage::Kind::rejection) {
      std::string spaced;
      for (const auto value : message.bytes)
        spaced += std::format("{:02x} ", std::to_integer<unsigned>(value));
      diagnostics.write(
          {platform::LogSeverity::debug,
           std::format(
               "stream: received packet rejected: {} at inbound line {}, {} "
               "bytes: {}",
               magic_enum::enum_name(
                   static_cast<takion::ClientError>(message.type)),
               message.metadata, message.bytes.size(), spaced)});
      return;
    }
    if (message.kind == rp::UnrecognisedMessage::Kind::generic_data_outcome) {
      diagnostics.write(
          {platform::LogSeverity::debug,
           std::format("stream: sent generic data transfer {} {}", message.type,
                       message.metadata)});
      return;
    }
    if (message.kind == rp::UnrecognisedMessage::Kind::deep_link) {
      diagnostics.write(
          {platform::LogSeverity::debug,
           std::format(
               "stream: deep link type {}, {}", message.type,
               std::string_view{message.metadata}.substr(0, shown_text))});
      return;
    }
    if (message.kind == rp::UnrecognisedMessage::Kind::microphone) {
      diagnostics.write(
          {platform::LogSeverity::info,
           std::format("stream: microphone {}", message.metadata)});
      return;
    }
    if (message.kind == rp::UnrecognisedMessage::Kind::server_message) {
      diagnostics.write(
          {platform::LogSeverity::debug,
           std::format(
               "stream: server message, {} characters: {}",
               message.metadata.size(),
               std::string_view{message.metadata}.substr(0, shown_text))});
      return;
    }
    std::string hex;
    text::append_hex(hex, std::span{message.bytes}.first(
                              std::min(message.bytes.size(), shown_bytes)));
    const bool control = message.kind == rp::UnrecognisedMessage::Kind::control;
    auto line =
        control
            ? std::format(
                  "stream: unrecognised control message type {}, {} bytes: {}",
                  message.type, message.bytes.size(), hex)
            : std::format(
                  "stream: generic data transfer {}, metadata \"{}\", {} "
                  "bytes: {}",
                  message.type,
                  std::string_view{message.metadata}.substr(0, shown_metadata),
                  message.bytes.size(), hex);
    diagnostics.write({platform::LogSeverity::debug, line});
  } catch (...) {
  }
}

auto ActiveStream::release_media() -> bool {
  delivery_.reset();
  route_ = nullptr;
  if (auto* session = active(); lean_ && session)
    session->set_disconnect_drain(lean_disconnect_drain);
  const auto stop = [this](auto& held) {
    if (!held || held->request_stop()) return true;
    ending_.record_cleanup_failure(
        LinkFailure::shut_down(held->stop_failure()));
    return false;
  };
  const bool cleaned = std::visit(
      [&](auto& held) {
        if constexpr (std::is_same_v<std::decay_t<decltype(held)>,
                                     std::monostate>)
          return true;
        else
          return stop(held);
      },
      media_);
  media_ = std::monostate{};
  return cleaned;
}

auto ActiveStream::send_controller_state(const rp::ControllerState& state)
    -> Result {
  if (state_ == rp::StreamState::idle ||
      state_ == rp::StreamState::establishing_route ||
      state_ == rp::StreamState::negotiating_stream)
    return couch_.store_before_streaming(state);
  if (!streaming() || !delivery_)
    return unexpected(rp::StreamError::invalid_state);
  if (!delivery_->submit(state, now_us()))
    return unexpected(rp::StreamError::controller_rejected);
  couch_.store(state);
  if (!lean_) waiter_.wake();
  return {};
}

auto ActiveStream::send_deep_link(std::string text) -> Result {
  auto* session = streaming_session();
  if (!session) return unexpected(rp::StreamError::invalid_state);
  takion::DeepLink link;
  link.type = takion::DeepLink::Type::raw;
  link.request_id = deep_link_requests_++;
  link.raw_data = std::move(text);
  return posted(*session, session->post_deep_link(link));
}

auto ActiveStream::send_invitation(std::string invitation_id,
                                   std::string session_id) -> Result {
  auto* session = streaming_session();
  if (!session) return unexpected(rp::StreamError::invalid_state);
  takion::DeepLink link;
  link.type = takion::DeepLink::Type::invite;
  link.request_id = deep_link_requests_++;
  link.invitation_id = std::move(invitation_id);
  link.session_id = std::move(session_id);
  return posted(*session, session->post_deep_link(link));
}

auto ActiveStream::send_saved_data() -> Result {
  auto* session = streaming_session();
  if (!session) return unexpected(rp::StreamError::invalid_state);
  takion::DeepLink link;
  link.type = takion::DeepLink::Type::save_data;
  link.request_id = deep_link_requests_++;
  return posted(*session, session->post_deep_link(link));
}

auto ActiveStream::send_system_menu_toggle(std::uint32_t pad) -> Result {
  auto* session = streaming_session();
  if (!session) return unexpected(rp::StreamError::invalid_state);
  return posted(*session, session->post_system_menu_toggle(pad));
}

auto ActiveStream::send_microphone_connection() -> Result {
  auto* session = streaming_session();
  if (!session) return unexpected(rp::StreamError::invalid_state);
  return posted(*session, session->post_microphone_connection());
}

auto ActiveStream::send_microphone_mute(bool muted) -> Result {
  auto* session = streaming_session();
  if (!session) return unexpected(rp::StreamError::invalid_state);
  return posted(*session, session->post_microphone_mute(muted));
}

auto ActiveStream::report_video_loss(std::uint16_t first, std::uint16_t last)
    -> Result {
  auto* session = streaming_session();
  if (!session) return unexpected(rp::StreamError::invalid_state);
  if (!session->report_video_loss(first, last))
    return fail(rp::StreamError::pipeline_failed,
                rp::StreamFailure::pipeline_failed);
  return {};
}

auto ActiveStream::send_microphone(std::span<const std::byte> opus_frame)
    -> Result {
  auto* session = streaming_session();
  if (!session) return unexpected(rp::StreamError::invalid_state);
  if (opus_frame.size() != rp::MicrophoneSender::frame_size)
    return unexpected(rp::StreamError::invalid_configuration);
  if (!session->post_microphone(opus_frame))
    return fail(rp::StreamError::pipeline_failed,
                rp::StreamFailure::pipeline_failed);
  waiter_.wake();
  return {};
}

auto ActiveStream::posted(rp::ActiveSession& session,
                          const Expected<void, rp::ActiveSessionError>& result)
    -> Result {
  if (!result) {
    if (result.error() == rp::ActiveSessionError::unsupported_request)
      return unexpected(rp::StreamError::invalid_configuration);
    if (session.failure())
      return fail(rp::StreamError::pipeline_failed,
                  rp::StreamFailure::pipeline_failed);
    return unexpected(rp::StreamError::pipeline_failed);
  }
  waiter_.wake();
  return {};
}

auto ActiveStream::send_audio_state(rp::AudioStateKind kind,
                                    std::span<const std::byte> data) -> Result {
  auto* session = streaming_session();
  if (!session) return unexpected(rp::StreamError::invalid_state);
  return posted(*session, session->post_audio_state(kind, data));
}

auto ActiveStream::send_keyboard(const KeyboardState& state) -> Result {
  auto* session = streaming_session();
  if (!session || !delivery_) return unexpected(rp::StreamError::invalid_state);
  auto event =
      session->keyboard_event(state.modifiers(), state.leds(), state.keys());
  if (!event)
    return posted(*session, Expected<void, rp::ActiveSessionError>{
                                unexpected(event.error())});
  const auto pushed = delivery_->push_event(
      rp::ActiveSession::keyboard_controller_id, std::move(*event), now_us());
  if (!pushed &&
      pushed.error() == rp::ControllerDeliveryError::invalid_controller_state)
    return unexpected(rp::StreamError::invalid_state);
  return posted(*session, {});
}

auto ActiveStream::drop_video_frame() -> Result {
  auto* session = streaming_session();
  if (!session || !session->post_video_frame_drop())
    return unexpected(rp::StreamError::invalid_state);
  return {};
}

auto ActiveStream::request_idr() -> Result {
  auto* session = streaming_session();
  if (!session) return unexpected(rp::StreamError::invalid_state);
  if (const auto requested = session->post_idr_request(); !requested) {
    if (session->failure())
      return fail(rp::StreamError::pipeline_failed,
                  rp::StreamFailure::pipeline_failed);
    return unexpected(rp::StreamError::pipeline_failed);
  }
  waiter_.wake();
  return {};
}

auto ActiveStream::execute(const rp::StreamCommand& command) -> Result {
  if (std::holds_alternative<rp::command::SetPadCount>(command) ||
      std::holds_alternative<rp::command::CouchSetAccountId>(command))
    return couch_.execute(command, features_, nullptr, nullptr, 0);
  if (!running()) return unexpected(rp::StreamError::invalid_state);
  if (rp::CtrlFeatures::is_ctrl_command(command)) {
    if (!ctrl_open_) return unexpected(rp::StreamError::invalid_state);
    auto queued = features_.queue_command(command);
    if (queued) waiter_.wake();
    return queued;
  }
  auto done = couch_.execute(command, features_,
                             delivery_ && streaming() ? &*delivery_ : nullptr,
                             streaming_session(), now_us());
  if (!ctrl_open_) features_.clear_outbound();
  if (done) waiter_.wake();
  return done;
}

auto ActiveStream::wait(
    std::chrono::milliseconds timeout,
    std::initializer_list<const platform::Pollable*> watched) -> Result {
  auto* source = media_source();
  const auto due = source ? source->next_deadline_us() : std::nullopt;
  const auto paced = pacing(source);
  const auto wake_at =
      paced == RouteWaiter::Pacing::scheduled
          ? timing::Deadline::earliest(
                source->next_wake_us(),
                delivery_ ? delivery_->next_deadline_us() : std::nullopt)
          : due;
  const auto bounded =
      RouteWaiter::bound_to_deadline(timeout, wake_at, now_us());
  std::array<const platform::Pollable*, RouteWaiter::max_pollables> polled{};
  polled[0] = has_media() ? route_ : nullptr;
  std::size_t at = 1;
  for (const auto* pollable : watched)
    if (at < polled.size()) polled[at++] = pollable;
  const auto readable = waiter_.wait(
      bounded, {polled[0], polled[1], polled[2], polled[3]}, paced);
  readable_ = readable;
  if (readable) {
    const bool route_readable = (*readable & 1U) != 0;
    if (route_readable)
      if (auto* pipeline = this->pipeline())
        pipeline->mark_route_readable(now_us());
    if (lean_ && polled[0] && source)
      source->note_route_readiness(*polled[0], route_readable);
  }
  if (due && now_us() >= *due) static_cast<void>(source->pump_once());
  return {};
}

auto ActiveStream::take_watched_readable(std::size_t index) noexcept -> bool {
  if (!lean_ || !readable_) return true;
  const auto bit = std::uint32_t{1} << (index + 1);
  const bool found = (*readable_ & bit) != 0;
  *readable_ &= ~bit;
  return found;
}

auto ActiveStream::pacing(const rp::ActiveSession* source) const noexcept
    -> RouteWaiter::Pacing {
  if (lean_ && streaming() && source) return RouteWaiter::Pacing::scheduled;
  return running() ? RouteWaiter::Pacing::ticked : RouteWaiter::Pacing::idle;
}

auto ActiveStream::diagnostics() const noexcept -> rp::StreamDiagnostics {
  const auto* session = active();
  if (!session) return {};
  auto diagnostics = session->diagnostics();
  diagnostics.dropped_posts += deferred_refreshes_;
  if (delivery_) delivery_->report(diagnostics.input);
  return diagnostics;
}

auto ActiveStream::committed_takion_version() const noexcept
    -> std::optional<std::uint32_t> {
  if (const auto* held =
          std::get_if<std::unique_ptr<rp::LocalCalibratedPipeline>>(&media_))
    return (*held)->committed_takion_version();
  if (const auto* held =
          std::get_if<std::unique_ptr<rp::ActiveSession>>(&media_))
    return (*held)->committed_takion_version();
  return std::nullopt;
}

auto ActiveStream::media_loss() const noexcept
    -> const rp::ActiveSessionFailure* {
  if (transition_failure_) return nullptr;
  if (const auto* held =
          std::get_if<std::unique_ptr<rp::LocalCalibratedPipeline>>(&media_))
    return (*held)->active_failure() ? &*(*held)->active_failure() : nullptr;
  if (const auto* held =
          std::get_if<std::unique_ptr<rp::ActiveSession>>(&media_))
    return (*held)->failure() ? &*(*held)->failure() : nullptr;
  return nullptr;
}

auto ActiveStream::ended_by_console() -> bool {
  const auto* lost = media_loss();
  if (lost)
    LinkFailure::log_disconnect(
        platform_.supplies(platform::PlatformCapability::diagnostics)
            ? &platform_.diagnostics()
            : nullptr,
        *lost);
  deliver_media();
  lost = media_loss();
  if (!lost) return false;
  const auto ended = LinkFailure::peer_quit(*lost, trial_warned_);
  if (ended != rp::QuitReason::stream_connection_remote_shutdown &&
      ended != rp::QuitReason::stream_connection_time_limit_reached &&
      ended != rp::QuitReason::stream_connection_trial_ended)
    return false;
  ending_.record_quit(ended, *lost->peer_reason);
  return true;
}

auto ActiveStream::explained(rp::StreamError error,
                             std::optional<LinkFailure> detail) const
    -> LinkFailure {
  auto explained = detail ? std::move(*detail) : LinkFailure{stage_, error};
  if (error != rp::StreamError::pipeline_failed || !has_media())
    return explained;
  if (transition_failure_) return *transition_failure_;
  if (const auto* lost = media_loss()) return LinkFailure::lost(*lost);
  if (const auto* held =
          std::get_if<std::unique_ptr<rp::LocalCalibratedPipeline>>(&media_))
    if (const auto& calibration = (*held)->calibration_failure()) {
      LinkFailure calibrating{rp::FailureStage::calibration, explained.result(),
                              *calibration};
      if (explained.cancelled()) calibrating.cancel();
      return calibrating;
    }
  return explained;
}

auto ActiveStream::fail(rp::StreamError error, rp::StreamFailure reason,
                        std::optional<LinkFailure> detail) -> Result {
  if (error == rp::StreamError::pipeline_failed && ended_by_console())
    return owner_.stop();
  auto failure = explained(error, std::move(detail));
  if (reason == rp::StreamFailure::cancelled) failure.cancel();
  trace_.failure(failure);
  ending_.record_failure(std::move(failure));
  if (!owner_.release_route())
    ending_.record_cleanup_failure(LinkFailure{
        rp::FailureStage::shutdown, rp::StreamError::shutdown_failed});
  state_ = rp::StreamState::failed;
  if (ending_.cleanup_failure())
    trace_.failure(*ending_.cleanup_failure(), true);
  trace_.record(diagnostics::Operation::shutdown,
                ending_.cleanup_failure() ? diagnostics::Outcome::failed
                                          : diagnostics::Outcome::succeeded);
  emit(rp::event::Failed{reason});
  return unexpected(error);
}

auto ActiveStream::stopping() -> bool {
  trace_.record(diagnostics::Operation::stop_requested,
                diagnostics::Outcome::observed);
  return !ended();
}

auto ActiveStream::stopped(bool cleaned) -> Result {
  if (!cleaned) {
    state_ = rp::StreamState::failed;
    shutdown_failed_ = true;
    ending_.record_cleanup_failure(LinkFailure{
        rp::FailureStage::shutdown, rp::StreamError::shutdown_failed});
    if (ending_.cleanup_failure())
      trace_.failure(*ending_.cleanup_failure(), true);
    emit(rp::event::Failed{rp::StreamFailure::shutdown_failed});
    return unexpected(rp::StreamError::shutdown_failed);
  }
  state_ = rp::StreamState::stopped;
  trace_.record(diagnostics::Operation::shutdown,
                diagnostics::Outcome::succeeded);
  emit(ending_.stopped_event());
  return {};
}

auto ActiveStream::RouteWaiter::bound_to_deadline(
    std::chrono::nanoseconds timeout, std::optional<std::uint64_t> due_us,
    std::uint64_t now_us) noexcept -> std::chrono::nanoseconds {
  if (!due_us) return timeout;
  const auto remaining = std::chrono::microseconds{
      *due_us > now_us ? static_cast<std::int64_t>(*due_us - now_us) : 0};
  return std::min(timeout, std::chrono::nanoseconds{remaining});
}

ActiveStream::RouteWaiter::RouteWaiter(platform::NetworkProvider& network,
                                       platform::ThreadingProvider& threading,
                                       const platform::ClockProvider& clock,
                                       platform::StopToken stop)
    : stop_(std::move(stop)), sleeper_(threading, clock) {
  try {
    auto poller = network.pollers().create_poller();
    if (poller) poller_ = std::move(poller.value());
  } catch (...) {
    poller_.reset();
  }
}

void ActiveStream::RouteWaiter::wait(std::chrono::nanoseconds timeout,
                                     const platform::Pollable* route,
                                     bool establishing) {
  if (stop_.stop_requested() || timeout.count() <= 0) return;
  const auto bounded =
      route || establishing
          ? std::min(timeout, std::chrono::nanoseconds{protocol_tick})
          : timeout;
  if (!poller_) {
    static_cast<void>(sleeper_.sleep_for(
        std::min(bounded,
                 std::chrono::nanoseconds{std::chrono::milliseconds{1}}),
        stop_));
    return;
  }
  const std::array<platform::PollRequest, 1> requests{
      {{route, 0, platform::PollEvent::readable}}};
  const auto watched = route ? std::span<const platform::PollRequest>{requests}
                             : std::span<const platform::PollRequest>{};
  (void)poller_->wait(watched, bounded, stop_);
}

auto ActiveStream::RouteWaiter::wait(
    std::chrono::nanoseconds timeout,
    std::initializer_list<const platform::Pollable*> watched, Pacing pacing)
    -> std::optional<std::uint32_t> {
  if (stop_.stop_requested()) return std::nullopt;
  std::array<platform::PollRequest, max_pollables> requests{};
  std::size_t count = 0;
  std::size_t position = 0;
  for (const auto* pollable : watched) {
    const auto bit = std::uint64_t{1} << position++;
    if (!pollable || count == requests.size()) continue;
    const auto existing = std::find_if(
        requests.begin(), requests.begin() + static_cast<std::ptrdiff_t>(count),
        [pollable](const platform::PollRequest& request) {
          return request.pollable == pollable;
        });
    if (existing != requests.begin() + static_cast<std::ptrdiff_t>(count)) {
      existing->tag |= bit;
      continue;
    }
    requests[count] = {pollable, bit, platform::PollEvent::readable};
    ++count;
  }
  if (timeout.count() <= 0 && count == 0) return std::nullopt;
  const bool ticked =
      pacing == Pacing::ticked || (pacing == Pacing::idle && count != 0);
  const auto bounded =
      ticked ? std::min(timeout, std::chrono::nanoseconds{protocol_tick})
             : timeout;
  if (!poller_) {
    static_cast<void>(sleeper_.sleep_for(
        std::min(bounded,
                 std::chrono::nanoseconds{std::chrono::milliseconds{1}}),
        stop_));
    return std::nullopt;
  }
  const auto waited = poller_->wait(
      std::span<const platform::PollRequest>{requests.data(), count}, bounded,
      stop_);
  if (!waited) return std::nullopt;
  std::uint32_t readable = 0;
  for (const auto& readiness : waited.value().ready)
    if ((readiness.ready & platform::PollEvent::readable) !=
        platform::PollEvent::none)
      readable |= static_cast<std::uint32_t>(readiness.tag);
  return readable;
}

auto ActiveStream::RouteWaiter::wait(
    std::chrono::nanoseconds timeout,
    std::initializer_list<const platform::Pollable*> watched, bool active)
    -> std::uint32_t {
  return wait(timeout, watched, active ? Pacing::ticked : Pacing::idle)
      .value_or(0);
}

void ActiveStream::RouteWaiter::wake() noexcept {
  if (poller_) (void)poller_->wake();
}

}  // namespace libchiaking::play
