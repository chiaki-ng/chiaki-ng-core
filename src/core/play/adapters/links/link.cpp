#include "src/core/play/adapters/links/link.hpp"

#include "src/core/play/adapters/links/active_stream.hpp"

#include <algorithm>

namespace libchiaking::play {

void LinkEnding::record_failure(LinkFailure failure) noexcept {
  if (venue_) failure.reached_through(*venue_);
  const auto quit = failure.quit();
  record_quit(quit.reason, quit.text);
  if (!failure_) failure_ = std::move(failure);
}

void LinkEnding::record_cleanup_failure(LinkFailure failure) noexcept {
  if (venue_) failure.reached_through(*venue_);
  if (!cleanup_failure_) cleanup_failure_ = std::move(failure);
}

void LinkEnding::record_quit(net::protocol::remote_play::QuitReason reason,
                             std::string_view text) noexcept {
  if (quit_.reason != net::protocol::remote_play::QuitReason::none ||
      reason == net::protocol::remote_play::QuitReason::none)
    return;
  quit_.reason = reason;
  quit_.text.assign(
      text.substr(0, net::protocol::remote_play::quit_text_limit));
}

auto LinkEnding::stopped_event() noexcept
    -> net::protocol::remote_play::event::StateChanged {
  record_quit(net::protocol::remote_play::QuitReason::stopped);
  return net::protocol::remote_play::event::StateChanged{
      net::protocol::remote_play::StreamState::stopped, quit_.reason,
      quit_.text};
}

auto Link::wait(std::chrono::milliseconds timeout) -> Result {
  if (auto* stream = active_stream()) return stream->wait(timeout, {});
  return {};
}

void Link::wake() noexcept {
  if (auto* stream = active_stream()) stream->wake();
}

auto Link::select_datacenter(std::string_view) -> Result {
  return unexpected(net::protocol::remote_play::StreamError::invalid_state);
}

auto Link::state() const noexcept -> net::protocol::remote_play::StreamState {
  const auto* stream = active_stream();
  return stream ? stream->state()
                : net::protocol::remote_play::StreamState::idle;
}

auto Link::send_controller_state(
    const net::protocol::remote_play::ControllerState& state) -> Result {
  auto* stream = active_stream();
  return stream ? stream->send_controller_state(state) : refused();
}

auto Link::report_video_loss(std::uint16_t first, std::uint16_t last)
    -> Result {
  auto* stream = active_stream();
  return stream ? stream->report_video_loss(first, last) : refused();
}

auto Link::execute(const net::protocol::remote_play::StreamCommand& command)
    -> Result {
  auto* stream = active_stream();
  return stream ? stream->execute(command) : refused();
}

auto Link::send_create(CloudMessaging::CapturePress) -> Result {
  return unexpected(
      net::protocol::remote_play::StreamError::invalid_configuration);
}

auto Link::send_deep_link(std::string text) -> Result {
  auto* stream = active_stream();
  return stream ? stream->send_deep_link(std::move(text)) : refused();
}

auto Link::send_invitation(std::string invitation_id, std::string session_id)
    -> Result {
  auto* stream = active_stream();
  return stream ? stream->send_invitation(std::move(invitation_id),
                                          std::move(session_id))
                : refused();
}

auto Link::send_microphone(std::span<const std::byte> opus_frame) -> Result {
  auto* stream = active_stream();
  return stream ? stream->send_microphone(opus_frame) : refused();
}

auto Link::connect_microphone() -> Result { return refused(); }

auto Link::toggle_system_menu(std::uint32_t) -> Result { return refused(); }

auto Link::open_saved_data() -> Result { return refused(); }

auto Link::set_microphone_muted(bool) -> Result { return refused(); }

auto Link::drop_video_frame() -> Result {
  auto* stream = active_stream();
  return stream ? stream->drop_video_frame() : refused();
}

auto Link::request_idr() -> Result {
  auto* stream = active_stream();
  return stream ? stream->request_idr() : refused();
}

auto Link::send_audio_state(net::protocol::remote_play::AudioStateKind kind,
                            std::span<const std::byte> body) -> Result {
  auto* stream = active_stream();
  return stream ? stream->send_audio_state(kind, body) : refused();
}

auto Link::send_keyboard(const KeyboardState& state) -> Result {
  auto* stream = active_stream();
  return stream ? stream->send_keyboard(state) : refused();
}

void Link::set_event_sink(LinkEventSink& sink) noexcept {
  if (auto* stream = active_stream()) stream->set_sink(sink);
}

auto Link::diagnostics() const noexcept
    -> net::protocol::remote_play::StreamDiagnostics {
  const auto* stream = active_stream();
  return stream ? stream->diagnostics()
                : net::protocol::remote_play::StreamDiagnostics{};
}

auto Link::takion_version() const noexcept -> std::optional<std::uint32_t> {
  const auto* stream = active_stream();
  return stream ? stream->committed_takion_version() : std::nullopt;
}

auto Link::refused() -> Result {
  return unexpected(net::protocol::remote_play::StreamError::invalid_state);
}

}  // namespace libchiaking::play
