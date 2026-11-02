/// @file
/// @brief Declares Capture, the host's hand-off of the console's encrypted
/// stream from the path that receives it to the shared session that forwards
/// it to guests.
///
/// The stream is forwarded as the console sent it, still encrypted. Guests get
/// the stream's receive keys in the source setup and decrypt it themselves, so
/// Capture keeps both: the current keys, and a bounded queue of copies of the
/// datagrams that arrived since the shared session last took them.
#pragma once

#include "libchiaking/detail/play/session_share.hpp"
#include "libchiaking/platform/byte_units.hpp"
#include "libchiaking/platform/runtime/threading.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace libchiaking::kakusan {

/// @brief Most bytes of datagrams held in the queue at once, 4 MiB; also the
/// largest single datagram accepted.
inline constexpr std::size_t capture_byte_limit = 4 * bytes_per_mib;
/// @brief Most datagrams held in the queue at once.
inline constexpr std::size_t capture_datagram_limit = 4096;

/// @brief One encrypted stream datagram copied out of the receive path.
///
/// Plain data. `facts.bytes` is left empty once captured, since the span it
/// held belonged to the caller; view() points it at the owned copy.
struct CapturedDatagram {
  /// @brief What the receive path knew about the datagram (media kind, key
  /// position and so on), with its byte span cleared.
  play::SharedDatagram facts;
  /// @brief The owned copy of the encrypted datagram.
  std::vector<std::byte> bytes;

  /// @brief The datagram as a play::SharedDatagram again.
  /// @return The facts with their byte span pointing at bytes; valid while
  ///     this object lives and is not moved.
  auto view() const noexcept -> play::SharedDatagram {
    auto described = facts;
    described.bytes = bytes;
    return described;
  }
};

/// @brief The receive keys of the stream being shared, tagged with the stream
/// protocol version and the generation they belong to.
///
/// A new generation starts each time the host's session hands over keys, so a
/// guest's setup can be matched with the keys it was built from. Owns its key
/// material in wiping storage; move-only in effect, with clone() for an
/// explicit copy. Not thread-safe on its own; Capture guards the copy it holds.
class CapturedKeys {
 public:
  /// @brief Takes ownership of a set of keys.
  /// @param version The stream protocol version the keys are for.
  /// @param keys The keys.
  /// @param generation The generation number; counts up from one per Capture.
  CapturedKeys(std::uint32_t version, play::ReceiveKeys keys,
               std::uint64_t generation) noexcept
      : keys_{std::move(keys)}, version_{version}, generation_{generation} {}

  /// @brief The stream's receive key.
  /// @return A view of the key; valid while this object lives.
  auto key() const noexcept
      -> std::span<const std::byte, play::ReceiveKeys::size> {
    return keys_.key();
  }
  /// @brief The stream's receive IV.
  /// @return A view of the IV; valid while this object lives.
  auto iv() const noexcept
      -> std::span<const std::byte, play::ReceiveKeys::size> {
    return keys_.iv();
  }
  /// @brief The stream protocol version the keys are for.
  /// @return The version.
  auto version() const noexcept -> std::uint32_t { return version_; }
  /// @brief Which handover of keys these are.
  /// @return The generation number.
  auto generation() const noexcept -> std::uint64_t { return generation_; }

  /// @brief Makes an explicit copy, with its own wiping storage.
  /// @return The copy.
  auto clone() const noexcept -> CapturedKeys {
    return CapturedKeys{version_, keys_.clone(), generation_};
  }

 private:
  /// @brief The key material.
  play::ReceiveKeys keys_;
  /// @brief The stream protocol version.
  std::uint32_t version_;
  /// @brief The generation number.
  std::uint64_t generation_;
};

/// @brief How many datagrams the queue lost since the last drain, by media
/// kind.
///
/// A dropped video datagram makes the shared session ask for a keyframe.
struct CaptureDrops {
  /// @brief Video datagrams dropped.
  std::size_t dropped_video{};
  /// @brief Audio datagrams dropped.
  std::size_t dropped_audio{};
};

