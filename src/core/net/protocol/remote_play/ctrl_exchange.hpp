/// @file
/// @brief Declares the TCP side of the control channel at home: the startup
/// exchange that sends the `sess/ctrl` request and keeps its socket, and
/// CtrlFrameReader, which then reads and writes encrypted control frames on
/// it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/net/socket.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"

#include "auth_material.hpp"
#include "ctrl_codec.hpp"
#include "limits.hpp"
#include "tcp_exchange.hpp"

namespace libchiaking::net::protocol::remote_play {

/// @brief The socket and reply of an accepted control request, handed from
/// the startup exchange to the frame reader.
///
/// Move-only; owns the socket and the still-encrypted server type.
struct CtrlStartupConnection {
  /// @brief The connected TCP socket; control frames follow the reply head on
  /// it.
  std::unique_ptr<platform::TcpSocket> socket;
  /// @brief The console's grant.
  CtrlGrant response;
  /// @brief The first outbound cipher counter: the one after the request's
  /// encrypted headers.
  std::uint64_t next_local_crypt_counter{};

  /// @brief Makes an empty connection.
  CtrlStartupConnection() noexcept = default;
  CtrlStartupConnection(const CtrlStartupConnection&) = delete;
  CtrlStartupConnection& operator=(const CtrlStartupConnection&) = delete;
  /// @brief Takes over another connection.
  CtrlStartupConnection(CtrlStartupConnection&&) noexcept = default;
  /// @brief Takes over another connection, dropping what this held.
  CtrlStartupConnection& operator=(CtrlStartupConnection&&) noexcept = default;
};

/// @brief The Reply policy that lets TcpExchange run the control request.
///
/// The socket is kept for the control channel; the reply head is read one
/// byte at a time so no control frame after it is consumed; and when the head
/// does not arrive within rp_phase_timeout the request is sent once more
/// on a fresh connection. Not shared between threads.
class CtrlStartupReply {
 public:
  /// @brief The request type sent.
  using Request = CtrlStartupRequest;
  /// @brief The reply type parsed.
  using Response = CtrlGrant;
  /// @brief What take() hands over.
  using Taken = CtrlStartupConnection;

  /// @brief The socket outlives the exchange.
  static constexpr bool keeps_socket = true;
  /// @brief A reply head that does not arrive is retried once on a new
  /// connection.
  static constexpr bool retries_header_timeout = true;

  /// @brief Whether the policy can run.
  ///
  /// @return Always true.
  static constexpr auto valid() noexcept -> bool { return true; }

  /// @brief How many bytes to read next.
  ///
  /// @return Always 1, so reading stops exactly at the end of the head.
  static constexpr auto receive_window(std::size_t) noexcept -> std::size_t {
    return 1;
  }

  /// @brief Reads the reply received so far.
  ///
  /// @param received The bytes received so far.
  /// @return The reply once complete, or nothing while incomplete.
  /// @retval ConsoleExchangeError::too_large The head is too long.
  /// @retval ConsoleExchangeError::invalid_response The bytes are not a valid
  ///     reply.
  /// @retval ConsoleExchangeError::refused The console answered with a status
  ///     other than 200.
  [[nodiscard]] static auto parse(std::span<const std::byte> received)
      -> Expected<std::optional<CtrlGrant>, ConsoleExchangeError>;

  /// @brief Remembers the request's next cipher counter once it is sent.
  ///
  /// @param request The request that was delivered.
  void on_sent(const CtrlStartupRequest& request) noexcept {
    next_local_counter_ = request.next_crypt_counter();
  }

  /// @brief Hands over the socket and reply.
  ///
  /// @param socket The connected socket.
  /// @param response The console's grant.
  /// @return The connection for CtrlFrameReader::adopt().
  auto take(std::unique_ptr<platform::TcpSocket> socket,
            CtrlGrant response) const -> CtrlStartupConnection;

