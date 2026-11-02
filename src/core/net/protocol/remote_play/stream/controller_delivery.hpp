/// @file
/// @brief Declares ControllerDelivery, which paces up to four controllers'
/// state and history to the console.
///
/// Takion carries controller input two ways. A feedback state packet holds
/// the continuous values (sticks, motion, trigger states) and is sent when
/// they change or every 200 ms. A feedback history packet holds the discrete
/// edges (buttons, triggers, touches) newest first, so a lost packet is
/// recovered by the next one; each window is resent twice 20 ms apart with
/// the same sequence number, which the console ignores unless it lost the
/// original.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "src/core/net/protocol/remote_play/stream/controller.hpp"
#include "src/core/net/protocol/remote_play/stream/diagnostics.hpp"
#include "src/core/net/protocol/remote_play/stream/limits.hpp"
#include "src/core/net/protocol/remote_play/stream/local_settings.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <rigtorp/SPSCQueue.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace libchiaking::net::protocol::remote_play {

/// @brief How controller feedback is paced.
///
/// Plain data.
struct FeedbackDeliveryPolicy {
  /// @brief How often an unchanged state is resent, in microseconds.
  std::uint64_t state_refresh_interval_us{default_state_refresh_interval_us};
  /// @brief How far a motion value may move before the state is resent.
  float motion_tolerance{default_motion_tolerance};
  /// @brief How many recent events every history window repeats.
  std::uint8_t recent_history_events{default_recent_history_events};
  /// @brief How many times a window is resent after its first send.
  std::uint8_t history_repeats{default_history_repeats};
  /// @brief How long between resends, in microseconds.
  std::uint64_t history_repeat_interval_us{default_history_repeat_interval_us};
  /// @brief Whether every edge gets its own history window.
  bool per_edge_flush{default_per_edge_flush};

  /// @brief The pacing for the Switch client.
  ///
  /// @return The defaults.
  static constexpr auto switch_console() noexcept -> FeedbackDeliveryPolicy {
    return FeedbackDeliveryPolicy{};
  }

  /// @brief The pacing for a feedback style.
  ///
  /// @param mode The style; currently ignored.
  /// @return The defaults.
  static constexpr auto for_mode(FeedbackMode mode) noexcept
      -> FeedbackDeliveryPolicy {
    static_cast<void>(mode);
    return FeedbackDeliveryPolicy{};
  }
};

/// @brief Decides when one controller's state packet is due.
///
/// Not thread-safe.
class FeedbackStateSchedule {
 public:
  /// @brief Makes a schedule that has sent nothing.
  ///
  /// @param policy The pacing.
  explicit FeedbackStateSchedule(FeedbackDeliveryPolicy policy = {}) noexcept
      : policy_(policy) {}

  /// @brief Whether a state should be sent now.
  ///
  /// @param state The current state.
  /// @param now_us Monotonic microseconds now.
  /// @return True when nothing was sent yet, the clock went back, the sticks,
  ///     motion or trigger states changed, or the refresh interval passed.
  auto should_send(const ControllerState& state,
                   std::uint64_t now_us) const noexcept -> bool;
  /// @brief Records a sent state.
  ///
  /// @param state The state sent.
  /// @param now_us Monotonic microseconds when it was sent.
  void mark_sent(const ControllerState& state, std::uint64_t now_us) noexcept;
  /// @brief Reads when an unchanged state is next due for its refresh.
  ///
  /// @return Monotonic microseconds; nothing before anything was sent.
  auto refresh_due_us() const noexcept -> std::optional<std::uint64_t> {
    if (!last_sent_) return std::nullopt;
    return timing::Deadline::after_us(last_sent_us_,
                                      policy_.state_refresh_interval_us);
  }
  /// @brief Forgets what was sent.
  void reset() noexcept;

 private:
  /// @brief Whether two states agree on what a state packet carries, within
  /// the motion tolerance.
  ///
  /// @param left One state.
  /// @param right The other.
  /// @return True when equal enough not to resend.
  auto relevant_equal(const ControllerState& left,
                      const ControllerState& right) const noexcept -> bool;

  /// @brief The pacing.
  const FeedbackDeliveryPolicy policy_;
  /// @brief The state last sent.
  std::optional<ControllerState> last_sent_;
  /// @brief When it was sent, in monotonic microseconds.
  std::uint64_t last_sent_us_{};
};

