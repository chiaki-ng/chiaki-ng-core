#include "src/core/play/adapters/links/internet/wire/remote_ctrl_effects.hpp"

#include <utility>
#include "src/core/play/adapters/links/internet/wire/remote_ctrl.hpp"

#include <cstdint>
#include <variant>

namespace libchiaking::play::internet::remote_ctrl {

auto Effects::step(internet::remote_ctrl::Session& owner,
                   net::transport::rudp::HandshakeChannel& channel,
                   platform::CryptoProvider& crypto,
                   net::protocol::remote_play::RegistrationTarget target,
                   bool pin_notified) -> Expected<EffectProgress, EffectError> {
  namespace effect = net::protocol::remote_play::ctrl_lifecycle_detail;
  for (;;) {
    const auto* front = owner.inbox().front_effect();
    if (!front) return EffectProgress::idle;
    if (std::holds_alternative<effect::RequestPin>(*front)) {
      if (pin_notified) {
        owner.inbox().promote_heartbeat_replies();
        const auto* promoted = owner.inbox().front_effect();
        if (promoted &&
            std::holds_alternative<effect::SendHeartbeatReply>(*promoted))
          continue;
        return EffectProgress::idle;
      }
      owner.inbox().set_pin_prompt_open(true);
      owner.inbox().coalesce_pin_requests();
      return EffectProgress::pin_required;
    }
    if (std::holds_alternative<effect::PublishSessionIdFallback>(*front)) {
      (void)owner.inbox().take_effect();
      return EffectProgress::fallback_required;
    }
    if (std::holds_alternative<effect::SendHeartbeatReply>(*front)) {
      auto sent = owner.send_heartbeat_once(channel, crypto, target);
      if (!sent) return unexpected(sent.error());
      continue;
    }
    if (std::holds_alternative<effect::PublishSwitchAck>(*front)) {
      (void)owner.inbox().take_effect();
      continue;
    }
    if (std::holds_alternative<effect::PublishCtrlFailed>(*front)) {
      (void)owner.inbox().take_effect();
      return EffectProgress::ctrl_failed;
    }
    (void)owner.inbox().take_effect();
    return EffectProgress::failed;
  }
}

void Effects::drain_notices(
    internet::remote_ctrl::Session& owner,
    net::protocol::remote_play::CtrlFeatures& features,
    std::vector<net::protocol::remote_play::StreamEvent>& events,
    net::protocol::remote_play::CouchBackend* couch) {
  while (auto notice = owner.inbox().take_notice())
    features.on_notice(std::move(*notice), events, couch);
}

auto Effects::flush_outbound(
    internet::remote_ctrl::Session& owner,
    net::transport::rudp::HandshakeChannel& channel,
    platform::CryptoProvider& crypto,
    net::protocol::remote_play::RegistrationTarget target,
    net::protocol::remote_play::CtrlFeatures& features, bool enable_dualsense,
    bool enable_keyboard)
    -> Expected<void, internet::remote_ctrl::OutboundStepError> {
  if (!owner.post_id_complete() ||
      owner.inbox().state() !=
          net::protocol::remote_play::ctrl_lifecycle_detail::State::active)
    return {};
  if (auto repeated = owner.repeat_post_id_features(
          channel, crypto, target, enable_dualsense, enable_keyboard);
      !repeated)
    return repeated;
  while (const auto* frame = features.front_outbound()) {
    auto sent = owner.send_ctrl_frame(channel, crypto, target, frame->type,
                                      frame->payload);
    if (!sent) return sent;
    features.pop_outbound();
  }
  return {};
}

}  // namespace libchiaking::play::internet::remote_ctrl