 private:
  /// @brief The counter learned from on_sent().
  std::uint64_t next_local_counter_{};
};

/// @brief Sends the control request over TCP and hands over its socket.
using CtrlStartupExchange = TcpExchange<CtrlStartupReply>;

/// @brief Why the control frame reader failed.
enum class CtrlReaderError : std::uint8_t {
  /// @brief A missing socket or a target with no RP version.
  invalid_argument,
  /// @brief The reader was already started or has ended, or nothing is queued
  /// to send.
  invalid_state,
  /// @brief The socket failed, or sent or received an impossible byte count.
  network_failure,
  /// @brief The console closed the connection.
  disconnected,
  /// @brief A frame declares more than ctrl_frame_max_payload, or the buffer
  /// filled without a whole frame.
  too_large,
  /// @brief The cipher failed while sealing an outbound frame.
  crypto_failure,
  /// @brief A cipher counter reached its maximum.
  counter_exhausted,
};

/// @brief One received control frame, decrypted.
///
/// The payload sits in wiped storage; move-only.
class CtrlPlainFrame {
 public:
  /// @brief Makes an empty frame.
  CtrlPlainFrame() noexcept = default;
  CtrlPlainFrame(const CtrlPlainFrame&) = delete;
  CtrlPlainFrame& operator=(const CtrlPlainFrame&) = delete;
  /// @brief Takes over another frame.
  CtrlPlainFrame(CtrlPlainFrame&&) noexcept = default;
  /// @brief Takes over another frame, wiping what this held.
  CtrlPlainFrame& operator=(CtrlPlainFrame&&) noexcept = default;

  /// @brief The message type.
  ///
  /// @return The 16-bit type.
  std::uint16_t type() const noexcept { return type_; }
  /// @brief The decrypted payload.
  ///
  /// @return A view into this frame.
  std::span<const std::byte> payload() const noexcept {
    return {payload_.data(), size_};
  }

 private:
  friend class CtrlFrameReader;
  /// @brief The message type.
  std::uint16_t type_{};
  /// @brief The decrypted payload; only the first size_ bytes are meaningful.
  SecretArray<ctrl_frame_max_payload> payload_;
  /// @brief The payload length, in bytes.
  std::size_t size_{};
};

/// @brief Reads and writes encrypted control frames on the TCP control
/// connection.
///
/// Inbound frames are decrypted at the remote counter, outbound frames sealed
/// at the local counter; each non-empty payload advances its counter by one. A
/// frame that fails to decrypt still consumes its counter and is counted in
/// dropped_frames(). Any fatal error cancels the reader: the socket is closed,
/// buffers and material are wiped, and the reader stays ended.
///
/// The socket is non-blocking; the owner polls pollable() and pumps. Not
/// thread-safe: one thread owns it.
class CtrlFrameReader {
 public:
  /// @brief Makes an idle reader.
  ///
  /// @param crypto The provider for the cipher; must outlive the reader.
  explicit CtrlFrameReader(platform::CryptoProvider& crypto) noexcept;
  /// @brief Cancels the reader, closing the socket.
  ~CtrlFrameReader();

  CtrlFrameReader(const CtrlFrameReader&) = delete;
  CtrlFrameReader& operator=(const CtrlFrameReader&) = delete;

