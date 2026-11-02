/// @file
/// @brief Declares the actions generic-data transfers produce and the Boost.SML
/// machine that runs outgoing transfers.
#pragma once

#include "src/core/net/protocol/takion/detail/typed_message_codec.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <boost/sml.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace libchiaking::net::protocol::takion {

/// @brief An outgoing transfer is abandoned this long, in microseconds, after
/// it was announced or the receiver last said ready (10 s).
inline constexpr std::uint64_t generic_data_transfer_timeout_us = 10'000'000;
/// @brief An unanswered announcement is repeated after this long, in
/// microseconds (1 s).
inline constexpr std::uint64_t generic_data_announce_retry_us = 1'000'000;
/// @brief While streaming, missing chunks are queried this often, in
/// microseconds (250 ms).
inline constexpr std::uint64_t generic_data_missing_query_interval_us = 250'000;

/// @brief Action: send a GENERICDATA control message.
struct GenericDataSendControl {
  /// @brief The message.
  GenericDataControl control;
};

/// @brief Action: send a chunk packet.
struct GenericDataSendChunk {
  /// @brief The chunk, tag and key position still to be filled in.
  GenericDataChunk chunk;
};

/// @brief Action: an incoming transfer arrived whole.
struct GenericDataDelivered {
  /// @brief The transfer.
  std::uint32_t transfer_id{};
  /// @brief Its metadata.
  std::string metadata;
  /// @brief The blob.
  std::vector<std::byte> data;
};

/// @brief Action: an outgoing transfer ended.
struct GenericDataOutgoingEnded {
  /// @brief The transfer.
  std::uint32_t transfer_id{};
  /// @brief How.
  GenericDataOutcome outcome{GenericDataOutcome::completed};
};

/// @brief Any generic-data action.
using GenericDataAction =
    std::variant<GenericDataSendControl, GenericDataSendChunk,
                 GenericDataDelivered, GenericDataOutgoingEnded>;

namespace generic_data_lifecycle {
namespace sml = boost::sml;

/// @brief A blob queued or being sent.
struct Transfer {
  /// @brief Its id.
  std::uint32_t transfer_id{};
  /// @brief The blob.
  std::vector<std::byte> data;
  /// @brief Its metadata.
  std::string metadata;
  /// @brief Its chunk count: size / 519, rounded up.
  std::uint32_t packet_count{};
};

/// @brief Nothing being sent.
struct IdleState {};
/// @brief Announced; waiting for ready.
struct AnnouncingState {};
/// @brief Chunks sent; answering missing reports until complete.
struct StreamingState {};

/// @brief Event: offer the next queued transfer if idle.
struct NextOffered {
  /// @brief The current time, in monotonic microseconds.
  std::uint64_t now_us{};
};
/// @brief Event: the receiver said ready.
struct ReadyHeard {
  /// @brief The transfer named.
  std::uint32_t transfer_id{};
  /// @brief The current time, in monotonic microseconds.
  std::uint64_t now_us{};
};
/// @brief Event: the receiver listed missing chunks.
struct MissingReported {
  /// @brief The transfer named.
  std::uint32_t transfer_id{};
  /// @brief The missing indices; valid during the call only.
  std::span<const std::uint32_t> packets;
};
/// @brief Event: the receiver reported the transfer complete or refused.
struct Concluded {
  /// @brief The transfer named.
  std::uint32_t transfer_id{};
  /// @brief How it ended.
  GenericDataOutcome outcome{GenericDataOutcome::completed};
};
/// @brief Event: time passed.
struct Tick {
  /// @brief The current time, in monotonic microseconds.
  std::uint64_t now_us{};
};

/// @brief The outgoing side's memory: the queue, the active transfer, its
/// timers and the action list.
class Context {
 public:
  /// @brief Actions produced, oldest first, waiting to be collected.
  std::vector<GenericDataAction> actions;
  /// @brief Events naming a transfer other than the active one.
  std::size_t stale_events{};
  /// @brief Events no transition applied to.
  std::size_t unexpected_events{};

  /// @brief Reads the active transfer.
  ///
  /// @return Its id; nothing when idle.
  auto transfer_id() const noexcept -> std::optional<std::uint32_t> {
    return transfer_ ? std::optional{transfer_->transfer_id} : std::nullopt;
  }

  /// @brief Tells whether an id names the active transfer.
  ///
  /// @param transfer_id The id.
  /// @return Whether it does.
  auto is_current(std::uint32_t transfer_id) const noexcept -> bool {
    return transfer_ && transfer_->transfer_id == transfer_id;
  }