/// @brief Why a controller state could not be delivered.
enum class ControllerDeliveryError : std::uint8_t {
  /// @brief The state is out of range (id, buttons or touches).
  invalid_controller_state,
  /// @brief The session refused a packet.
  post_failed,
};

/// @brief Why a controller state could not be converted.
enum class ControllerFeedbackError : std::uint8_t {
  /// @brief The controller id is 4 or more.
  invalid_controller_id,
};

/// @brief Why a history delta could not be computed.
enum class ControllerHistoryError : std::uint8_t {
  /// @brief A controller id is 4 or more.
  invalid_controller_id,
  /// @brief The two states belong to different controllers.
  controller_id_mismatch,
  /// @brief A touch id is below -1.
  invalid_touch_id,
  /// @brief A button bit outside controller_button_mask is set.
  invalid_button_mask,
};

/// @brief The feedback history events between two controller states, held
/// in place.
///
/// Holds at most one event per touch, button, L2 and R2, so computing a
/// delta never allocates. Invariant: size() never exceeds capacity.
class FeedbackHistoryDelta {
 public:
  /// @brief The most events one delta holds: one per touch, per button, and
  /// for each of L2 and R2.
  static constexpr std::size_t capacity =
      std::tuple_size_v<decltype(ControllerState::touches)> +
      controller_button_count + 2;

  /// @brief Appends an event; one past capacity is dropped.
  ///
  /// @param event The event.
  void push(const takion::FeedbackHistoryEvent& event) {
    if (size_ == capacity) {
      return;
    }
    events_[size_] = event;
    ++size_;
  }

  /// @brief Reads the events in order.
  ///
  /// @return The events, oldest first.
  auto events() const noexcept
      -> std::span<const takion::FeedbackHistoryEvent> {
    return std::span{events_}.first(size_);
  }

  /// @brief Counts the events.
  ///
  /// @return The count.
  auto size() const noexcept -> std::size_t { return size_; }

  /// @brief Whether nothing changed.
  ///
  /// @return True when there are no events.
  auto empty() const noexcept -> bool { return size_ == 0; }

 private:
  /// @brief The events; those past size_ are unused.
  std::array<takion::FeedbackHistoryEvent, capacity> events_{};
  /// @brief How many events are held.
  std::size_t size_{};
};

/// @brief Checks and cleans up controller samples, and converts them into
/// the Takion feedback they are sent as.
///
/// Stateless; every member is static.
class ControllerSample {
 public:
  /// @brief Converts a controller state into the values of a feedback state
  /// packet: motion, sticks and, when known, the adaptive trigger states.
  ///
  /// Absent trigger states are sent as zero and marked emulated.
  ///
  /// @param state The controller state.
  /// @return The packet values.
  /// @retval ControllerFeedbackError::invalid_controller_id The id is out of
  ///     range.
  [[nodiscard]] static auto feedback_state(const ControllerState& state)
      -> Expected<takion::FeedbackStateValues, ControllerFeedbackError>;

  /// @brief The feedback history events that turn one state into the next,
  /// held in place without allocating.
  ///
  /// Touches come first (a lifted or replaced finger is an up event with its
  /// last position, a new or moved finger a down event), then every changed
  /// button bit in bit order, then L2 and R2 when their pressure changed.
  ///
  /// @param previous The state last sent.
  /// @param current The new state.
  /// @return The events; empty when nothing changed.
  /// @retval ControllerHistoryError::invalid_controller_id An id is out of
  ///     range.
  /// @retval ControllerHistoryError::controller_id_mismatch The ids differ.
  /// @retval ControllerHistoryError::invalid_button_mask An unknown button
  ///     bit is set.
  /// @retval ControllerHistoryError::invalid_touch_id A touch id is below -1.
  [[nodiscard]] static auto history_edges(const ControllerState& previous,
                                          const ControllerState& current)
      -> Expected<FeedbackHistoryDelta, ControllerHistoryError>;

  /// @brief The feedback history events that turn one state into the next,
  /// copied into a vector for a caller that keeps them.
  ///
  /// @param previous The state last sent.
  /// @param current The new state.
  /// @return The events of history_edges(); empty when nothing changed.
  /// @retval ControllerHistoryError As history_edges().
  [[nodiscard]] static auto history_delta(const ControllerState& previous,
                                          const ControllerState& current)
      -> Expected<std::vector<takion::FeedbackHistoryEvent>,
                  ControllerHistoryError>;