/// @brief What one Capture::drain() took out of the queue.
struct CaptureDrain {
  /// @brief The datagrams, in arrival order.
  std::vector<CapturedDatagram> datagrams;
  /// @brief What was lost since the previous drain.
  CaptureDrops dropped;
};

/// @brief The queue between the host's stream receive path and the shared
/// session's pump.
///
/// Owned by SharedSession::Core. The receive path calls keys() and
/// datagram() from its own thread (through Host, which is a
/// play::SharedAudience); the shared session's pump calls the rest under the
/// session's lock. Datagrams are only kept while forwarding is on, which the
/// shared session turns on while at least one guest is being sent media. When
/// the queue is full the oldest datagrams are dropped first and counted.
///
/// Every member takes the internal mutex briefly and none blocks otherwise, so
/// all may be called from any thread.
class Capture {
 public:
  /// @brief Builds an empty capture: no keys, not forwarding.
  ///
  /// @param threading Provider of the internal mutex; must outlive the
  ///     capture.
  explicit Capture(platform::ThreadingProvider& threading)
      : mutex_(threading) {}
  /// @brief Not copyable; it holds key material and a queue.
  Capture(const Capture&) = delete;
  /// @brief Not copy-assignable; it holds key material and a queue.
  Capture& operator=(const Capture&) = delete;

  /// @brief Starts or stops keeping datagrams.
  ///
  /// Stopping empties the queue and forgets the drop counts.
  ///
  /// @param enabled True to keep datagrams from now on.
  void set_forwarding(bool enabled) noexcept;
  /// @brief Copies the current keys.
  /// @return A copy, or nothing before the first keys() or after reset().
  [[nodiscard]] auto clone_keys() const -> std::optional<CapturedKeys>;
  /// @brief Takes every queued datagram and the drop counts, leaving the
  /// queue empty and the counts at zero.
  /// @return What was queued and what was lost.
  auto drain() -> CaptureDrain;
  /// @brief Forgets the keys, the queue and the counts, and stops forwarding,
  /// for when the shared stream has ended.
  void reset() noexcept;

  /// @brief Takes the stream's new keys and starts a new generation.
  ///
  /// Datagrams still queued belong to the old keys and are discarded, without
  /// being counted as drops.
  ///
  /// @param protocol_version The stream protocol version.
  /// @param keys The keys; this object takes ownership.
  void keys(std::uint32_t protocol_version, play::ReceiveKeys keys) noexcept;
  /// @brief Copies one encrypted datagram into the queue.
  ///
  /// Ignored while not forwarding, and when the datagram is empty or larger
  /// than capture_byte_limit. Makes room by dropping the oldest datagrams; a
  /// copy that cannot be allocated is counted as dropped. Never blocks beyond
  /// the internal lock and never throws.
  ///
  /// @param datagram The datagram; its bytes are copied before returning.
  void datagram(const play::SharedDatagram& datagram) noexcept;

 private:
  /// @brief Drops the oldest queued datagram and counts it; the queue must
  /// not be empty and the mutex must be held.
  void drop_oldest() noexcept;

  /// @brief Guards every other member.
  mutable platform::Mutex mutex_;
  /// @brief The current keys; empty before the first keys() or after reset().
  std::optional<CapturedKeys> keys_;
  /// @brief Datagrams waiting for drain(), oldest first.
  std::deque<CapturedDatagram> queue_;
  /// @brief Total size of queue_, in bytes; at most capture_byte_limit.
  std::size_t queued_bytes_{};
  /// @brief Drops since the last drain.
  CaptureDrops dropped_{};
  /// @brief The last generation handed out; zero before any keys.
  std::uint64_t generation_{};
  /// @brief Whether datagram() keeps what it is given.
  bool forwarding_{};
};

}  // namespace libchiaking::kakusan