  /// @brief Queues a transfer.
  ///
  /// @param transfer The transfer.
  void enqueue(Transfer transfer) { queued_.push_back(std::move(transfer)); }

  /// @brief Counts queued transfers.
  ///
  /// @return The count.
  auto queued() const noexcept -> std::size_t { return queued_.size(); }

  /// @brief Makes the oldest queued transfer active and announces it.
  ///
  /// @param now_us The current time, in monotonic microseconds.
  void offer_next(std::uint64_t now_us) {
    transfer_ = std::move(queued_.front());
    queued_.pop_front();
    timer_start_us_ = now_us;
    announced_us_ = now_us;
    pending_.clear();
    announce();
  }

  /// @brief Announces the active transfer again.
  ///
  /// @param now_us The current time, in monotonic microseconds.
  void reannounce(std::uint64_t now_us) {
    announced_us_ = now_us;
    announce();
  }

  /// @brief Sends every chunk of the active transfer and starts the missing
  /// query timer.
  ///
  /// @param now_us The current time, in monotonic microseconds.
  void start_streaming(std::uint64_t now_us) {
    timer_start_us_ = now_us;
    queried_us_ = now_us;
    for (std::uint32_t index = 0; index < transfer_->packet_count; ++index)
      pending_.push_back(static_cast<std::uint8_t>(index));
    flush();
  }

  /// @brief Restarts the transfer timeout.
  ///
  /// @param now_us The current time, in monotonic microseconds.
  void keep_alive(std::uint64_t now_us) noexcept { timer_start_us_ = now_us; }

  /// @brief Sends any chunks waiting and, every 250 ms, a missing-chunks query.
  ///
  /// @param now_us The current time, in monotonic microseconds.
  void stream(std::uint64_t now_us) {
    flush();
    if (!beyond(now_us, queried_us_, generic_data_missing_query_interval_us))
      return;
    queried_us_ = now_us;
    actions.emplace_back(GenericDataSendControl{GenericDataControl{
        transfer_->transfer_id,
        static_cast<std::uint32_t>(GenericDataControlType::missing_query),
        {},
        {},
        {},
        {},
        {}}});
  }

  /// @brief Queues the listed chunks for resending, ignoring unknown indices
  /// and ones already queued, and sends them.
  ///
  /// @param packets The missing indices.
  void resend(std::span<const std::uint32_t> packets) {
    for (auto requested = packets.rbegin(); requested != packets.rend();
         ++requested) {
      if (*requested >= transfer_->packet_count) continue;
      const auto index = static_cast<std::uint8_t>(*requested);
      if (std::ranges::find(pending_, index) == pending_.end())
        pending_.push_back(index);
    }
    flush();
  }

  /// @brief Ends the active transfer.
  ///
  /// @param outcome How it ended.
  void conclude(GenericDataOutcome outcome) {
    actions.emplace_back(
        GenericDataOutgoingEnded{transfer_->transfer_id, outcome});
    transfer_.reset();
    pending_.clear();
  }

  /// @brief Tells whether the active transfer has timed out.
  ///
  /// @param now_us The current time, in monotonic microseconds.
  /// @return Whether 10 s have passed since its timer started.
  auto timed_out(std::uint64_t now_us) const noexcept -> bool {
    return beyond(now_us, timer_start_us_, generic_data_transfer_timeout_us);
  }

  /// @brief Tells whether the announcement should be repeated.
  ///
  /// @param now_us The current time, in monotonic microseconds.
  /// @return Whether 1 s has passed since the last announcement.
  auto announce_due(std::uint64_t now_us) const noexcept -> bool {
    return beyond(now_us, announced_us_, generic_data_announce_retry_us);
  }

  /// @brief When a Tick next acts on the active transfer while it is
  /// announced: its timeout or the next announcement.
  ///
  /// @return Monotonic microseconds; nothing without an active transfer.
  auto announcing_deadline_us() const noexcept -> std::optional<std::uint64_t> {
    if (!transfer_) return std::nullopt;
    return std::min(passed(timer_start_us_, generic_data_transfer_timeout_us),
                    passed(announced_us_, generic_data_announce_retry_us));
  }