  /// @brief Whether two states agree on what a feedback state packet carries:
  /// the sticks and trigger states exactly, and motion within a tolerance.
  ///
  /// @param left One state.
  /// @param right The other.
  /// @param motion_tolerance How far a motion value may differ.
  /// @return True when no state packet is needed to go from one to the other;
  ///     false when a motion value is not finite.
  static auto same_state_values(const ControllerState& left,
                                const ControllerState& right,
                                float motion_tolerance) noexcept -> bool;

  /// @brief Whether two states send the same feedback: no button, trigger or
  /// touch differs, so there is no history to send, and same_state_values()
  /// holds.
  ///
  /// @param left One state.
  /// @param right The other.
  /// @param motion_tolerance How far a motion value may differ.
  /// @return True when going from one to the other sends nothing new.
  static auto same_feedback(const ControllerState& left,
                            const ControllerState& right,
                            float motion_tolerance) noexcept -> bool;

  /// @brief Checks that a state can be sent, without allocating.
  ///
  /// @param state The state.
  /// @return Nothing, or ControllerDeliveryError::invalid_controller_state.
  [[nodiscard]] static auto validate(const ControllerState& state)
      -> Expected<void, ControllerDeliveryError>;

  /// @brief Replaces non-finite motion: gyro with 0, acceleration with +1 g on
  /// y, orientation with identity.
  ///
  /// @param state The state.
  /// @return The cleaned state.
  static auto sanitize_motion(ControllerState state) noexcept
      -> ControllerState;

 private:
  /// @brief Checks that one state's id, buttons and touch ids are in range.
  ///
  /// @param state The state.
  /// @return Nothing on success.
  /// @retval ControllerHistoryError::invalid_controller_id The id is out of
  ///     range.
  /// @retval ControllerHistoryError::invalid_button_mask An unknown button
  ///     bit is set.
  /// @retval ControllerHistoryError::invalid_touch_id A touch id is below -1.
  [[nodiscard]] static auto check_history(const ControllerState& state) noexcept
      -> Expected<void, ControllerHistoryError>;
};

/// @brief Feeds up to four controllers' samples to the console as feedback
/// state and history.
///
/// Seat 0 is active from construction; couch seats are activated when they
/// join. Buttons and triggers held when a seat is activated are latched off
/// until released, so a press that started before the join is not seen. Not
/// thread-safe: the caller serialises calls.
///
/// History windows are built in fixed storage and queued in a ring allocated
/// once at construction, so a sample, an edge and a resend never allocate in
/// the delivery itself.
///
/// @tparam Sink The session, with `post_controller_state(state)` and
///     `post_controller_history_window(events, controller_ids, fresh)`
///     taking two equally long spans, newest event first, and the count of
///     new events, 0 marking a resend, and returning something testable as
///     bool.
template <class Sink>
class ControllerDelivery {
 public:
  /// @brief Starts delivery with seat 0 active and an idle state counted as
  /// sent.
  ///
  /// Allocates the window ring once.
  ///
  /// @param sink The session; must outlive the delivery.
  /// @param start_us Monotonic microseconds now.
  /// @param policy The pacing.
  ControllerDelivery(Sink& sink, std::uint64_t start_us,
                     FeedbackDeliveryPolicy policy = {})
      : sink_(sink),
        policy_(policy),
        schedules_{FeedbackStateSchedule{policy}, FeedbackStateSchedule{policy},
                   FeedbackStateSchedule{policy},
                   FeedbackStateSchedule{policy}},
        outbox_(outbox_capacity) {
    for (std::uint8_t id = 0; id < history_previous_.size(); ++id) {
      history_previous_[id].controller_id = id;
    }
    latest_[0] = ControllerState{};
    schedules_[0].mark_sent(*latest_[0], start_us);
  }

  /// @brief Not copyable; the window ring is one per delivery.
  ControllerDelivery(const ControllerDelivery&) = delete;
  /// @brief Not copy-assignable; the window ring is one per delivery.
  auto operator=(const ControllerDelivery&) -> ControllerDelivery& = delete;

