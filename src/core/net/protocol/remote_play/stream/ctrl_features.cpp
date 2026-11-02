#include "src/core/net/protocol/remote_play/stream/ctrl_features.hpp"

#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"

#include <variant>

namespace libchiaking::net::protocol::remote_play {

CtrlFeatures::CtrlFeatures(platform::DiagnosticsProvider* diagnostics) noexcept
    : diagnostics_(diagnostics) {}

auto CtrlFeatures::is_ctrl_command(const StreamCommand& command) noexcept
    -> bool {
  return std::holds_alternative<command::GotoBed>(command) ||
         std::holds_alternative<command::GoHome>(command) ||
         std::holds_alternative<command::KeyboardSetText>(command) ||
         std::holds_alternative<command::KeyboardAccept>(command) ||
         std::holds_alternative<command::KeyboardReject>(command);
}

auto CtrlFeatures::queue_command(const StreamCommand& command)
    -> Expected<void, StreamError> {
  if (const auto* text = std::get_if<command::KeyboardSetText>(&command)) {
    if (outbound_.size() >= outbound_capacity)
      return unexpected(StreamError::invalid_state);
    auto frame = CtrlCodec::encode_keyboard_set_text(keyboard_counter_ + 1U,
                                                     text->text.view());
    if (!frame) return unexpected(StreamError::invalid_argument);
    ++keyboard_counter_;
    return queue(std::move(frame.value()));
  }
  if (std::holds_alternative<command::GotoBed>(command))
    return queue(CtrlCodec::encode_goto_bed());
  if (std::holds_alternative<command::GoHome>(command)) {
    log(platform::LogSeverity::verbose,
        "Ctrl sending go to home screen message");
    return queue(CtrlCodec::encode_go_home());
  }
  if (std::holds_alternative<command::KeyboardAccept>(command))
    return queue(CtrlCodec::encode_keyboard_accept());
  if (std::holds_alternative<command::KeyboardReject>(command))
    return queue(CtrlCodec::encode_keyboard_reject());
  return unexpected(StreamError::invalid_state);
}

auto CtrlFeatures::queue_frame(CtrlOutboundFrame frame)
    -> Expected<void, StreamError> {
  return queue(std::move(frame));
}

auto CtrlFeatures::can_queue(std::size_t frames) const noexcept -> bool {
  return outbound_capacity - outbound_.size() >= frames;
}

auto CtrlFeatures::front_outbound() const noexcept -> const CtrlOutboundFrame* {
  return outbound_.empty() ? nullptr : &outbound_.front();
}

void CtrlFeatures::pop_outbound() noexcept {
  if (!outbound_.empty()) outbound_.pop_front();
}

void CtrlFeatures::clear_outbound() noexcept { outbound_.clear(); }

void CtrlFeatures::on_rp_prohibit(bool prohibited,
                                  std::vector<StreamEvent>& events) {
  if (prohibited) events.emplace_back(event::DisplayProhibited{true});
}

void CtrlFeatures::on_notice(CtrlNotice notice,
                             std::vector<StreamEvent>& events,
                             CouchBackend* couch) {
  couch_ = couch;
  std::visit([this, &events](auto& value) { handle(value, events); }, notice);
  couch_ = nullptr;
}

auto CtrlFeatures::queue(CtrlOutboundFrame frame)
    -> Expected<void, StreamError> {
  if (outbound_.size() >= outbound_capacity)
    return unexpected(StreamError::invalid_state);
  outbound_.push_back(std::move(frame));
  return {};
}

void CtrlFeatures::handle(CtrlKeyboardOpen& value,
                          std::vector<StreamEvent>& events) {
  events.emplace_back(event::KeyboardOpen{std::move(value.text)});
}

void CtrlFeatures::handle(CtrlKeyboardTextChange& value,
                          std::vector<StreamEvent>& events) {
  events.emplace_back(event::KeyboardTextChange{std::move(value.text)});
}

void CtrlFeatures::handle(CtrlKeyboardRemoteClose&,
                          std::vector<StreamEvent>& events) {
  events.emplace_back(event::KeyboardRemoteClose{});
}

void CtrlFeatures::handle(CtrlDisplayA& value,
                          std::vector<StreamEvent>& events) {
  if (value.state == ctrl_display_a_prohibited) {
    cant_display_a_ = true;
  } else if (value.state == ctrl_display_a_allowed && !cant_display_b_) {
    cant_display_a_ = false;
    log(platform::LogSeverity::info,
        "Ctrl received message that the stream can now display.");
    events.emplace_back(event::DisplayProhibited{false});
  }
}

void CtrlFeatures::handle(CtrlDisplayB& value,
                          std::vector<StreamEvent>& events) {
  const bool released = value.first == ctrl_display_b_released_first &&
                        value.second == ctrl_display_b_released_second;
  if (cant_display_a_ && !released && !cant_display_b_) {
    events.emplace_back(event::DisplayProhibited{true});
    log(platform::LogSeverity::info,
        "Ctrl received message that the stream can't display due to displaying "
        "some content that can't be streamed.");
    cant_display_b_ = true;
  }
  if (cant_display_b_ && released) cant_display_b_ = false;
}

void CtrlFeatures::handle(CtrlCouchAccountUnresolvable& value,
                          std::vector<StreamEvent>& events) {
  logf(platform::LogSeverity::warning,
       "Couch: console cannot resolve the account joined on pad {} (ctrl "
       "0x{:04X}): {:.64}",
       static_cast<unsigned>(value.pad),
       static_cast<unsigned>(CtrlMessageType::couch_user_code),
       std::string_view{value.printable.c_str()});
  forward(std::move(value), events);
}

void CtrlFeatures::handle(CtrlCouchUserJoinReply& value,
                          std::vector<StreamEvent>& events) {
  logf(platform::LogSeverity::info,
       "Couch: user join reply (0x{:04X}) for pad {}, status {}",
       static_cast<unsigned>(CtrlMessageType::couch_user_join_rep),
       static_cast<unsigned>(value.pad), static_cast<unsigned>(value.status));
  forward(value, events);
}

void CtrlFeatures::handle(CtrlCouchPasscodeRequest& value,
                          std::vector<StreamEvent>& events) {
  logf(platform::LogSeverity::info,
       "Couch: console asked for pad {}'s console login passcode (ctrl "
       "0x{:04X})",
       static_cast<unsigned>(value.pad),
       static_cast<unsigned>(CtrlMessageType::couch_passcode_req));
  forward(value, events);
}

void CtrlFeatures::handle(CtrlCouchPadJoinReply& value,
                          std::vector<StreamEvent>& events) {
  logf(platform::LogSeverity::info,
       "Couch: pad join reply (0x{:04X}) for pad {}, result {}",
       static_cast<unsigned>(CtrlMessageType::couch_pad_join_rep),
       static_cast<unsigned>(value.pad), static_cast<unsigned>(value.result));
  forward(value, events);
}

void CtrlFeatures::handle(CtrlCouchPadDropped& value,
                          std::vector<StreamEvent>& events) {
  logf(platform::LogSeverity::info,
       "Couch: console dropped pad {} (ctrl 0x{:04X})",
       static_cast<unsigned>(value.pad),
       static_cast<unsigned>(CtrlMessageType::couch_pad_dropped));
  forward(value, events);
}

void CtrlFeatures::handle(CtrlCouchLeaveReply& value,
                          std::vector<StreamEvent>&) {
  logf(platform::LogSeverity::info, "Couch: leave reply for pad {}, result {}",
       static_cast<unsigned>(value.pad), static_cast<unsigned>(value.result));
}

void CtrlFeatures::handle(CtrlMalformedNotice& value,
                          std::vector<StreamEvent>&) {
  logf(platform::LogSeverity::warning,
       "Ctrl received malformed message type {:#x} with payload size {}",
       static_cast<unsigned>(value.type), value.size);
}

void CtrlFeatures::log(platform::LogSeverity severity,
                       std::string_view message) noexcept {
  if (diagnostics_) diagnostics_->write({severity, message});
}

}  // namespace libchiaking::net::protocol::remote_play
