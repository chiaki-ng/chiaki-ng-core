/// @file
/// @brief Declares motion fusion for a controller: sensor readings, the
/// accelerometer zero, OrientationTracker and MotionTracker.
#pragma once

#include "libchiaking/detail/controllers/controller_state.hpp"

#include <chrono>
#include <cstdint>

namespace libchiaking::controllers {

/// @brief A three-axis sensor reading.
struct MotionVector {
  /// @brief Value along the x axis.
  float x{};
  /// @brief Value along the y axis.
  float y{};
  /// @brief Value along the z axis.
  float z{};
};

/// @brief An orientation as a unit quaternion; the default is no rotation.
struct Quaternion {
  /// @brief The x component.
  float x{};
  /// @brief The y component.
  float y{};
  /// @brief The z component.
  float z{};
  /// @brief The w component.
  float w{1.0F};
};

/// @brief An offset subtracted from accelerometer readings to define what
/// counts as the resting pose.
class AccelZero {
 public:
  /// @brief Constructs a zero offset, which leaves readings unchanged.
  AccelZero() noexcept = default;
  /// @brief Constructs an offset with the given value.
  ///
  /// @param offset The value subtracted from each reading.
  explicit AccelZero(MotionVector offset) noexcept : offset_(offset) {}

  /// @brief Makes the offset used before any resting pose has been captured.
  ///
  /// @param real_accel True for an offset that stands for a raw resting
  ///     reading, with 1 g on the y axis; false for an offset that removes
  ///     nothing.
  /// @return The offset.
  static auto inactive(bool real_accel) noexcept -> AccelZero;
  /// @brief Makes an offset from a reading taken in the pose that should
  /// count as resting.
  ///
  /// @param accel The accelerometer reading in that pose, in g.
  /// @param real_accel True to keep the reading whole. False to leave 1 g on
  ///     the y axis out of the offset, so that a corrected reading in that
  ///     pose shows gravity alone.
  /// @return The offset.
  static auto active(MotionVector accel, bool real_accel) noexcept -> AccelZero;

  /// @brief The value subtracted from each reading.
  ///
  /// @return The value subtracted from each reading.
  auto offset() const noexcept -> MotionVector { return offset_; }
  /// @brief Applies the offset to a reading.
  ///
  /// @param accel A raw accelerometer reading, in g.
  /// @return The reading with the offset subtracted.
  auto removed_from(MotionVector accel) const noexcept -> MotionVector;

 private:
  MotionVector offset_{};
};

/// @brief Estimates a controller's orientation by fusing gyroscope and
/// accelerometer samples.
///
/// Feed it samples with update and copy the result into a ControllerState with
/// apply. Most frontends use MotionTracker, which wraps this class and takes
/// the two sensors separately.
class OrientationTracker {
 public:
  /// @brief Number of first samples during which the estimate follows the
  /// accelerometer strongly, so that it settles quickly.
  static constexpr std::uint64_t warmup_samples = 30;

  /// @brief The tracker's whole state, which can be saved and restored.
  struct Snapshot {
    /// @brief The last gyroscope reading, in radians per second.
    MotionVector gyro{};
    /// @brief The last accelerometer reading, in g.
    MotionVector accel{0.0F, 1.0F, 0.0F};
    /// @brief The estimated orientation, before the fixed rotation that
    /// apply adds.
    Quaternion orientation{static_cast<float>(0.7071067811865475), 0.0F, 0.0F,
                           static_cast<float>(0.7071067811865476)};
    /// @brief Timestamp of the last sample, in microseconds, modulo 2^32.
    std::uint32_t timestamp_us{};
    /// @brief Number of samples taken so far.
    std::uint64_t sample_index{};
  };

  /// @brief Constructs a tracker that has seen no samples.
  OrientationTracker() noexcept = default;
  /// @brief Constructs a tracker that continues from a saved state.
  ///
  /// @param snapshot A value read earlier from snapshot().
  explicit OrientationTracker(const Snapshot& snapshot) noexcept
      : value_(snapshot) {}

  /// @brief The tracker's current state.
  ///
  /// @return The tracker's current state.
  auto snapshot() const noexcept -> const Snapshot& { return value_; }