  /// @brief Takes one sample: sends its edges as history and its state when
  /// due, then drains pending history.
  ///
  /// Samples of inactive seats are ignored. Allocates nothing itself; what
  /// the sink does with a posted window is its own.
  ///
  /// @param sample The sample.
  /// @param now_us Monotonic microseconds now.
  /// @return Nothing on success.
  /// @retval ControllerDeliveryError::invalid_controller_state The sample is
  ///     out of range.
  [[nodiscard]] auto submit(const ControllerState& sample, std::uint64_t now_us)
      -> Expected<void, ControllerDeliveryError> {
    auto state = ControllerSample::sanitize_motion(sample);
    if (!ControllerSample::validate(state)) {
      return unexpected(ControllerDeliveryError::invalid_controller_state);
    }
    const auto id = state.controller_id;
    if (!active_[id]) {
      ++ignored_samples_;
      return {};
    }
    apply_latch(id, state);
    if (policy_.per_edge_flush) {
      if (auto flushed = flush_edges(id, state); !flushed) {
        return flushed;
      }
    } else {
      const auto delta =
          ControllerSample::history_edges(history_previous_[id], state);
      if (!delta) {
        return unexpected(ControllerDeliveryError::invalid_controller_state);
      }
      if (!delta->empty()) {
        flush_history(id, delta->events());
      }
    }
    history_previous_[id] = state;
    latest_[id] = state;
    auto& schedule = schedules_[id];
    if (schedule.should_send(state, now_us) &&
        sink_.post_controller_state(state)) {
      schedule.mark_sent(state, now_us);
    }
    drain_history(now_us);
    return {};
  }

  /// @brief Whether a seat is active.
  ///
  /// @param id The seat.
  /// @return True when active.
  auto is_active(std::uint8_t id) const noexcept -> bool {
    return id < active_.size() && active_[id];
  }

  /// @brief Activates a seat, latching the buttons and triggers it already
  /// holds.
  ///
  /// @param id The seat, 0 to 3.
  /// @param held The seat's state at activation.
  /// @param now_us Monotonic microseconds now.
  /// @return Nothing; nothing happens when already active.
  /// @retval ControllerDeliveryError::invalid_controller_state The seat is out
  ///     of range.
  [[nodiscard]] auto activate(std::uint8_t id, const ControllerState& held,
                              std::uint64_t now_us)
      -> Expected<void, ControllerDeliveryError> {
    if (id >= active_.size()) {
      return unexpected(ControllerDeliveryError::invalid_controller_state);
    }
    if (active_[id]) {
      return {};
    }
    reset_pad(id);
    active_[id] = true;
    latched_buttons_[id] = held.buttons;
    latched_l2_[id] = held.l2 != 0;
    latched_r2_[id] = held.r2 != 0;
    latest_[id] = history_previous_[id];
    schedules_[id].mark_sent(history_previous_[id], now_us);
    return {};
  }

  /// @brief Deactivates a seat and forgets its state.
  ///
  /// @param id The seat, 0 to 3.
  /// @return Nothing on success.
  /// @retval ControllerDeliveryError::invalid_controller_state The seat is out
  ///     of range.
  [[nodiscard]] auto deactivate(std::uint8_t id)
      -> Expected<void, ControllerDeliveryError> {
    if (id >= active_.size()) {
      return unexpected(ControllerDeliveryError::invalid_controller_state);
    }
    reset_pad(id);
    return {};
  }

  /// @brief Sends a seat's presence as a history event at once.
  ///
  /// @param id The seat, 0 to 3.
  /// @param present Whether its controller is present.
  /// @param now_us Monotonic microseconds now.
  /// @return Nothing on success.
  /// @retval ControllerDeliveryError::invalid_controller_state The seat is out
  ///     of range.
  /// @retval ControllerDeliveryError::post_failed The session refused it.
  [[nodiscard]] auto push_presence(std::uint8_t id, bool present,
                                   std::uint64_t now_us)
      -> Expected<void, ControllerDeliveryError> {
    return push_event(
        id,
        takion::FeedbackHistoryEvent{takion::FeedbackPresenceEvent{present}},
        now_us);
  }

  /// @brief Sends one history event of a seat at once, in its own window
  /// with the same numbering and resends as button edges.
  ///
  /// @param id The seat, 0 to 3.
  /// @param event The event, such as a keyboard record.
  /// @param now_us Monotonic microseconds now.
  /// @return Nothing on success.
  /// @retval ControllerDeliveryError::invalid_controller_state The seat is out
  ///     of range.
  /// @retval ControllerDeliveryError::post_failed The session refused the
  ///     window; it stays queued and goes with the next refresh or sample.
  [[nodiscard]] auto push_event(std::uint8_t id,
                                const takion::FeedbackHistoryEvent& event,
                                std::uint64_t now_us)
      -> Expected<void, ControllerDeliveryError> {
    if (id >= active_.size()) {
      return unexpected(ControllerDeliveryError::invalid_controller_state);
    }
    flush_history(id, std::span{&event, 1});
    if (!drain_history(now_us)) {
      return unexpected(ControllerDeliveryError::post_failed);
    }
    return {};
  }

