/// @file
/// @brief Declares GenericDataExchange, both ends of Takion's generic-data
/// transfers: queuing and sending blobs, and receiving and reassembling the
/// console's.
///
/// Sending: announce (size, chunk count, metadata), wait for ready, send every
/// chunk, query missing chunks every 250 ms and resend them, until complete or
/// reject, or until 10 s pass without the receiver saying ready. Receiving:
/// answer an announcement with ready (or reject), place chunks by offset,
/// answer missing queries, and deliver the blob with complete once every byte
/// has arrived. One transfer is active in each direction at a time.
#pragma once

#include "src/core/net/protocol/takion/detail/typed_message_codec.hpp"
#include "src/core/net/protocol/takion/generic_data_lifecycle.hpp"
#include "src/core/net/protocol/takion/result.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace libchiaking::net::protocol::takion {

/// @brief Outgoing transfers that may wait behind the active one.
inline constexpr std::size_t generic_data_outgoing_queue_capacity = 8;

/// @brief Why a transfer could not be queued, or a message from the console
/// was refused.
enum class GenericDataError : std::uint8_t {
  /// @brief The blob is empty.
  empty_data,
  /// @brief The blob is 51200 bytes or more.
  data_too_large,
  /// @brief The metadata is 2048 bytes or more.
  metadata_too_large,
  /// @brief Eight transfers already wait.
  queue_full,
  /// @brief An announcement was refused (stale, or impossible sizes); a reject
  /// was queued.
  invalid_announcement,
  /// @brief A chunk lies outside the transfer, is empty, has an unknown index,
  /// or overlaps a received one.
  invalid_chunk,
  /// @brief Every chunk arrived but they do not cover the transfer; a reject
  /// was
  /// queued.
  incomplete_coverage,
};

/// @brief The generic-data state of one session.
///
/// It performs no I/O: every message and chunk it wants sent, every delivered
/// blob and every finished transfer becomes an action, collected with
/// take_actions(). Owned by the Takion client and used under its lock; not
/// copyable. Time is monotonic microseconds.
class GenericDataExchange {
 public:
  /// @brief Makes an idle exchange.
  ///
  /// @param first_transfer_id Id of the first outgoing transfer; later ones
  ///     count up.
  explicit GenericDataExchange(std::uint32_t first_transfer_id = 1)
      : next_transfer_id_(first_transfer_id) {}
  /// @brief Not copyable.
  GenericDataExchange(const GenericDataExchange&) = delete;
  /// @brief Not copy-assignable.
  auto operator=(const GenericDataExchange&) -> GenericDataExchange& = delete;

  /// @brief Queues a blob to send, announcing it at once when nothing else is
  /// being sent.
  ///
  /// @param data The blob; 1 to 51199 bytes.
  /// @param metadata Text sent with the announcement; under 2048 bytes.
  /// @param now_us The current time.
  /// @return The transfer's id.
  /// @retval GenericDataError::empty_data The blob is empty.
  /// @retval GenericDataError::data_too_large The blob is too large.
  /// @retval GenericDataError::metadata_too_large The metadata is too long.
  /// @retval GenericDataError::queue_full Eight transfers already wait.
  [[nodiscard]] auto submit(std::vector<std::byte> data, std::string metadata,
                            std::uint64_t now_us)
      -> Expected<std::uint32_t, GenericDataError>;
  /// @brief Takes a GENERICDATA control message from the console.
  ///
  /// @param control The message.
  /// @param now_us The current time.
  /// @return Nothing; unknown types and messages for other transfers are
  ///     ignored.
  /// @retval GenericDataError::invalid_announcement An announcement was
  /// refused.
  [[nodiscard]] auto receive_control(const GenericDataControl& control,
                                     std::uint64_t now_us)
      -> Expected<void, GenericDataError>;
  /// @brief Takes a decrypted chunk from the console.
  ///
  /// @param chunk The chunk.
  /// @return Nothing; chunks of other transfers and repeated indices are
  ///     ignored.
  /// @retval GenericDataError::invalid_chunk The chunk is out of bounds or
  ///     overlaps.
  /// @retval GenericDataError::incomplete_coverage The last chunk arrived with
  ///     bytes still uncovered; the transfer was rejected.
  [[nodiscard]] auto receive_chunk(const GenericDataChunk& chunk)
      -> Expected<void, GenericDataError>;
  /// @brief Advances the outgoing transfer's timers and offers the next queued
  /// one when idle.
  ///
  /// @param now_us The current time.
  void poll(std::uint64_t now_us);

