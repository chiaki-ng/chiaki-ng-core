#include "libchiaking/detail/controllers/motion.hpp"
#include "libchiaking/detail/play/controllers.hpp"

#include "libchiaking/platform/runtime/time_units.hpp"

#include <cmath>

namespace libchiaking::controllers {

namespace {

constexpr float angular_velocity_deadband = 0.017453292F;

auto dead_banded(MotionVector value) noexcept -> MotionVector {
  const auto band = [](float axis) noexcept {
    return std::fabs(axis) < angular_velocity_deadband ? 0.0F : axis;
  };
  return {band(value.x), band(value.y), band(value.z)};
}

}  // namespace

auto OrientationTracker::inverse_sqrt(float x) noexcept -> float {
  return static_cast<float>(1.0f / std::sqrt(static_cast<double>(x)));
}

auto MotionTracker::finite(MotionVector value) noexcept -> bool {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

auto OrientationTracker::fuzzed(const float current,
                                const float previous) noexcept -> float {
  const float amount = orient_fuzz;
  const float weight = fuzz_previous_weight;
  const float weight_double = fuzz_previous_weight_double;
  if ((current < previous + amount / static_cast<float>(2)) &&
      (current > previous - amount / static_cast<float>(2)))
    return previous;
  if ((current < previous + amount) && (current > previous - amount))
    return (weight * previous + (1 - weight) * current);
  if ((current < previous + amount * 2) && (current > previous - amount * 2))
    return (weight_double * previous + (1 - weight_double) * current);
  return current;
}

auto AccelZero::inactive(bool real_accel) noexcept -> AccelZero {
  return AccelZero{MotionVector{0.0f, real_accel ? 1.0f : 0.0f, 0.0f}};
}

auto AccelZero::active(MotionVector accel, bool real_accel) noexcept
    -> AccelZero {
  return AccelZero{
      MotionVector{accel.x, real_accel ? accel.y : accel.y - 1.0f, accel.z}};
}

auto AccelZero::removed_from(MotionVector accel) const noexcept
    -> MotionVector {
  accel.x -= offset_.x;
  accel.y -= offset_.y;
  accel.z -= offset_.z;
  return accel;
}

void OrientationTracker::update(MotionVector gyro, MotionVector accel,
                                const AccelZero& zero,
                                std::uint32_t timestamp_us) noexcept {
  update(gyro, zero.removed_from(accel), timestamp_us);
}

void OrientationTracker::update(MotionVector gyro, MotionVector accel,
                                std::uint32_t timestamp_us) noexcept {
  value_.gyro = gyro;
  value_.accel = accel;
  value_.sample_index++;
  if (value_.sample_index <= 1) {
    value_.timestamp_us = timestamp_us;
    return;
  }
  std::uint64_t delta_us = timestamp_us;
  if (delta_us < value_.timestamp_us) delta_us += motion_timestamp_wrap;
  delta_us -= value_.timestamp_us;
  value_.timestamp_us = timestamp_us;
  const float settled_beta = tilt_correction_ ? beta_default : 0.0f;
  integrate(value_.sample_index < warmup_samples ? beta_warmup : settled_beta,
            static_cast<float>(delta_us) /
                static_cast<float>(microseconds_per_second));
}

void OrientationTracker::integrate(float beta, float time_step_sec) noexcept {
  const float gx = value_.gyro.x, gy = value_.gyro.y, gz = value_.gyro.z;
  float ax = value_.accel.x, ay = value_.accel.y, az = value_.accel.z;
  auto& orient = value_.orientation;
  float q0 = orient.w, q1 = orient.x, q2 = orient.y, q3 = orient.z;
  float recip_norm;
  float s0, s1, s2, s3;
  float q_dot1, q_dot2, q_dot3, q_dot4;
  float _2q0, _2q1, _2q2, _2q3, _4q0, _4q1, _4q2, _8q1, _8q2, q0q0, q1q1, q2q2,
      q3q3;

  q_dot1 = 0.5f * (-q1 * gx - q2 * gy - q3 * gz);
  q_dot2 = 0.5f * (q0 * gx + q2 * gz - q3 * gy);
  q_dot3 = 0.5f * (q0 * gy - q1 * gz + q3 * gx);
  q_dot4 = 0.5f * (q0 * gz + q1 * gy - q2 * gx);

  if (!((ax == 0.0f) && (ay == 0.0f) && (az == 0.0f))) {
    recip_norm = inverse_sqrt(ax * ax + ay * ay + az * az);
    ax *= recip_norm;
    ay *= recip_norm;
    az *= recip_norm;

    _2q0 = 2.0f * q0;
    _2q1 = 2.0f * q1;
    _2q2 = 2.0f * q2;
    _2q3 = 2.0f * q3;
    _4q0 = 4.0f * q0;
    _4q1 = 4.0f * q1;
    _4q2 = 4.0f * q2;
    _8q1 = 8.0f * q1;
    _8q2 = 8.0f * q2;
    q0q0 = q0 * q0;
    q1q1 = q1 * q1;
    q2q2 = q2 * q2;
    q3q3 = q3 * q3;

    s0 = _4q0 * q2q2 + _2q2 * ax + _4q0 * q1q1 - _2q1 * ay;
    s1 = _4q1 * q3q3 - _2q3 * ax + 4.0f * q0q0 * q1 - _2q0 * ay - _4q1 +
         _8q1 * q1q1 + _8q1 * q2q2 + _4q1 * az;
    s2 = 4.0f * q0q0 * q2 + _2q0 * ax + _4q2 * q3q3 - _2q3 * ay - _4q2 +
         _8q2 * q1q1 + _8q2 * q2q2 + _4q2 * az;
    s3 = 4.0f * q1q1 * q3 - _2q1 * ax + 4.0f * q2q2 * q3 - _2q2 * ay;
    recip_norm = s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3;
    if (recip_norm > 0.000001f) {
      recip_norm = inverse_sqrt(recip_norm);
      s0 *= recip_norm;
      s1 *= recip_norm;
      s2 *= recip_norm;
      s3 *= recip_norm;

      q_dot1 -= beta * s0;
      q_dot2 -= beta * s1;
      q_dot3 -= beta * s2;
      q_dot4 -= beta * s3;
    }
  }

  q0 += q_dot1 * time_step_sec;
  q1 += q_dot2 * time_step_sec;
  q2 += q_dot3 * time_step_sec;
  q3 += q_dot4 * time_step_sec;

  recip_norm = inverse_sqrt(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
  q0 *= recip_norm;
  q1 *= recip_norm;
  q2 *= recip_norm;
  q3 *= recip_norm;

  orient.w = fuzzed(q0, orient.w);
  orient.x = fuzzed(q1, orient.x);
  orient.y = fuzzed(q2, orient.y);
  orient.z = fuzzed(q3, orient.z);
}

void OrientationTracker::apply(ControllerState& state) const noexcept {
  const auto& orient = value_.orientation;
  state.gyro_x = value_.gyro.x;
  state.gyro_y = value_.gyro.y;
  state.gyro_z = value_.gyro.z;
  state.accel_x = value_.accel.x;
  state.accel_y = value_.accel.y;
  state.accel_z = value_.accel.z;
  state.orient_w = static_cast<float>(cos_negative_quarter_pi * orient.w -
                                      sin_negative_quarter_pi * orient.x);
  state.orient_x = static_cast<float>(cos_negative_quarter_pi * orient.x +
                                      sin_negative_quarter_pi * orient.w);
  state.orient_y = static_cast<float>(cos_negative_quarter_pi * orient.y -
                                      sin_negative_quarter_pi * orient.z);
  state.orient_z = static_cast<float>(cos_negative_quarter_pi * orient.z +
                                      sin_negative_quarter_pi * orient.y);
}

void MotionTracker::accelerometer(
    MotionVector sample, std::chrono::microseconds timestamp) noexcept {
  if (!finite(sample)) return;
  const auto timestamp_us = static_cast<std::uint32_t>(timestamp.count());
  real_accel_ = AccelZero::active(sample, true);
  tracker_.update({motion_.gyro_x, motion_.gyro_y, motion_.gyro_z}, sample,
                  accel_zero_, timestamp_us);
  publish(timestamp_us);
}

void MotionTracker::gyroscope(MotionVector sample,
                              std::chrono::microseconds timestamp) noexcept {
  if (!finite(sample)) return;
  const auto timestamp_us = static_cast<std::uint32_t>(timestamp.count());
  tracker_.update(angular_velocity_deadband_ ? dead_banded(sample) : sample,
                  {motion_.accel_x, motion_.accel_y, motion_.accel_z},
                  timestamp_us);
  publish(timestamp_us);
}

void MotionTracker::recentre() noexcept {
  if (!sampled_) return;
  const auto resting = real_accel_.offset();
  accel_zero_ = AccelZero::active(resting, false);
  tracker_ = OrientationTracker{};
  tracker_.correct_tilt(tilt_correction_);
  tracker_.update({motion_.gyro_x, motion_.gyro_y, motion_.gyro_z}, resting,
                  accel_zero_, last_timestamp_us_);
  tracker_.apply(motion_);
}

void MotionTracker::set_tilt_correction(bool enabled) noexcept {
  tilt_correction_ = enabled;
  tracker_.correct_tilt(enabled);
}

void MotionTracker::set_angular_velocity_deadband(bool enabled) noexcept {
  angular_velocity_deadband_ = enabled;
}

void MotionTracker::apply(ControllerState& state) const noexcept {
  if (!sampled_) return;
  state.gyro_x = motion_.gyro_x;
  state.gyro_y = motion_.gyro_y;
  state.gyro_z = motion_.gyro_z;
  state.accel_x = motion_.accel_x;
  state.accel_y = motion_.accel_y;
  state.accel_z = motion_.accel_z;
  state.orient_x = motion_.orient_x;
  state.orient_y = motion_.orient_y;
  state.orient_z = motion_.orient_z;
  state.orient_w = motion_.orient_w;
}

void MotionTracker::publish(std::uint32_t timestamp_us) noexcept {
  tracker_.apply(motion_);
  last_timestamp_us_ = timestamp_us;
  sampled_ = true;
}

}  // namespace libchiaking::controllers