  /// @brief Reads when refresh() next has a timed send to make: the next
  /// history resend or the earliest state refresh of a seat with a state.
  ///
  /// History a refused post left queued is not counted; it goes with the
  /// next refresh or sample.
  ///
  /// @return Monotonic microseconds; nothing when nothing is scheduled.
  auto next_deadline_us() const noexcept -> std::optional<std::uint64_t> {
    std::optional<std::uint64_t> next;
    if (repeats_left_ != 0 && !recent_history_.empty()) {
      next = timing::Deadline::after_us(last_history_sent_us_,
                                        policy_.history_repeat_interval_us);
    }
    for (std::size_t id = 0; id < schedules_.size(); ++id) {
      if (latest_[id]) {
        next =
            timing::Deadline::earliest(next, schedules_[id].refresh_due_us());
      }
    }
    return next;
  }

  /// @brief Sends what is due without a new sample: pending history, else a
  /// history resend, else one seat's overdue state (seats in turn).
  ///
  /// @param now_us Monotonic microseconds now.
  /// @return Nothing on success.
  /// @retval ControllerDeliveryError::post_failed The session refused a
  ///     packet.
  [[nodiscard]] auto refresh(std::uint64_t now_us)
      -> Expected<void, ControllerDeliveryError> {
    if (!drain_history(now_us)) {
      return unexpected(ControllerDeliveryError::post_failed);
    }
    if (repeats_left_ && !recent_history_.empty() &&
        now_us >= last_history_sent_us_ &&
        now_us - last_history_sent_us_ >= policy_.history_repeat_interval_us) {
      if (!post_window(recent_history_, 0)) {
        return unexpected(ControllerDeliveryError::post_failed);
      }
      ++resends_;
      --repeats_left_;
      last_history_sent_us_ = now_us;
      return {};
    }
    for (std::uint8_t offset = 0; offset < schedules_.size(); ++offset) {
      const auto id = static_cast<std::uint8_t>((next_refresh_ + offset) %
                                                schedules_.size());
      if (!latest_[id] || !schedules_[id].should_send(*latest_[id], now_us)) {
        continue;
      }
      if (!sink_.post_controller_state(*latest_[id])) {
        return unexpected(ControllerDeliveryError::post_failed);
      }
      schedules_[id].mark_sent(*latest_[id], now_us);
      next_refresh_ = static_cast<std::uint8_t>((id + 1U) % schedules_.size());
      return {};
    }
    return {};
  }

  /// @brief Writes the delivery's counters and seat state into a stream's
  /// input diagnostics.
  ///
  /// @param input The diagnostics; the session's own fields are left alone.
  void report(StreamInputDiagnostics& input) const noexcept {
    input.ignored_samples = ignored_samples_;
    input.history_events = history_events_;
    input.windows_queued = windows_queued_;
    input.windows_shed = windows_shed_;
    input.windows_waiting = outbox_.size();
    input.resends = resends_;
    input.active_seats = 0;
    for (std::size_t id = 0; id < active_.size(); ++id) {
      if (active_[id]) {
        input.active_seats |= static_cast<std::uint8_t>(1U << id);
      }
      input.held_buttons[id] = history_previous_[id].buttons;
      input.latched_buttons[id] = latched_buttons_[id];
    }
  }

 private:
  /// @brief The most events in a window.
  static constexpr std::size_t history_capacity = feedback_history_capacity;
  /// @brief The most queued windows.
  static constexpr std::size_t outbox_capacity = queue_capacity;

  /// @brief One history window, newest event first, with each event's seat,
  /// held in place.
  ///
  /// Invariant: holds at most history_capacity events, and its events and
  /// seats are equally long.
  class Window {
   public:
    /// @brief Makes an empty window.
    Window() = default;