  /// @brief Collects the actions produced so far.
  ///
  /// @return The actions, oldest first; the list is then empty.
  std::vector<GenericDataAction> take_actions();

  /// @brief Reads the outgoing transfer in progress.
  ///
  /// @return Its id; nothing when idle.
  std::optional<std::uint32_t> active_outgoing() const noexcept;
  /// @brief Reads when poll() next has something to do for the outgoing
  /// side.
  ///
  /// @return Monotonic microseconds, 0 when a queued transfer waits to be
  ///     offered; nothing when idle with nothing queued.
  std::optional<std::uint64_t> next_poll_us() const noexcept;
  /// @brief Reads the incoming transfer in progress.
  ///
  /// @return Its id; nothing when none.
  std::optional<std::uint32_t> active_incoming() const noexcept;
  /// @brief Counts outgoing transfers waiting.
  ///
  /// @return The count.
  std::size_t queued_outgoing() const noexcept { return outgoing_.queued(); }

 private:
  /// @brief A byte range of an incoming transfer already filled.
  struct ReceivedRange {
    /// @brief Its offset.
    std::uint32_t offset{};
    /// @brief Its length.
    std::uint32_t length{};
  };

  /// @brief The incoming transfer being received.
  struct Incoming {
    /// @brief Its id.
    std::uint32_t transfer_id{};
    /// @brief Its metadata.
    std::string metadata;
    /// @brief Chunks announced.
    std::uint32_t packet_count{};
    /// @brief The blob being filled, sized as announced.
    std::vector<std::byte> buffer;
    /// @brief Which chunk indices are still missing.
    std::vector<bool> missing;
    /// @brief How many are.
    std::size_t missing_count{};
    /// @brief Ranges filled.
    std::vector<ReceivedRange> ranges;
    /// @brief Bytes filled.
    std::size_t covered{};
  };

  /// @brief Ends the outgoing transfer and offers the next.
  ///
  /// @param outcome How it ended.
  /// @param transfer_id The transfer the console named.
  /// @param now_us The current time.
  void finish_outgoing(GenericDataOutcome outcome, std::uint32_t transfer_id,
                       std::uint64_t now_us);
  /// @brief Answers an announcement: ready for a new or repeated transfer,
  /// reject for a stale or impossible one.
  ///
  /// @param control The announcement.
  void handle_announce(const GenericDataControl& control);
  /// @brief Answers a missing-chunks query: up to 25 missing indices, or
  /// complete for the transfer just finished.
  ///
  /// @param control The query.
  void handle_missing_query(const GenericDataControl& control);
  /// @brief Queues a reject for an announcement.
  ///
  /// @param transfer_id The transfer.
  /// @param error Why.
  void reject_incoming_announcement(std::uint32_t transfer_id,
                                    std::string error);
  /// @brief Queues a control message to send.
  ///
  /// @param control The message.
  void send_control(GenericDataControl control);

  /// @brief Id of the next outgoing transfer.
  std::uint32_t next_transfer_id_;
  /// @brief The outgoing machine, which also holds the action list.
  generic_data_lifecycle::Outgoing outgoing_;
  /// @brief The incoming transfer, if any.
  std::optional<Incoming> incoming_;
  /// @brief The incoming transfer last completed, to answer late queries.
  std::optional<std::uint32_t> last_completed_incoming_;

  /// @brief Gives a control type's wire value.
  ///
  /// @param type The type.
  /// @return Its number.
  static constexpr std::uint32_t control_type(
      GenericDataControlType type) noexcept;
  /// @brief Tells whether two byte ranges overlap.
  ///
  /// @param left_offset First range's start.
  /// @param left_length First range's length.
  /// @param right_offset Second range's start.
  /// @param right_length Second range's length.
  /// @return Whether they share a byte.
  static bool ranges_overlap(std::uint64_t left_offset,
                             std::uint64_t left_length,
                             std::uint64_t right_offset,
                             std::uint64_t right_length) noexcept;
};

}  // namespace libchiaking::net::protocol::takion
