/// @file
/// @brief Declares MediaReceiver, the guest side of Kakusan media forwarding.
#pragma once

#include "libchiaking/platform/byte_units.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "src/core/utils/timing/token_bucket.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::net::kakusan {

/// @brief Entries in the replay and packet windows.
inline constexpr std::size_t sequence_window_size = 2048;
/// @brief Most source datagrams being reassembled.
inline constexpr std::size_t max_incomplete_packets = 64;
/// @brief Most reassembly buffer bytes: 512 KiB.
inline constexpr std::size_t max_reassembly_bytes = 512 * bytes_per_kib;
/// @brief Longest a datagram may stay incomplete, in microseconds: 100 ms.
inline constexpr std::uint64_t max_reassembly_age_us = 100'000;
/// @brief Longest an acknowledgement waits after the first unacknowledged
/// arrival, in microseconds: 25 ms.
inline constexpr std::uint64_t max_ack_delay_us = 25'000;
/// @brief Arrivals that make an acknowledgement due at once.
inline constexpr std::uint32_t ack_packet_threshold = 2;
/// @brief Acknowledgements sent per second at most.
inline constexpr std::uint64_t ack_tokens_per_second = 200;
/// @brief Burst of acknowledgements.
inline constexpr std::uint64_t ack_token_capacity = 20;

/// @brief A reassembled source datagram, ready for Takion decryption.
struct SourcePacket {
  /// @brief Video or audio.
  MediaKind kind{MediaKind::video};
  /// @brief Its full key position.
  std::uint64_t key_position{};
  /// @brief The original datagram.
  std::vector<std::byte> bytes;
};

/// @brief Reassembly limits; each is capped at its protocol maximum.
struct MediaReceiverLimits {
  /// @brief Most datagrams being reassembled.
  std::size_t incomplete_packets{max_incomplete_packets};
  /// @brief Most buffer bytes.
  std::size_t reassembly_bytes{max_reassembly_bytes};
  /// @brief Longest incomplete age, in microseconds.
  std::uint64_t reassembly_age_us{max_reassembly_age_us};
};

/// @brief The guest side of Kakusan media: authenticates RCPM datagrams,
/// rejects replays, reassembles source datagrams and schedules RCA1
/// acknowledgements (RCP TRANSPORT sections 3 and 4).
///
/// Datagram sequences pass a 2048-entry sliding window; a second window over
/// packet sequences stops completed or evicted datagrams being rebuilt.
/// Incomplete datagrams are evicted oldest first beyond 64, 512 KiB or 100 ms.
/// An acknowledgement is due after two arrivals, 25 ms after the first
/// unacknowledged one, or at once on a gap or reordering, at most 200 a second.
/// Holds the media key; not copyable or movable; one thread.
class MediaReceiver {
  /// @brief A sequence of this value is refused: the sender must close first.
  static constexpr std::uint64_t counter_limit =
      std::numeric_limits<std::uint64_t>::max();

  /// @brief A sequence's slot in a window.
  ///
  /// @param sequence The sequence.
  /// @return Its index modulo 2048.
  static constexpr auto slot_index(std::uint64_t sequence) noexcept
      -> std::size_t {
    return static_cast<std::size_t>(sequence % sequence_window_size);
  }

 public:
  /// @brief Starts a receiver for one spoke.
  ///
  /// @param crypto Checks the tags; must outlive the receiver.
  /// @param key The spoke's media key.
  /// @param connection The spoke's connection id.
  /// @param limits Reassembly limits, capped at the maximums.
  MediaReceiver(platform::CryptoProvider& crypto, MediaKey key,
                const ConnectionId& connection,
                MediaReceiverLimits limits = {}) noexcept;

  /// @brief Not copyable.
  MediaReceiver(const MediaReceiver&) = delete;
  /// @brief Not copyable.
  ///
  /// @return Never returns.
  MediaReceiver& operator=(const MediaReceiver&) = delete;
  /// @brief Not movable.
  MediaReceiver(MediaReceiver&&) = delete;
  /// @brief Not movable.
  ///
  /// @return Never returns.
  MediaReceiver& operator=(MediaReceiver&&) = delete;