    /// @brief Makes the window that puts new events of one seat before a
    /// recent window, cut to history_capacity.
    ///
    /// @param recent The window the new events go before.
    /// @param id The seat of the new events.
    /// @param oldest_first The new events, oldest first.
    Window(const Window& recent, std::uint8_t id,
           std::span<const takion::FeedbackHistoryEvent> oldest_first) {
      for (auto event = oldest_first.rbegin();
           event != oldest_first.rend() && size_ < history_capacity; ++event) {
        append(id, *event);
      }
      for (std::size_t index = 0;
           index < recent.size_ && size_ < history_capacity; ++index) {
        append(recent.controller_ids_[index], recent.events_[index]);
      }
    }

    /// @brief Makes a copy holding only the newest events.
    ///
    /// @param count The most events kept.
    /// @return The newest @p count events, or all when fewer.
    auto newest(std::size_t count) const -> Window {
      Window kept;
      for (std::size_t index = 0; index < std::min(count, size_); ++index) {
        kept.append(controller_ids_[index], events_[index]);
      }
      return kept;
    }

    /// @brief Reads the events.
    ///
    /// @return The events, newest first.
    auto events() const noexcept
        -> std::span<const takion::FeedbackHistoryEvent> {
      return std::span{events_}.first(size_);
    }

    /// @brief Reads each event's seat.
    ///
    /// @return The seats, in the order of events().
    auto controller_ids() const noexcept -> std::span<const std::uint8_t> {
      return std::span{controller_ids_}.first(size_);
    }

    /// @brief Whether the window holds nothing.
    ///
    /// @return True when empty.
    auto empty() const noexcept -> bool { return size_ == 0; }

   private:
    /// @brief Adds one event at the old end.
    ///
    /// @param id The event's seat.
    /// @param event The event.
    void append(std::uint8_t id, const takion::FeedbackHistoryEvent& event) {
      events_[size_] = event;
      controller_ids_[size_] = id;
      ++size_;
    }

    /// @brief The events, newest first; those past size_ are unused.
    std::array<takion::FeedbackHistoryEvent, history_capacity> events_{};
    /// @brief Each event's seat.
    std::array<std::uint8_t, history_capacity> controller_ids_{};
    /// @brief How many events are held.
    std::size_t size_{};
  };
  /// @brief A window waiting to be posted.
  ///
  /// Plain data.
  struct Pending {
    /// @brief The window.
    Window window;
    /// @brief How many of its events no posted window carried yet.
    std::size_t fresh{};
  };

  /// @brief Deactivates a seat and clears everything about it.
  ///
  /// @param id The seat.
  void reset_pad(std::uint8_t id) noexcept {
    active_[id] = false;
    schedules_[id].reset();
    history_previous_[id] = ControllerState{};
    history_previous_[id].controller_id = id;
    latest_[id].reset();
    latched_buttons_[id] = 0;
    latched_l2_[id] = false;
    latched_r2_[id] = false;
  }

  /// @brief Hides latched buttons and triggers until they are released.
  ///
  /// @param id The seat.
  /// @param state The sample, changed in place.
  void apply_latch(std::uint8_t id, ControllerState& state) noexcept {
    latched_buttons_[id] &= state.buttons;
    state.buttons &= ~latched_buttons_[id];
    if (latched_l2_[id]) {
      if (state.l2 == 0) {
        latched_l2_[id] = false;
      } else {
        state.l2 = 0;
      }
    }
    if (latched_r2_[id]) {
      if (state.r2 == 0) {
        latched_r2_[id] = false;
      } else {
        state.r2 = 0;
      }
    }
  }

  /// @brief Queues one window for every touch, button and trigger that
  /// changed, in that order, moving the seat's history state one edge at a
  /// time.
  ///
  /// @param id The seat.
  /// @param state The sample, already latched.
  /// @return Nothing on success.
  /// @retval ControllerDeliveryError::invalid_controller_state An edge could
  ///     not be computed.
  [[nodiscard]] auto flush_edges(std::uint8_t id, const ControllerState& state)
      -> Expected<void, ControllerDeliveryError> {
    auto& previous = history_previous_[id];
    const auto flush_edge = [&](const ControllerState& next)
        -> Expected<void, ControllerDeliveryError> {
      const auto edge = ControllerSample::history_edges(previous, next);
      if (!edge) {
        return unexpected(ControllerDeliveryError::invalid_controller_state);
      }
      if (!edge->empty()) {
        flush_history(id, edge->events());
      }
      previous = next;
      return {};
    };
    for (std::size_t index = 0; index < state.touches.size(); ++index) {
      auto next = previous;
      next.touches[index] = state.touches[index];
      if (auto flushed = flush_edge(next); !flushed) {
        return flushed;
      }
    }
    for (unsigned bit = 0; bit < controller_button_count; ++bit) {
      const auto mask = std::uint32_t{1} << bit;
      auto next = previous;
      next.buttons = (previous.buttons & ~mask) | (state.buttons & mask);
      if (auto flushed = flush_edge(next); !flushed) {
        return flushed;
      }
    }
    auto next = previous;
    next.l2 = state.l2;
    if (auto flushed = flush_edge(next); !flushed) {
      return flushed;
    }
    next.r2 = state.r2;
    return flush_edge(next);
  }

