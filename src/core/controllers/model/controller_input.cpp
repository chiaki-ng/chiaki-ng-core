#include "libchiaking/detail/play/controllers.hpp"

#include <algorithm>

namespace libchiaking::controllers {

auto ControllerInput::outside(float value, float reference) noexcept -> bool {
  return value < reference - motion_epsilon ||
         value > reference + motion_epsilon;
}

auto ControllerInput::larger_magnitude(std::int16_t a, std::int16_t b) noexcept
    -> std::int16_t {
  const int absolute_a = a > 0 ? a : -a;
  const int absolute_b = b > 0 ? b : -b;
  return absolute_a > absolute_b ? a : b;
}

auto ControllerInput::holds(const ControllerTouch& touch,
                            std::uint8_t id) noexcept -> bool {
  return static_cast<int>(touch.id) == static_cast<int>(id);
}

auto ControllerInput::in_motion(const ControllerState& state) noexcept -> bool {
  return outside(state.gyro_x, 0.0F) || outside(state.gyro_y, 0.0F) ||
         outside(state.gyro_z, 0.0F) || outside(state.accel_x, 0.0F) ||
         outside(state.accel_y, 1.0F) || outside(state.accel_z, 0.0F);
}

ControllerInput::ControllerInput(const ControllerState& state,
                                 std::uint8_t next_touch_id) noexcept
    : state_(state), next_touch_id_(next_touch_id) {}

void ControllerInput::set_idle() noexcept {
  state_ = ControllerState{};
  next_touch_id_ = 0;
}

auto ControllerInput::start_touch(std::uint16_t x, std::uint16_t y) noexcept
    -> std::optional<std::uint8_t> {
  const auto slot = std::ranges::find_if(
      state_.touches,
      [](const ControllerTouch& touch) { return touch.id < 0; });
  if (slot == state_.touches.end()) return std::nullopt;
  const auto id = next_touch_id_;
  *slot = ControllerTouch{static_cast<std::int8_t>(id), x, y};
  next_touch_id_ = static_cast<std::uint8_t>((id + 1) & touch_id_mask);
  return id;
}

void ControllerInput::stop_touch(std::uint8_t id) noexcept {
  const auto slot = std::ranges::find_if(
      state_.touches,
      [id](const ControllerTouch& touch) { return holds(touch, id); });
  if (slot != state_.touches.end()) slot->id = -1;
}

void ControllerInput::move_touch(std::uint8_t id, std::uint16_t x,
                                 std::uint16_t y) noexcept {
  const auto masked = static_cast<std::uint8_t>(id & touch_id_mask);
  const auto slot = std::ranges::find_if(
      state_.touches,
      [masked](const ControllerTouch& touch) { return holds(touch, masked); });
  if (slot == state_.touches.end()) return;
  slot->x = x;
  slot->y = y;
}

auto ControllerInput::matches(const ControllerInput& other) const noexcept
    -> bool {
  const auto& a = state_;
  const auto& b = other.state_;
  if (!(a.buttons == b.buttons && a.l2 == b.l2 && a.r2 == b.r2 &&
        a.left_x == b.left_x && a.left_y == b.left_y &&
        a.right_x == b.right_x && a.right_y == b.right_y))
    return false;
  const auto same_touch = [](const ControllerTouch& left,
                             const ControllerTouch& right) {
    return left.id == right.id &&
           (left.id < 0 || (left.x == right.x && left.y == right.y));
  };
  if (!std::ranges::equal(a.touches, b.touches, same_touch) ||
      a.trigger_states != b.trigger_states)
    return false;
  return !(outside(a.gyro_x, b.gyro_x) || outside(a.gyro_y, b.gyro_y) ||
           outside(a.gyro_z, b.gyro_z) || outside(a.accel_x, b.accel_x) ||
           outside(a.accel_y, b.accel_y) || outside(a.accel_z, b.accel_z) ||
           outside(a.orient_x, b.orient_x) || outside(a.orient_y, b.orient_y) ||
           outside(a.orient_z, b.orient_z) || outside(a.orient_w, b.orient_w));
}

auto ControllerInput::identical(const ControllerInput& other) const noexcept
    -> bool {
  const auto& a = state_;
  const auto& b = other.state_;
  const auto same_touch = [](const ControllerTouch& left,
                             const ControllerTouch& right) {
    return left.id == right.id && left.x == right.x && left.y == right.y;
  };
  return a.buttons == b.buttons && a.l2 == b.l2 && a.r2 == b.r2 &&
         a.left_x == b.left_x && a.left_y == b.left_y &&
         a.right_x == b.right_x && a.right_y == b.right_y &&
         std::ranges::equal(a.touches, b.touches, same_touch) &&
         a.gyro_x == b.gyro_x && a.gyro_y == b.gyro_y && a.gyro_z == b.gyro_z &&
         a.accel_x == b.accel_x && a.accel_y == b.accel_y &&
         a.accel_z == b.accel_z && a.orient_x == b.orient_x &&
         a.orient_y == b.orient_y && a.orient_z == b.orient_z &&
         a.orient_w == b.orient_w && a.trigger_states == b.trigger_states;
}

auto ControllerInput::merged(const ControllerInput& other) const noexcept
    -> ControllerInput {
  const auto& a = state_;
  const auto& b = other.state_;
  ControllerState result;
  result.buttons = a.buttons | b.buttons;
  result.l2 = std::max(a.l2, b.l2);
  result.r2 = std::max(a.r2, b.r2);
  result.left_x = larger_magnitude(a.left_x, b.left_x);
  result.left_y = larger_magnitude(a.left_y, b.left_y);
  result.right_x = larger_magnitude(a.right_x, b.right_x);
  result.right_y = larger_magnitude(a.right_y, b.right_y);
  std::ranges::transform(
      a.touches, b.touches, result.touches.begin(),
      [](const ControllerTouch& first, const ControllerTouch& second) {
        return first.id >= 0    ? first
               : second.id >= 0 ? second
                                : ControllerTouch{};
      });
  const auto& motion = in_motion(a) ? a : b;
  result.gyro_x = motion.gyro_x;
  result.gyro_y = motion.gyro_y;
  result.gyro_z = motion.gyro_z;
  result.accel_x = motion.accel_x;
  result.accel_y = motion.accel_y;
  result.accel_z = motion.accel_z;
  result.orient_x = motion.orient_x;
  result.orient_y = motion.orient_y;
  result.orient_z = motion.orient_z;
  result.orient_w = motion.orient_w;
  result.trigger_states =
      a.trigger_states ? a.trigger_states : b.trigger_states;
  return ControllerInput{result};
}

}  // namespace libchiaking::controllers