  /// @brief When a Tick next acts on the active transfer while it streams:
  /// at once with chunks waiting, else its timeout or the next missing-chunks
  /// query.
  ///
  /// @return Monotonic microseconds; nothing without an active transfer.
  auto streaming_deadline_us() const noexcept -> std::optional<std::uint64_t> {
    if (!transfer_) return std::nullopt;
    if (!pending_.empty()) return timer_start_us_;
    return std::min(
        passed(timer_start_us_, generic_data_transfer_timeout_us),
        passed(queried_us_, generic_data_missing_query_interval_us));
  }

 private:
  /// @brief Tells whether more than a limit has passed since a moment.
  ///
  /// @param now_us The current time.
  /// @param since_us The moment.
  /// @param limit_us The limit.
  /// @return Whether `now_us - since_us` exceeds it.
  static auto beyond(std::uint64_t now_us, std::uint64_t since_us,
                     std::uint64_t limit_us) noexcept -> bool {
    return now_us > since_us && now_us - since_us > limit_us;
  }

  /// @brief The first moment beyond() is true for a moment and a limit.
  ///
  /// @param since_us The moment.
  /// @param limit_us The limit.
  /// @return One microsecond after the limit has passed, saturating.
  static auto passed(std::uint64_t since_us, std::uint64_t limit_us) noexcept
      -> std::uint64_t {
    return timing::Deadline::after_us(since_us, limit_us + 1);
  }

  /// @brief Queues the active transfer's announcement.
  void announce() {
    actions.emplace_back(GenericDataSendControl{GenericDataControl{
        transfer_->transfer_id,
        static_cast<std::uint32_t>(GenericDataControlType::announce),
        static_cast<std::uint32_t>(transfer_->data.size()),
        transfer_->metadata,
        transfer_->packet_count,
        {},
        {}}});
  }

  /// @brief Turns every pending chunk index into a send action.
  void flush() {
    while (!pending_.empty()) {
      const auto index = pending_.front();
      pending_.pop_front();
      const auto offset =
          static_cast<std::size_t>(index) * generic_data_chunk_size;
      const auto end =
          std::min(offset + generic_data_chunk_size, transfer_->data.size());
      GenericDataChunk chunk;
      chunk.transfer_id = transfer_->transfer_id;
      chunk.packet_index = index;
      chunk.transfer_offset = static_cast<std::uint32_t>(offset);
      chunk.payload.assign(std::span<const std::byte>{transfer_->data}.subspan(
          offset, end - offset));
      actions.emplace_back(GenericDataSendChunk{std::move(chunk)});
    }
  }

