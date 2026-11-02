/// @file
/// @brief Declares Microphone, which sends the player's voice to the console
/// or to a cloud game.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/input_refusal.hpp"
#include "libchiaking/detail/play/outcome.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace libchiaking::play {

/// @brief Sends encoded microphone audio to the console during remote play, or
/// to the game during cloud streaming.
///
/// The usual order is connect, then set_muted(false), then push for every
/// encoded frame. Each call only queues work; the session's own thread sends it
/// while the stream is running. Frames are 40-byte Opus frames as produced by
/// media::MicrophoneEncoder.
///
/// Obtained from Session::microphone and valid for as long as that Session. All
/// members may be called from any thread. PS5 cloud sessions connect the
/// microphone with MICCONNECTION and report muting with a MICINFO direct
/// message. LegacyCloudStream sessions take the same messages.
///
/// @see media::MicrophoneEncoder
class Microphone {
 public:
  /// @brief Implementation behind the microphone; owned by the session.
  class Core;

  /// @brief Size in bytes of one encoded frame taken by push.
  static constexpr std::size_t frame_size = 40;
  /// @brief Number of frames a session queues before the oldest is dropped.
  static constexpr std::size_t default_depth = 16;

  /// @brief Identifies one connect request so its result can be read later.
  class ConnectTicket {
   public:
    /// @brief Builds a ticket from its two parts.
    ///
    /// @param id Number the session gave the request; unique within a
    ///     session.
    /// @param tag Value the caller passed to Microphone::connect.
    constexpr ConnectTicket(std::uint32_t id, std::uint32_t tag) noexcept
        : id_(id), tag_(tag) {}

    /// @brief Number the session gave the request, counting from 1.
    ///
    /// @return Number the session gave the request, counting from 1.
    constexpr auto id() const noexcept -> std::uint32_t { return id_; }
    /// @brief The caller's own value, returned unchanged.
    ///
    /// @return The caller's own value, returned unchanged.
    constexpr auto tag() const noexcept -> std::uint32_t { return tag_; }

    /// @brief Compares both the number and the tag.
    friend constexpr auto operator==(const ConnectTicket&,
                                     const ConnectTicket&) noexcept
        -> bool = default;

   private:
    std::uint32_t id_;
    std::uint32_t tag_;
  };

  /// @brief Where a connect request stands.
  enum class ConnectState : std::uint8_t {
    /// @brief The request is queued and the session can still send it.
    pending,
    /// @brief The session sent it to the console or cloud service
    /// successfully.
    accepted,
    /// @brief It was not sent successfully, the ticket is no longer
    /// remembered, or the session can no longer send it.
    rejected,
  };

  /// @brief Running totals for the microphone since the session began.
  struct Statistics {
    /// @brief Frames accepted by push.
    std::uint64_t frames_queued{};
    /// @brief Frames handed to the connection successfully.
    std::uint64_t frames_sent{};
    /// @brief Frames discarded before sending, because the queue was full
    /// or the microphone was muted.
    std::uint64_t frames_dropped{};
    /// @brief Connect, mute and frame sends that failed.
    std::uint64_t send_failures{};
    /// @brief Cause of the most recent failed send, if any.
    std::optional<EndReason::Cause> last_error{};

    /// @brief Compares every field.
    friend constexpr auto operator==(const Statistics&,
                                     const Statistics&) noexcept
        -> bool = default;
  };

  /// @brief Binds the microphone to the session's implementation.
  ///
  /// Frontends do not construct this; they use Session::microphone.
  ///
  /// @param core Implementation owned by the session. It must outlive this
  ///     object.
  explicit Microphone(Core& core) noexcept : core_(&core) {}
  /// @brief Not copyable; a session has one microphone.
  Microphone(const Microphone&) = delete;
  /// @brief Not copy-assignable; a session has one microphone.
  auto operator=(const Microphone&) -> Microphone& = delete;

  /// @brief Asks the console or cloud service to open its microphone
  /// channel.
  ///
  /// The request is queued and its result is read with state. A new request
  /// replaces one that has not been sent yet.
  ///
  /// @param tag Any value the caller wants returned in the ticket, for
  ///     example to tell its own attempts apart.
  /// @return The ticket for this request.
  /// @retval InputRefusal::not_running The session is not accepting requests.
  /// @retval InputRefusal::wrong_route The session has no known venue.
  /// @retval InputRefusal::not_ready A rest mode request is under way.
  [[nodiscard]] auto connect(std::uint32_t tag = 0)
      -> Expected<ConnectTicket, InputRefusal>;
  /// @brief Returns the ticket of the most recent connect request.
  ///
  /// @return The ticket, or nothing when connect has not succeeded yet.
  auto latest() const -> std::optional<ConnectTicket>;
  /// @brief Reports where a connect request stands.
  ///
  /// @param ticket A ticket returned by connect. The session remembers the
  ///     last 16 tickets.
  /// @return The request's state; see ConnectState. A ticket the session does
  ///     not remember reads as rejected.
  auto state(ConnectTicket ticket) const -> ConnectState;
  /// @brief Reports whether the session can take microphone requests now.
  ///
  /// @return True when the session is running against a console or the cloud
  ///     and no rest mode request is under way.
  auto open() const noexcept -> bool;
  /// @brief Mutes or unmutes the microphone.
  ///
  /// The microphone starts muted as far as this object is concerned: push
  /// refuses frames until this has been called with false. Muting discards
  /// the frames still queued.
  ///
  /// @param muted True to mute, false to start sending.
  /// @return Nothing on success; the request is queued.
  /// @retval InputRefusal::not_running The session is not accepting requests.
  /// @retval InputRefusal::wrong_route The session has no known venue.
  /// @retval InputRefusal::not_ready A rest mode request is under way.
  [[nodiscard]] auto set_muted(bool muted) -> Expected<void, InputRefusal>;
  /// @brief Queues one encoded frame for sending.
  ///
  /// The frame is copied. When the queue is full the oldest frame is dropped
  /// and counted in Statistics::frames_dropped.
  ///
  /// @param frame One Opus frame of frame_size bytes.
  /// @return Nothing on success.
  /// @retval InputRefusal::not_running The session is not accepting requests.
  /// @retval InputRefusal::wrong_route The session has no known venue.
  /// @retval InputRefusal::not_ready The microphone is muted,
  ///     set_muted(false) has not been called yet, or a rest mode request is
  ///     under way.
  [[nodiscard]] auto push(std::span<const std::byte, frame_size> frame)
      -> Expected<void, InputRefusal>;
  /// @brief Returns a copy of the running totals.
  ///
  /// @return A copy of the running totals.
  auto statistics() const -> Statistics;

 private:
  Core* core_;
};

}  // namespace libchiaking::play