  /// @brief Feeds a datagram from the nominated pair.
  ///
  /// Datagrams for another connection or with a bad tag are dropped silently,
  /// as are replays.
  ///
  /// @param datagram The datagram.
  /// @param now_us The current monotonic time.
  /// @return The source datagram it completed; nothing otherwise.
  /// @retval Error::protocol A validly tagged datagram has invalid fields,
  ///     conflicting metadata, a duplicate fragment, or exhausted counters.
  /// @retval Error The MAC could not be computed.
  [[nodiscard]] auto receive(std::span<const std::byte> datagram,
                             std::uint64_t now_us)
      -> Result<std::optional<SourcePacket>>;
  /// @brief Expires stale reassemblies and builds a due acknowledgement.
  ///
  /// @param now_us The current monotonic time.
  /// @return The RCA1 record to send; nothing when none is due or the rate
  ///     limit holds it back.
  [[nodiscard]] auto poll(std::uint64_t now_us)
      -> std::optional<std::vector<std::byte>>;
  /// @brief When poll() must next run.
  ///
  /// @return The earlier of the acknowledgement and the oldest reassembly's
  ///     expiry; nothing when neither.
  auto next_deadline_us() const noexcept -> std::optional<std::uint64_t>;

  /// @brief Datagrams being reassembled.
  ///
  /// @return The count.
  auto incomplete_packets() const noexcept -> std::size_t;
  /// @brief Reassembly buffer bytes in use.
  ///
  /// @return The count.
  auto reassembly_bytes() const noexcept -> std::size_t;

 private:
  /// @brief What a window remembers of a sequence.
  enum class Slot : std::uint8_t {
    /// @brief Not seen.
    empty = 0,
    /// @brief Accepted (datagrams), or evicted incomplete (packets).
    marked = 1,
    /// @brief Reassembled and delivered (packets).
    completed = 2,
  };

  /// @brief The 2048 sequences ending at the largest seen.
  class SequenceWindow {
   public:
    /// @brief Whether any sequence was seen.
    ///
    /// @return True after the first.
    auto started() const noexcept -> bool { return started_; }
    /// @brief The largest sequence seen.
    ///
    /// @return It.
    auto largest() const noexcept -> std::uint64_t { return largest_; }
    /// @brief Whether a sequence lies in the window.
    ///
    /// @param sequence The sequence.
    /// @return True for the largest and the 2047 below it.
    auto contains(std::uint64_t sequence) const noexcept -> bool;
    /// @brief What the window remembers of a sequence.
    ///
    /// @param sequence The sequence.
    /// @return Its slot; empty outside the window.
    auto slot(std::uint64_t sequence) const noexcept -> Slot;
    /// @brief Moves the window up to a new largest, clearing the slots
    /// passed.
    ///
    /// @param sequence The new largest; smaller values do nothing.
    void advance_to(std::uint64_t sequence) noexcept;
    /// @brief Sets a sequence's slot if it lies in the window.
    ///
    /// @param sequence The sequence.
    /// @param value The slot.
    void set(std::uint64_t sequence, Slot value) noexcept;
    /// @brief Forgets everything.
    void clear() noexcept;

   private:
    /// @brief The slots, by sequence modulo 2048.
    std::array<Slot, sequence_window_size> slots_{};
    /// @brief The largest sequence seen.
    std::uint64_t largest_{};
    /// @brief Whether any was seen.
    bool started_{};
  };