  /// @brief Starts reading on a connected socket with known counters.
  ///
  /// @param socket The connected socket; closed when refused.
  /// @param target The console variant.
  /// @param material The session's material; taken over.
  /// @param initial_remote_counter The first inbound cipher counter.
  /// @param initial_local_counter The first outbound cipher counter.
  /// @return Nothing on success.
  /// @retval CtrlReaderError::invalid_state Already started or ended.
  /// @retval CtrlReaderError::invalid_argument The socket is missing or the
  ///     target unknown.
  [[nodiscard]] auto begin(std::unique_ptr<platform::TcpSocket> socket,
                           RegistrationTarget target, AuthMaterial&& material,
                           std::uint64_t initial_remote_counter,
                           std::uint64_t initial_local_counter)
      -> Expected<void, CtrlReaderError>;
  /// @brief Starts reading on the socket of an accepted control request,
  /// decrypting its server type first.
  ///
  /// @param connection The startup exchange's connection; taken over.
  /// @param target The console variant.
  /// @param material The session's material; taken over, and wiped on failure.
  /// @return The decrypted reply.
  /// @retval CtrlReaderError::invalid_state Already started or ended, or no
  ///     socket.
  [[nodiscard]] auto adopt(CtrlStartupConnection&& connection,
                           RegistrationTarget target, AuthMaterial&& material)
      -> Expected<CtrlResponseFinal, CtrlReaderError>;
  /// @brief Reads what the socket has and returns the next whole frame.
  ///
  /// Never blocks: returns nothing when no whole frame is available yet.
  ///
  /// @return The next frame, or nothing (also for a frame that failed to
  ///     decrypt).
  /// @retval CtrlReaderError::invalid_state Not started or ended.
  /// @retval CtrlReaderError::disconnected The console closed the connection.
  /// @retval CtrlReaderError::network_failure The socket failed.
  /// @retval CtrlReaderError::too_large A frame is too large.
  /// @retval CtrlReaderError::counter_exhausted The inbound counter is spent.
  [[nodiscard]] auto pump_once()
      -> Expected<std::optional<CtrlPlainFrame>, CtrlReaderError>;
  /// @brief Seals a message and appends it to the outbound bytes.
  ///
  /// @param type The message type.
  /// @param payload The plaintext payload; an empty one is not encrypted.
  /// @return Nothing on success; send with pump_send().
  /// @retval CtrlReaderError::invalid_state Not started or ended.
  /// @retval CtrlReaderError::too_large The payload does not fit the length.
  /// @retval CtrlReaderError::counter_exhausted The outbound counter is spent.
  /// @retval CtrlReaderError::crypto_failure The cipher failed; nothing was
  ///     queued.
  [[nodiscard]] auto queue_send(std::uint16_t type,
                                std::span<const std::byte> payload)
      -> Expected<void, CtrlReaderError>;
  /// @brief Writes as much of the queued bytes as the socket takes.
  ///
  /// Never blocks.
  ///
  /// @return True when everything queued has been sent, false when bytes
  ///     remain.
  /// @retval CtrlReaderError::invalid_state Not started, ended, or nothing
  ///     queued.
  /// @retval CtrlReaderError::network_failure The socket failed; the queue is
  ///     dropped.
  [[nodiscard]] auto pump_send() -> Expected<bool, CtrlReaderError>;
  /// @brief Ends the reader: closes the socket and wipes everything.
  ///
  /// @return Nothing, or CtrlReaderError::network_failure when the close
  ///     failed.
  [[nodiscard]] auto cancel() -> Expected<void, CtrlReaderError>;
  /// @brief The next inbound cipher counter.
  ///
  /// @return The counter; 0 once cancelled.
  std::uint64_t remote_counter() const noexcept { return remote_counter_; }
  /// @brief The next outbound cipher counter.
  ///
  /// @return The counter; 0 once cancelled.
  std::uint64_t local_counter() const noexcept { return local_counter_; }
  /// @brief How many inbound frames failed to decrypt and were skipped.
  ///
  /// @return The count.
  std::uint64_t dropped_frames() const noexcept { return dropped_frames_; }
  /// @brief What the owner polls for readiness.
  ///
  /// @return The socket, or null once ended.
  auto pollable() const noexcept -> const platform::Pollable* {
    return terminal_ ? nullptr : socket_.get();
  }

 private:
  /// @brief Decodes a frame from the buffer, receiving once when none is
  /// whole.
  ///
  /// @return As pump_once().
  [[nodiscard]] auto parse_or_receive()
      -> Expected<std::optional<CtrlPlainFrame>, CtrlReaderError>;
  /// @brief Cancels the reader and reports an error.
  ///
  /// @param error The error.
  /// @return The error.
  [[nodiscard]] auto fail(CtrlReaderError error)
      -> Expected<std::optional<CtrlPlainFrame>, CtrlReaderError>;
  /// @brief Closes the socket.
  ///
  /// @return Nothing, or CtrlReaderError::network_failure.
  [[nodiscard]] auto close_socket() -> Expected<void, CtrlReaderError>;
  /// @brief Forgets the queued outbound bytes.
  void drop_outbound() noexcept;
  /// @brief Removes a decoded frame from the front of the buffer, wiping the
  /// freed tail.
  ///
  /// @param consumed The frame's size.
  void consume(std::size_t consumed) noexcept;

  /// @brief The cipher provider.
  platform::CryptoProvider& crypto_;
  /// @brief The control socket.
  HeldTcpSocket socket_;
  /// @brief The session's material.
  AuthMaterial material_;
  /// @brief Received bytes not yet decoded.
  SecretArray<ctrl_frame_max_size> buffer_;
  /// @brief Sealed frames waiting to be sent.
  SecretBytes outbound_;
  /// @brief The console variant.
  RegistrationTarget target_{};
  /// @brief How many bytes of buffer_ hold data.
  std::size_t filled_{};
  /// @brief The next inbound cipher counter.
  std::uint64_t remote_counter_{};
  /// @brief The next outbound cipher counter.
  std::uint64_t local_counter_{};
  /// @brief How many bytes are queued in outbound_.
  std::size_t outbound_size_{};
  /// @brief How many queued bytes have been sent.
  std::size_t outbound_sent_{};
  /// @brief Frames that failed to decrypt.
  std::uint64_t dropped_frames_{};
  /// @brief Whether bytes are queued.
  bool sending_{false};
  /// @brief Whether begin() succeeded.
  bool started_{false};
  /// @brief Whether the reader has ended.
  bool terminal_{false};
};

}  // namespace libchiaking::net::protocol::remote_play
