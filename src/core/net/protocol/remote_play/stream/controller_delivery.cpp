#include "src/core/net/protocol/remote_play/stream/controller_delivery.hpp"

#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"

#include <algorithm>
#include <cstddef>

#include <cmath>

namespace libchiaking::net::protocol::remote_play {

auto ControllerSample::validate(const ControllerState& state)
    -> Expected<void, ControllerDeliveryError> {
  if (!feedback_state(state) || !check_history(state)) {
    return unexpected(ControllerDeliveryError::invalid_controller_state);
  }
  return {};
}

auto ControllerSample::check_history(const ControllerState& state) noexcept
    -> Expected<void, ControllerHistoryError> {
  if (state.controller_id >= Pad::count) {
    return unexpected(ControllerHistoryError::invalid_controller_id);
  }
  if ((state.buttons & ~controller_button_mask) != 0) {
    return unexpected(ControllerHistoryError::invalid_button_mask);
  }
  if (std::ranges::any_of(state.touches, [](const ControllerTouch& touch) {
        return touch.id < -1;
      })) {
    return unexpected(ControllerHistoryError::invalid_touch_id);
  }
  return {};
}

auto ControllerSample::sanitize_motion(ControllerState state) noexcept
    -> ControllerState {
  for (auto* gyro : {&state.gyro_x, &state.gyro_y, &state.gyro_z})
    if (!std::isfinite(*gyro)) *gyro = 0.0F;
  if (!std::isfinite(state.accel_x) || !std::isfinite(state.accel_y) ||
      !std::isfinite(state.accel_z)) {
    state.accel_x = 0.0F;
    state.accel_y = 1.0F;
    state.accel_z = 0.0F;
  }
  if (!std::isfinite(state.orient_x) || !std::isfinite(state.orient_y) ||
      !std::isfinite(state.orient_z) || !std::isfinite(state.orient_w)) {
    state.orient_x = 0.0F;
    state.orient_y = 0.0F;
    state.orient_z = 0.0F;
    state.orient_w = 1.0F;
  }
  return state;
}

void FeedbackStateSchedule::reset() noexcept {
  last_sent_.reset();
  last_sent_us_ = 0;
}

auto FeedbackStateSchedule::relevant_equal(
    const ControllerState& left, const ControllerState& right) const noexcept
    -> bool {
  return ControllerSample::same_state_values(left, right,
                                             policy_.motion_tolerance);
}

auto ControllerSample::same_state_values(const ControllerState& left,
                                         const ControllerState& right,
                                         float motion_tolerance) noexcept
    -> bool {
  if (left.left_x != right.left_x || left.left_y != right.left_y ||
      left.right_x != right.right_x || left.right_y != right.right_y)
    return false;
  const auto near = [motion_tolerance](float a, float b) noexcept {
    if (!std::isfinite(a) || !std::isfinite(b)) return false;
    return !(a < b - motion_tolerance || a > b + motion_tolerance);
  };
  return near(left.gyro_x, right.gyro_x) && near(left.gyro_y, right.gyro_y) &&
         near(left.gyro_z, right.gyro_z) && near(left.accel_x, right.accel_x) &&
         near(left.accel_y, right.accel_y) &&
         near(left.accel_z, right.accel_z) &&
         near(left.orient_x, right.orient_x) &&
         near(left.orient_y, right.orient_y) &&
         near(left.orient_z, right.orient_z) &&
         near(left.orient_w, right.orient_w) &&
         left.trigger_states == right.trigger_states;
}

auto ControllerSample::same_feedback(const ControllerState& left,
                                     const ControllerState& right,
                                     float motion_tolerance) noexcept -> bool {
  const auto same_touches = std::ranges::equal(
      left.touches, right.touches,
      [](const ControllerTouch& a, const ControllerTouch& b) {
        return a.id == b.id && a.x == b.x && a.y == b.y;
      });
  return left.controller_id == right.controller_id &&
         left.buttons == right.buttons && left.l2 == right.l2 &&
         left.r2 == right.r2 && same_touches &&
         same_state_values(left, right, motion_tolerance);
}

auto FeedbackStateSchedule::should_send(const ControllerState& state,
                                        std::uint64_t now_us) const noexcept
    -> bool {
  if (!last_sent_ || now_us < last_sent_us_) return true;
  if (!relevant_equal(state, *last_sent_)) return true;
  return now_us - last_sent_us_ >= policy_.state_refresh_interval_us;
}

void FeedbackStateSchedule::mark_sent(const ControllerState& state,
                                      std::uint64_t now_us) noexcept {
  last_sent_ = state;
  last_sent_us_ = now_us;
}

auto ControllerSample::feedback_state(const ControllerState& state)
    -> Expected<takion::FeedbackStateValues, ControllerFeedbackError> {
  if (state.controller_id >= Pad::count)
    return unexpected(ControllerFeedbackError::invalid_controller_id);
  const auto triggers =
      state.trigger_states.value_or(std::array<std::uint8_t, 2>{});
  return takion::FeedbackStateValues{
      .controller_id = state.controller_id,
      .gyro_x = state.gyro_x,
      .gyro_y = state.gyro_y,
      .gyro_z = state.gyro_z,
      .accel_x = state.accel_x,
      .accel_y = state.accel_y,
      .accel_z = state.accel_z,
      .orient_x = state.orient_x,
      .orient_y = state.orient_y,
      .orient_z = state.orient_z,
      .orient_w = state.orient_w,
      .left_x = state.left_x,
      .left_y = state.left_y,
      .right_x = state.right_x,
      .right_y = state.right_y,
      .left_trigger_state = triggers[0],
      .right_trigger_state = triggers[1],
      .trigger_states_emulated = !state.trigger_states.has_value(),
  };
}

auto ControllerSample::history_edges(const ControllerState& previous,
                                     const ControllerState& current)
    -> Expected<FeedbackHistoryDelta, ControllerHistoryError> {
  if (previous.controller_id >= Pad::count ||
      current.controller_id >= Pad::count) {
    return unexpected(ControllerHistoryError::invalid_controller_id);
  }
  if (previous.controller_id != current.controller_id) {
    return unexpected(ControllerHistoryError::controller_id_mismatch);
  }
  if (const auto checked = check_history(previous); !checked) {
    return unexpected(checked.error());
  }
  if (const auto checked = check_history(current); !checked) {
    return unexpected(checked.error());
  }

  FeedbackHistoryDelta events;
  for (std::size_t i = 0; i < previous.touches.size(); ++i) {
    const auto& before = previous.touches[i];
    const auto& after = current.touches[i];
    if (before.id != after.id && before.id >= 0) {
      events.push(takion::FeedbackTouchEvent{
          false, static_cast<std::uint8_t>(before.id), before.x, before.y});
    } else if (after.id >= 0 && (before.id != after.id || before.x != after.x ||
                                 before.y != after.y)) {
      events.push(takion::FeedbackTouchEvent{
          true, static_cast<std::uint8_t>(after.id), after.x, after.y});
    }
  }
  for (unsigned bit = 0; bit < controller_button_count; ++bit) {
    const auto mask = std::uint32_t{1} << bit;
    const bool was = (previous.buttons & mask) != 0;
    const bool now = (current.buttons & mask) != 0;
    if (was != now) {
      events.push(takion::FeedbackDigitalButtonEvent{
          static_cast<takion::FeedbackDigitalButton>(bit), now});
    }
  }
  if (previous.l2 != current.l2) {
    events.push(takion::FeedbackAnalogButtonEvent{
        takion::FeedbackAnalogButton::l2, current.l2});
  }
  if (previous.r2 != current.r2) {
    events.push(takion::FeedbackAnalogButtonEvent{
        takion::FeedbackAnalogButton::r2, current.r2});
  }
  return events;
}

auto ControllerSample::history_delta(const ControllerState& previous,
                                     const ControllerState& current)
    -> Expected<std::vector<takion::FeedbackHistoryEvent>,
                ControllerHistoryError> {
  const auto delta = history_edges(previous, current);
  if (!delta) {
    return unexpected(delta.error());
  }
  const auto events = delta->events();
  return std::vector<takion::FeedbackHistoryEvent>(events.begin(),
                                                   events.end());
}

}  // namespace libchiaking::net::protocol::remote_play