  /// @brief A source datagram being reassembled.
  struct Assembly {
    /// @brief Its packet sequence.
    std::uint64_t packet_sequence{};
    /// @brief Its kind; every fragment must agree.
    MediaKind kind{MediaKind::video};
    /// @brief Its key position; every fragment must agree.
    std::uint64_t key_position{};
    /// @brief Its size; every fragment must agree.
    std::uint16_t original_size{};
    /// @brief When its first fragment arrived.
    std::uint64_t first_us{};
    /// @brief Bit per fragment received.
    std::uint32_t present{};
    /// @brief Bit per fragment needed.
    std::uint32_t expected{};
    /// @brief The datagram being filled.
    std::vector<std::byte> buffer;
  };

  /// @brief Places an accepted fragment.
  ///
  /// @param media The fragment.
  /// @param now_us The current time.
  /// @return The completed datagram; nothing otherwise.
  /// @retval Error::protocol A completed datagram's fragment arrived again,
  ///     or
  ///     the fragment conflicts.
  [[nodiscard]] auto reassemble(const MediaDatagram& media,
                                std::uint64_t now_us)
      -> Result<std::optional<SourcePacket>>;
  /// @brief Starts a new reassembly, or delivers a single-fragment datagram.
  ///
  /// @param media The first fragment.
  /// @param now_us The current time.
  /// @return The datagram when it is one fragment; nothing otherwise.
  [[nodiscard]] auto admit(const MediaDatagram& media, std::uint64_t now_us)
      -> std::optional<SourcePacket>;
  /// @brief Adds a fragment to a reassembly.
  ///
  /// @param index The reassembly.
  /// @param media The fragment.
  /// @return The completed datagram; nothing otherwise.
  /// @retval Error::protocol Conflicting metadata or a duplicate fragment.
  [[nodiscard]] auto merge(std::size_t index, const MediaDatagram& media)
      -> Result<std::optional<SourcePacket>>;
  /// @brief Finds a reassembly.
  ///
  /// @param packet_sequence Its packet sequence.
  /// @return Its index; nothing when absent.
  [[nodiscard]] auto find(std::uint64_t packet_sequence) const noexcept
      -> std::optional<std::size_t>;
  /// @brief Abandons a reassembly, remembering its sequence.
  ///
  /// @param index The reassembly.
  void evict(std::size_t index) noexcept;
  /// @brief Abandons reassemblies the packet window has passed.
  void evict_outside_window() noexcept;
  /// @brief Abandons reassemblies older than the age limit.
  ///
  /// @param now_us The current time.
  void expire(std::uint64_t now_us) noexcept;
  /// @brief Marks a datagram accepted and schedules the acknowledgement.
  ///
  /// @param sequence Its datagram sequence.
  /// @param now_us When it arrived.
  void note_datagram(std::uint64_t sequence, std::uint64_t now_us) noexcept;
  /// @brief Builds an RCA1 of up to 32 newest ranges.
  ///
  /// @param now_us The current time, for the delay.
  /// @return The record; nothing when it cannot be encoded.
  [[nodiscard]] auto build_ack(std::uint64_t now_us) const
      -> std::optional<std::vector<std::byte>>;

  /// @brief Checks the tags.
  platform::CryptoProvider& crypto_;
  /// @brief The spoke's media key.
  MediaKey key_;
  /// @brief The spoke's connection id.
  ConnectionId connection_;
  /// @brief The capped limits.
  MediaReceiverLimits limits_;

  /// @brief The replay window over datagram sequences.
  SequenceWindow datagrams_;
  /// @brief The window over packet sequences.
  SequenceWindow packets_;
  /// @brief Reassemblies, oldest first.
  std::vector<Assembly> assemblies_;
  /// @brief Their buffer bytes.
  std::size_t reassembly_bytes_{};

  /// @brief When the largest datagram sequence arrived.
  std::uint64_t largest_arrival_us_{};
  /// @brief Arrivals since the last acknowledgement, capped at 2.
  std::uint32_t unacknowledged_{};
  /// @brief When the next acknowledgement is due.
  std::optional<std::uint64_t> ack_due_us_;
  /// @brief The acknowledgement rate limit: 200/s, burst 20.
  timing::TokenBucket ack_tokens_;
};

}  // namespace libchiaking::net::kakusan