  /// @brief Puts events before the recent history and queues the window.
  ///
  /// The window is capped at history_capacity events; the outbox drops its
  /// oldest window when full and counts that window's new events with the
  /// next one, so the sequence still moves by every event made.
  ///
  /// @param id The seat.
  /// @param events The events, oldest first.
  void flush_history(std::uint8_t id,
                     std::span<const takion::FeedbackHistoryEvent> events) {
    const Window pending{recent_history_, id, events};
    history_events_ += events.size();
    auto fresh = events.size();
    if (outbox_.size() >= outbox_capacity) {
      const auto shed = outbox_.front()->fresh;
      outbox_.pop();
      ++windows_shed_;
      if (auto* next = outbox_.front()) {
        next->fresh += shed;
      } else {
        fresh += shed;
      }
    }
    static_cast<void>(outbox_.try_emplace(pending, fresh));
    ++windows_queued_;
    recent_history_ = pending.newest(policy_.recent_history_events);
  }

  /// @brief Posts one window.
  ///
  /// @param window The window.
  /// @param fresh How many of its events are new; 0 for a resend.
  /// @return Whether the session accepted it.
  [[nodiscard]] auto post_window(const Window& window, std::size_t fresh)
      -> bool {
    return static_cast<bool>(sink_.post_controller_history_window(
        window.events(), window.controller_ids(), fresh));
  }

  /// @brief Posts every queued window, arming the resends.
  ///
  /// @param now_us Monotonic microseconds now.
  /// @return False when the session refused one; the rest stay queued.
  auto drain_history(std::uint64_t now_us) -> bool {
    while (const auto* pending = outbox_.front()) {
      if (!post_window(pending->window, pending->fresh)) {
        return false;
      }
      outbox_.pop();
      repeats_left_ = policy_.history_repeats;
      last_history_sent_us_ = now_us;
    }
    return true;
  }

  /// @brief The session.
  Sink& sink_;
  /// @brief The pacing.
  const FeedbackDeliveryPolicy policy_;
  /// @brief Each seat's state schedule.
  std::array<FeedbackStateSchedule, Pad::count> schedules_;
  /// @brief Each seat's state as last turned into history.
  std::array<ControllerState, Pad::count> history_previous_{};
  /// @brief Each seat's latest state, for refresh().
  std::array<std::optional<ControllerState>, Pad::count> latest_{};
  /// @brief Which seats are active.
  std::array<bool, Pad::count> active_{true, false, false, false};
  /// @brief Each seat's latched buttons.
  std::array<std::uint32_t, Pad::count> latched_buttons_{};
  /// @brief Each seat's latched L2.
  std::array<bool, Pad::count> latched_l2_{};
  /// @brief Each seat's latched R2.
  std::array<bool, Pad::count> latched_r2_{};
  /// @brief The recent events every window repeats.
  Window recent_history_;
  /// @brief Windows waiting to be posted, oldest first; only this delivery's
  /// caller touches it, so its single producer and consumer are one thread.
  rigtorp::SPSCQueue<Pending> outbox_;
  /// @brief Resends left for the last window.
  std::uint8_t repeats_left_{};
  /// @brief When history was last posted, in monotonic microseconds.
  std::uint64_t last_history_sent_us_{};
  /// @brief The seat whose state refresh() checks first.
  std::uint8_t next_refresh_{};
  /// @brief Samples of inactive seats ignored.
  std::uint64_t ignored_samples_{};
  /// @brief History events queued.
  std::uint64_t history_events_{};
  /// @brief History windows queued.
  std::uint64_t windows_queued_{};
  /// @brief Windows dropped from a full outbox.
  std::uint64_t windows_shed_{};
  /// @brief Resends of the newest window.
  std::uint64_t resends_{};
};

}  // namespace libchiaking::net::protocol::remote_play