  /// @brief Takes one motion sample and advances the estimate.
  ///
  /// The first sample only sets the time reference; the estimate moves from
  /// the second sample on.
  ///
  /// @param gyro Angular velocity, in radians per second.
  /// @param accel Acceleration, in g.
  /// @param timestamp_us Time of the sample in microseconds. Only differences
  ///     matter, and the value may wrap at 2^32.
  void update(MotionVector gyro, MotionVector accel,
              std::uint32_t timestamp_us) noexcept;
  /// @brief Takes one motion sample after removing an accelerometer offset.
  ///
  /// @param gyro Angular velocity, in radians per second.
  /// @param accel Raw acceleration, in g.
  /// @param zero The offset to subtract from accel first.
  /// @param timestamp_us Time of the sample in microseconds; it may wrap at
  ///     2^32.
  void update(MotionVector gyro, MotionVector accel, const AccelZero& zero,
              std::uint32_t timestamp_us) noexcept;
  /// @brief Writes the last readings and the estimated orientation into a
  /// controller state.
  ///
  /// The orientation is written rotated a quarter turn about the x axis from
  /// the tracker's own estimate. Only the motion fields of the state are
  /// changed.
  ///
  /// @param state The state to update.
  void apply(ControllerState& state) const noexcept;
  /// @brief Turns the accelerometer's pull of the estimate towards gravity on
  /// or off once the first samples have settled it; on by default.
  ///
  /// @param enabled Whether to keep correcting tilt.
  void correct_tilt(bool enabled) noexcept { tilt_correction_ = enabled; }

 private:
  static constexpr double sin_negative_quarter_pi = -0.7071067811865475;
  static constexpr double cos_negative_quarter_pi = 0.7071067811865476;
  static constexpr float beta_warmup = 20.0f;
  static constexpr float beta_default = 0.05f;
  static constexpr float orient_fuzz = 0.0007f;
  static constexpr float fuzz_previous_weight = 0.75f;
  static constexpr float fuzz_previous_weight_double = 0.6f;
  static constexpr std::uint64_t motion_timestamp_wrap = 1ULL << 32;

  static auto inverse_sqrt(float x) noexcept -> float;
  static auto fuzzed(float current, float previous) noexcept -> float;

  void integrate(float beta, float time_step_sec) noexcept;

  Snapshot value_{};
  bool tilt_correction_{true};
};

/// @brief Turns raw accelerometer and gyroscope samples from a device into the
/// motion fields of a ControllerState.
///
/// Feed each sensor's samples as they arrive, then call apply on the state
/// about to be sent. recentre makes the controller's current pose the resting
/// pose.
///
/// The tracker is not thread-safe; use it from one thread or guard it.
class MotionTracker {
 public:
  /// @brief Constructs a tracker that has seen no samples.
  MotionTracker() noexcept = default;

  /// @brief Takes an accelerometer sample.
  ///
  /// A sample with a non-finite component is ignored.
  ///
  /// @param sample Acceleration in g.
  /// @param timestamp Time of the sample on any steady clock; only its low 32
  ///     bits of microseconds are used.
  void accelerometer(MotionVector sample,
                     std::chrono::microseconds timestamp) noexcept;
  /// @brief Takes a gyroscope sample.
  ///
  /// A sample with a non-finite component is ignored. While the dead band is
  /// on, an axis turning slower than one degree per second counts as still,
  /// so a resting controller does not drift.
  ///
  /// @param sample Angular velocity in radians per second.
  /// @param timestamp Time of the sample on the same clock as the
  ///     accelerometer's.
  void gyroscope(MotionVector sample,
                 std::chrono::microseconds timestamp) noexcept;
  /// @brief Makes the current pose the resting pose and restarts the
  /// orientation estimate from it.
  ///
  /// Does nothing before the first sample.
  void recentre() noexcept;
  /// @brief Turns tilt correction on or off, as the console asks; on by
  /// default.
  ///
  /// With it off, the orientation follows the gyroscope alone once the first
  /// samples have settled it. The setting outlasts recentre.
  ///
  /// @param enabled Whether the estimate is pulled towards gravity.
  void set_tilt_correction(bool enabled) noexcept;
  /// @brief Turns the one degree per second gyroscope dead band on or off,
  /// as the console asks; on by default.
  ///
  /// @param enabled Whether slow turns count as still.
  void set_angular_velocity_deadband(bool enabled) noexcept;

  /// @brief Reports whether any valid sample has been taken.
  ///
  /// @return `true` when any valid sample has been taken.
  auto sampled() const noexcept -> bool { return sampled_; }
  /// @brief Writes the tracked motion into a controller state.
  ///
  /// Only the gyroscope, accelerometer and orientation fields are changed.
  /// Does nothing before the first sample, which leaves the state's own
  /// motion values in place.
  ///
  /// @param state The state to update.
  void apply(ControllerState& state) const noexcept;

 private:
  static auto finite(MotionVector value) noexcept -> bool;

  void publish(std::uint32_t timestamp_us) noexcept;

  OrientationTracker tracker_{};
  AccelZero accel_zero_{AccelZero::inactive(false)};
  AccelZero real_accel_{AccelZero::inactive(true)};
  ControllerState motion_{};
  std::uint32_t last_timestamp_us_{};
  bool sampled_{};
  bool tilt_correction_{true};
  bool angular_velocity_deadband_{true};
};

}  // namespace libchiaking::controllers