  /// @brief Transfers waiting.
  std::deque<Transfer> queued_;
  /// @brief The active transfer.
  std::optional<Transfer> transfer_;
  /// @brief When the transfer timeout started.
  std::uint64_t timer_start_us_{};
  /// @brief When the transfer was last announced.
  std::uint64_t announced_us_{};
  /// @brief When missing chunks were last queried.
  std::uint64_t queried_us_{};
  /// @brief Chunk indices waiting to be sent.
  std::deque<std::uint8_t> pending_;
};

/// @brief The outgoing transfer transition table: Idle to Announcing on an
/// offer with something queued; Announcing to Streaming on ready; either back
/// to Idle on conclusion or timeout.
struct Fsm {
  /// @brief Builds the table.
  ///
  /// @return The table.
  auto operator()() const noexcept {
    using namespace sml;

    const auto offer = [](const NextOffered& event, Context& context) {
      context.offer_next(event.now_us);
    };
    const auto start_streaming = [](const ReadyHeard& event, Context& context) {
      context.start_streaming(event.now_us);
    };
    const auto keep_alive = [](const ReadyHeard& event, Context& context) {
      context.keep_alive(event.now_us);
    };
    const auto resend = [](const MissingReported& event, Context& context) {
      context.resend(event.packets);
    };
    const auto conclude = [](const Concluded& event, Context& context) {
      context.conclude(event.outcome);
    };
    const auto time_out = [](Context& context) {
      context.conclude(GenericDataOutcome::timed_out);
    };
    const auto reannounce = [](const Tick& event, Context& context) {
      context.reannounce(event.now_us);
    };
    const auto stream = [](const Tick& event, Context& context) {
      context.stream(event.now_us);
    };
    const auto nothing = [] {};

    const auto waiting = [](const Context& context) {
      return context.queued() != 0;
    };
    const auto expired = [](const Tick& event, const Context& context) {
      return context.timed_out(event.now_us);
    };
    const auto announce_due = [](const Tick& event, const Context& context) {
      return !context.timed_out(event.now_us) &&
             context.announce_due(event.now_us);
    };
    const auto announce_waits = [](const Tick& event, const Context& context) {
      return !context.timed_out(event.now_us) &&
             !context.announce_due(event.now_us);
    };
    const auto live = [](const Tick& event, const Context& context) {
      return !context.timed_out(event.now_us);
    };

    return make_transition_table(
        *state<IdleState> + event<NextOffered>[waiting] / offer =
            state<AnnouncingState>,
        state<IdleState> + event<NextOffered>[!waiting] / nothing,
        state<IdleState> + event<Tick> / nothing,

        state<AnnouncingState> + event<NextOffered> / nothing,
        state<AnnouncingState> + event<ReadyHeard> / start_streaming =
            state<StreamingState>,
        state<AnnouncingState> + event<MissingReported> / nothing,
        state<AnnouncingState> + event<Tick>[expired] / time_out =
            state<IdleState>,
        state<AnnouncingState> + event<Tick>[announce_due] / reannounce,
        state<AnnouncingState> + event<Tick>[announce_waits] / nothing,
        state<AnnouncingState> + event<Concluded> / conclude = state<IdleState>,

        state<StreamingState> + event<NextOffered> / nothing,
        state<StreamingState> + event<ReadyHeard> / keep_alive,
        state<StreamingState> + event<MissingReported> / resend,
        state<StreamingState> + event<Tick>[expired] / time_out =
            state<IdleState>,
        state<StreamingState> + event<Tick>[live] / stream,
        state<StreamingState> + event<Concluded> / conclude = state<IdleState>);
  }
};

/// @brief The outgoing side of generic data: the machine and its context,
/// with events naming another transfer dropped as stale.
///
/// Not copyable; used under the Takion client's lock.
class Outgoing {
 public:
  /// @brief Makes an idle outgoing side.
  Outgoing() : machine_(context_) {}
  /// @brief Not copyable.
  Outgoing(const Outgoing&) = delete;
  /// @brief Not copy-assignable.
  auto operator=(const Outgoing&) -> Outgoing& = delete;

  /// @brief Queues a transfer.
  ///
  /// @param transfer The transfer.
  void enqueue(Transfer transfer) { context_.enqueue(std::move(transfer)); }

  /// @brief Offers the next transfer.
  ///
  /// @param event The event.
  void process(const NextOffered& event) { settle(event); }

  /// @brief Lets time pass.
  ///
  /// @param event The event.
  void process(const Tick& event) { settle(event); }

  /// @brief Feeds an event that names a transfer, dropping it when stale.
  ///
  /// @tparam Input ReadyHeard, MissingReported or Concluded.
  /// @param event The event.
  template <class Input>
  void process(const Input& event) {
    if (!context_.is_current(event.transfer_id)) {
      ++context_.stale_events;
      return;
    }
    settle(event);
  }

  /// @brief Reads the active transfer.
  ///
  /// @return Its id; nothing when idle.
  auto transfer_id() const noexcept -> std::optional<std::uint32_t> {
    return context_.transfer_id();
  }

  /// @brief Counts queued transfers.
  ///
  /// @return The count.
  auto queued() const noexcept -> std::size_t { return context_.queued(); }

  /// @brief When the next Tick or offer has something to do.
  ///
  /// @return Monotonic microseconds, 0 when a queued transfer waits to be
  ///     offered; nothing when idle with nothing queued.
  auto next_deadline_us() const noexcept -> std::optional<std::uint64_t> {
    if (machine_.is(sml::state<IdleState>))
      return context_.queued() != 0 ? std::optional<std::uint64_t>{0}
                                    : std::nullopt;
    if (machine_.is(sml::state<StreamingState>))
      return context_.streaming_deadline_us();
    return context_.announcing_deadline_us();
  }

  /// @brief Reaches the action list.
  ///
  /// @return The list.
  auto actions() noexcept -> std::vector<GenericDataAction>& {
    return context_.actions;
  }

 private:
  /// @brief Feeds an event to the machine, counting it if refused.
  ///
  /// @tparam Input An event type.
  /// @param event The event.
  template <class Input>
  void settle(const Input& event) {
    if (!machine_.process_event(event)) ++context_.unexpected_events;
  }

  /// @brief The context.
  Context context_;
  /// @brief The machine.
  sml::sm<Fsm> machine_;
};

}  // namespace generic_data_lifecycle
}  // namespace libchiaking::net::protocol::takion
