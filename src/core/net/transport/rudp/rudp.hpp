/// @file
/// @brief Declares the RUDP session layer over a datagram transport: the
/// handshake and the runner that drives it, the connector, the reliable channel
/// that retransmits ctrl messages until acknowledged, and the drain that clears
/// a
/// route before reuse.
///
/// A PS5 reached over the internet speaks RUDP for its session init and ctrl
/// exchange before the stream switches to Takion. The handshake fixes the
/// packet counters each side starts from; FrameCodec holds the wire format.
#pragma once

#include <libchiaking/detail/wiping_allocator.hpp>
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/error.hpp"
#include "libchiaking/platform/net/poller.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/transport/datagram/transport.hpp"
#include "src/core/net/transport/datagram/udp_transport.hpp"
#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "src/core/net/transport/rudp/rudp_handshake_lifecycle.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace libchiaking::net::transport::datagram {
class ProviderUdpTransport;
}

namespace libchiaking::net::transport::rudp {

/// @brief The random values this side opens an RUDP handshake with.
struct HandshakeParameters {
  /// @brief The first packet counter, from 0x1FF to 0x5FFE.
  std::uint16_t counter;
  /// @brief The 32-bit header value sent in both requests.
  std::uint32_t header;
};

/// @brief What a finished handshake hands to the ctrl exchange that follows.
struct HandshakeCompletion {
  /// @brief This side's header value.
  std::uint32_t local_header;
  /// @brief The counter this side's next packet goes out with.
  std::uint16_t next_local_counter;
  /// @brief The console's next expected counter, to acknowledge.
  std::uint16_t remote_counter;
};

/// @brief One RUDP handshake attempt, turning datagrams and timeouts into the
/// next request to send.
///
/// Wraps the handshake_lifecycle machine and encodes its requests. Each call
/// returns a Step: a packet to send, nothing (keep waiting, or finished; ask
/// state()), or a failure. Datagrams tagged with another attempt id are counted
/// and ignored. Owns its machine; movable, not shared between threads.
class Handshake {
 public:
  /// @brief A handshake step: a packet to send, nothing to send, or why the
  /// handshake failed.
  using Step = Expected<std::optional<std::vector<std::byte>>, HandshakeError>;

  /// @brief Draws the opening counter and header value.
  ///
  /// Reads 8 random bytes as two big-endian 32-bit samples: the counter is
  /// the
  /// first modulo 0x5E00 plus 0x1FF, the header the second plus 0x8000 (mod
  /// 2^32).
  ///
  /// @param provider The source of randomness.
  /// @return The parameters.
  /// @retval platform::PlatformError The random source failed.
  [[nodiscard]] static auto generate_parameters(
      platform::SecureRandomProvider& provider)
      -> Expected<HandshakeParameters, platform::PlatformError>;

  /// @brief Prepares a handshake that has not begun.
  ///
  /// @param attempt_id The attempt datagrams must belong to.
  /// @param local_counter The counter of the first request.
  /// @param local_header This side's header value.
  Handshake(std::uint64_t attempt_id, std::uint16_t local_counter,
            std::uint32_t local_header);

  /// @brief The attempt this handshake belongs to.
  ///
  /// @return The id given at construction.
  std::uint64_t attempt_id() const;
  /// @brief Where the handshake stands.
  ///
  /// @return The current state.
  HandshakeState state() const;
  /// @brief This side's header value.
  ///
  /// @return The value given at construction.
  std::uint32_t local_header() const;
  /// @brief The counter the next packet goes out with.
  ///
  /// @return The opening counter plus the requests sent so far.
  std::uint16_t next_counter() const;
  /// @brief The console's next expected counter.
  ///
  /// @return The counter; nothing until the handshake is established.
  std::optional<std::uint16_t> remote_counter() const;
  /// @brief Requests sent in the current step.
  ///
  /// @return From 0 to rudp_max_sends.
  int sends_for_stage() const;
  /// @brief Whether a request is out and an answer is awaited.
  ///
  /// @return True in awaiting_init and awaiting_cookie.
  bool awaiting() const noexcept;

  /// @brief Starts the handshake.
  ///
  /// @return The init request to send.
  /// @retval HandshakeError::invalid_transition It had already begun.
  /// @retval HandshakeError::encode_failed The request could not be
  ///     encoded.
  [[nodiscard]] Step begin();

  /// @brief Feeds a received datagram.
  ///
  /// A datagram that is not the expected answer makes the same request go out
  /// again while sends remain.
  ///
  /// @param attempt The attempt the datagram was received for.
  /// @param packet The datagram.
  /// @return The next request to send; nothing when the datagram belonged to
  ///     another attempt, finished the handshake, or was ignored by the
  ///     current
  ///     state.
  /// @retval HandshakeError::network The datagram is no longer than one
  ///     frame header.
  /// @retval HandshakeError::retries_exhausted The step had no sends
  ///     left.
  /// @retval HandshakeError::encode_failed The next request could not be
  ///     encoded.
  [[nodiscard]] Step on_packet(std::uint64_t attempt,
                               std::span<const std::byte> packet);

  /// @brief Reports that the current request's answer is overdue.
  ///
  /// @return The request to send again; nothing when not awaiting.
  /// @retval HandshakeError::retries_exhausted The step had no sends
  ///     left.
  /// @retval HandshakeError::encode_failed The request could not be
  ///     encoded.
  [[nodiscard]] Step on_timeout();

  /// @brief Gives the handshake up.
  ///
  /// @return Nothing on success.
  /// @retval HandshakeError::invalid_transition It had already finished.
  [[nodiscard]] auto cancel() -> Expected<void, HandshakeError>;

 private:
  /// @brief Feeds one event and turns its outcome into a step.
  ///
  /// @tparam Event A handshake_lifecycle event type.
  /// @param event The event.
  /// @return The step; nothing when the current state had no transition for
  ///     the event.
  template <class Event>
  [[nodiscard]] auto advance(const Event& event) -> std::optional<Step>;
  /// @brief Encodes the request the machine asked for and records it sent.
  ///
  /// @param effect The request.
  /// @return The packet.
  /// @retval HandshakeError::encode_failed It could not be encoded; the
  ///     handshake has failed.
  [[nodiscard]] Step send(const handshake_lifecycle::Effect& effect);
  /// @brief Encodes a handshake request.
  ///
  /// @param effect The request.
  /// @return The packet.
  /// @retval FrameError Encoding failed.
  static auto encode(const handshake_lifecycle::Effect& effect)
      -> Expected<std::vector<std::byte>, FrameError>;
  /// @brief Reads whichever handshake answers a datagram holds.
  ///
  /// @param packet The datagram.
  /// @return The answers found; both empty when it holds neither.
  static auto read_reply(std::span<const std::byte> packet)
      -> handshake_lifecycle::Reply;

  /// @brief The attempt datagrams must belong to.
  std::uint64_t attempt_id_;
  /// @brief The machine, on the heap so the handshake can move; never null.
  std::unique_ptr<handshake_lifecycle::Lifecycle> lifecycle_;
};

/// @brief Why an RUDP connection could not be opened.
enum class ConnectError : std::uint8_t {
  /// @brief The opening parameters could not be drawn.
  random_failed,
  /// @brief The stop token fired or the handshake was cancelled.
  stopped,
  /// @brief A request could not be sent.
  send_failed,
  /// @brief A request was sent short.
  short_send,
  /// @brief Waiting or receiving failed, or a reply was truncated.
  receive_failed,
  /// @brief The console did not answer within the retry budget.
  timeout,
  /// @brief The handshake failed for any other reason.
  handshake_failed,
  /// @brief No transport was given.
  invalid_transport,
};

/// @brief The datagram channel RUDP exchanges run over: send one datagram,
/// receive one within a time limit.
///
/// Lets the handshake, ctrl exchange and reliable channel run over a real
/// transport, a reliable wrapper or a test double. Used from one thread at a
/// time.
class HandshakeChannel {
 public:
  /// @brief Releases the channel.
  virtual ~HandshakeChannel() = default;
  /// @brief Sends one datagram without blocking.
  ///
  /// @param datagram The datagram.
  /// @return The bytes sent.
  /// @retval net::transport::datagram::TransportError The send failed.
  [[nodiscard]] virtual auto send(std::span<const std::byte> datagram)
      -> Expected<std::size_t, net::transport::datagram::TransportError> = 0;
  /// @brief Waits for one datagram.
  ///
  /// Blocks for up to `timeout` or until `stop_token` stops.
  ///
  /// @param timeout Longest wait.
  /// @param stop_token Ends the wait early.
  /// @return The datagram; nothing on timeout, stop, or a datagram that was
  ///     dropped.
  /// @retval net::transport::datagram::TransportError Waiting or receiving
  ///     failed.
  [[nodiscard]] virtual auto receive_for(std::chrono::microseconds timeout,
                                         const platform::StopToken& stop_token)
      -> Expected<std::optional<net::transport::datagram::OwnedDatagram>,
                  net::transport::datagram::TransportError> = 0;
};

/// @brief Runs one Handshake to its end over a channel.
///
/// Sends each request, waits up to rudp_expect_timeout_us (1 second) for its
/// answer, and feeds replies and timeouts back, so the handshake takes at most
/// about 6 seconds (two steps of three sends). Borrows the channel and clock,
/// which must outlive it; use from one thread.
class HandshakeRunner {
 public:
  /// @brief Prepares the run.
  ///
  /// @param fsm The handshake, not begun.
  /// @param channel The channel; must outlive the runner.
  /// @param clock The monotonic clock the waits are timed on; must outlive
  ///     the
  ///     runner.
  /// @param stop_token Cancels the handshake.
  HandshakeRunner(Handshake fsm, HandshakeChannel& channel,
                  platform::ClockProvider& clock,
                  platform::StopToken stop_token);

  /// @brief Runs the handshake until it is established, fails or is stopped.
  ///
  /// Blocks for up to about 6 seconds.
  ///
  /// @return The counters to continue with.
  /// @retval ConnectError::stopped The stop token fired.
  /// @retval ConnectError::send_failed A send failed.
  /// @retval ConnectError::short_send A send was short.
  /// @retval ConnectError::receive_failed Receiving failed or a reply was
  ///     truncated.
  /// @retval ConnectError::timeout The console did not answer.
  /// @retval ConnectError::handshake_failed Any other failure.
  [[nodiscard]] auto run() -> Expected<HandshakeCompletion, ConnectError>;
  /// @brief Requests sent so far.
  ///
  /// @return The count across both steps.
  int total_sends() const noexcept { return total_sends_; }

 private:
  /// @brief Translates a handshake failure.
  ///
  /// @param error The failure.
  /// @return timeout for retries_exhausted, receive_failed for network,
  ///     handshake_failed otherwise.
  static auto connect_error(HandshakeError error) noexcept -> ConnectError;

  /// @brief Waits for the answer to the request just sent.
  ///
  /// Ignored datagrams do not extend the wait. Blocks until `deadline_us`.
  ///
  /// @param deadline_us When the answer is overdue, in monotonic
  ///     microseconds.
  /// @return The handshake's next step.
  /// @retval ConnectError::stopped The stop token fired.
  /// @retval ConnectError::receive_failed The channel failed.
  [[nodiscard]] auto await_reply(std::uint64_t deadline_us)
      -> Expected<Handshake::Step, ConnectError>;
  /// @brief Turns the final step into the run's result.
  ///
  /// @param step The last step, which asked for no send.
  /// @return The counters when established.
  /// @retval ConnectError::stopped The handshake was cancelled.
  /// @retval ConnectError The step's failure, translated;
  ///     handshake_failed
  ///     when it ended in any other state.
  [[nodiscard]] auto finish(const Handshake::Step& step) const
      -> Expected<HandshakeCompletion, ConnectError>;
  /// @brief Cancels the handshake after the stop token fired.
  ///
  /// @return ConnectError::stopped.
  auto stop() -> Unexpected<ConnectError>;

  /// @brief The handshake being run.
  Handshake fsm_;
  /// @brief The borrowed channel.
  HandshakeChannel& channel_;
  /// @brief The borrowed monotonic clock.
  platform::ClockProvider& clock_;
  /// @brief Cancels the run.
  platform::StopToken stop_token_;
  /// @brief Requests sent so far.
  int total_sends_ = 0;
};

/// @brief An HandshakeChannel over a ProviderUdpTransport.
///
/// Borrows the transport, which must outlive it. Not copyable. Use from one
/// thread at a time.
class ProviderRudpHandshakeChannel final : public HandshakeChannel {
 public:
  /// @brief Wraps a transport.
  ///
  /// @param transport The transport; must outlive the channel.
  explicit ProviderRudpHandshakeChannel(
      net::transport::datagram::ProviderUdpTransport& transport) noexcept;

  /// @brief Not copyable.
  ProviderRudpHandshakeChannel(const ProviderRudpHandshakeChannel&) = delete;
  /// @brief Not copyable.
  ///
  /// @return Never returns.
  ProviderRudpHandshakeChannel& operator=(const ProviderRudpHandshakeChannel&) =
      delete;

  /// @brief Sends one datagram through the transport.
  ///
  /// @param datagram The datagram.
  /// @return The bytes sent.
  /// @retval net::transport::datagram::TransportError The send failed.
  [[nodiscard]] auto send(std::span<const std::byte> datagram)
      -> Expected<std::size_t,
                  net::transport::datagram::TransportError> override;
  /// @brief Waits for the transport to become readable, then reads once and
  /// copies the datagram out of the transport's buffer.
  ///
  /// Blocks for up to `timeout` or until `stop_token` stops.
  ///
  /// @param timeout Longest wait.
  /// @param stop_token Ends the wait early.
  /// @return The datagram; nothing when stopped, on timeout, or when the
  ///     datagram was not from the peer.
  /// @retval net::transport::datagram::TransportError Waiting or receiving
  ///     failed.
  [[nodiscard]] auto receive_for(std::chrono::microseconds timeout,
                                 const platform::StopToken& stop_token)
      -> Expected<std::optional<net::transport::datagram::OwnedDatagram>,
                  net::transport::datagram::TransportError> override;

 private:
  /// @brief The borrowed transport; never null.
  net::transport::datagram::ProviderUdpTransport* transport_;
};

/// @brief Why a switch-to-stream request could not be sent.
enum class SwitchSendError : std::uint8_t {
  /// @brief The connection has no transport.
  invalid_transport,
  /// @brief The request could not be built.
  packet_failed,
  /// @brief The send failed.
  send_failed,
  /// @brief The send was short.
  short_send,
};

/// @brief An open RUDP connection: its transport and the counters to continue
/// with.
struct Connected {
  /// @brief The transport the handshake ran over; owned.
  std::unique_ptr<net::transport::datagram::ProviderUdpTransport> transport;
  /// @brief The counters the handshake settled on.
  HandshakeCompletion completion;
};

/// @brief Opens RUDP connections by running the handshake.
///
/// Stateless; every member is static and may be called from any thread on its
/// own channel.
class Connector {
 public:
  /// @brief Runs a handshake with freshly drawn parameters.
  ///
  /// Blocks for up to about 6 seconds.
  ///
  /// @param channel The channel to the console.
  /// @param random Draws the opening counter and header value.
  /// @param clock The monotonic clock the waits are timed on.
  /// @param stop_token Cancels the handshake.
  /// @param attempt The attempt id datagrams belong to.
  /// @return The counters to continue with.
  /// @retval ConnectError::random_failed The parameters could not be
  ///     drawn.
  /// @retval ConnectError Any failure of HandshakeRunner::run().
  [[nodiscard]] static auto connect_over_channel(
      HandshakeChannel& channel, platform::SecureRandomProvider& random,
      platform::ClockProvider& clock, platform::StopToken stop_token,
      std::uint64_t attempt) -> Expected<HandshakeCompletion, ConnectError>;

  /// @brief Runs a handshake with given parameters.
  ///
  /// Blocks for up to about 6 seconds.
  ///
  /// @param channel The channel to the console.
  /// @param params The opening counter and header value.
  /// @param clock The monotonic clock the waits are timed on.
  /// @param stop_token Cancels the handshake.
  /// @param attempt The attempt id datagrams belong to.
  /// @return The counters to continue with.
  /// @retval ConnectError Any failure of HandshakeRunner::run().
  [[nodiscard]] static auto connect_over_channel(HandshakeChannel& channel,
                                                 HandshakeParameters params,
                                                 platform::ClockProvider& clock,
                                                 platform::StopToken stop_token,
                                                 std::uint64_t attempt)
      -> Expected<HandshakeCompletion, ConnectError>;

  /// @brief Runs a handshake over a UDP transport and keeps the transport for
  /// what follows.
  ///
  /// Blocks for up to about 6 seconds.
  ///
  /// @param transport The transport to the console.
  /// @param random Draws the opening counter and header value.
  /// @param clock The monotonic clock the waits are timed on.
  /// @param stop_token Cancels the handshake.
  /// @param attempt The attempt id datagrams belong to.
  /// @return The open connection.
  /// @retval ConnectError::invalid_transport `transport` is null.
  /// @retval ConnectError Any failure of the handshake.
  [[nodiscard]] static auto connect(
      std::unique_ptr<net::transport::datagram::ProviderUdpTransport> transport,
      platform::SecureRandomProvider& random, platform::ClockProvider& clock,
      platform::StopToken stop_token, std::uint64_t attempt)
      -> Expected<Connected, ConnectError>;

  /// @brief Sends a switch-to-stream request on an open connection, once,
  /// without waiting for confirmation.
  ///
  /// @param connected The connection; consumed.
  /// @param random Draws the request's nonce.
  /// @return The connection with its next local counter raised by one.
  /// @retval SwitchSendError::invalid_transport The connection has no
  ///     transport.
  /// @retval SwitchSendError::packet_failed The request could not be
  ///     built.
  /// @retval SwitchSendError::send_failed The send failed.
  /// @retval SwitchSendError::short_send The send was short.
  [[nodiscard]] static auto send_switch_request(
      Connected connected, platform::SecureRandomProvider& random)
      -> Expected<Connected, SwitchSendError>;
};

/// @brief The ctrl messages sent but not yet acknowledged, held for
/// retransmission.
///
/// Each entry is keyed by the counter whose acknowledgement retires it (the
/// message's counter plus one). An acknowledgement of counter n retires every
/// entry whose key is within half the 16-bit range at or below n. Holds at most
/// 16 entries; their bytes are wiped when released, since ctrl messages carry
/// session secrets. Owned by the session and shared with each
/// ReliableChannel made over it; not thread-safe.
class SendBuffer {
 public:
  /// @brief Starts empty.
  SendBuffer() = default;
  /// @brief Not copyable: it holds session secrets.
  SendBuffer(const SendBuffer&) = delete;
  /// @brief Not copyable: it holds session secrets.
  ///
  /// @return Never returns.
  SendBuffer& operator=(const SendBuffer&) = delete;

  /// @brief Most messages held.
  static constexpr std::size_t capacity = 16;
  /// @brief How often held messages are checked for retransmission, in
  /// microseconds: 200 ms.
  static constexpr std::uint64_t poll_us = 200000;
  /// @brief How long a message waits after its last send before it is sent
  /// again, in microseconds: 400 ms, compared at millisecond granularity.
  static constexpr std::uint64_t retry_us = 400000;
  /// @brief Most retransmissions of one message before it, and all it covers,
  /// is dropped.
  static constexpr unsigned max_retries = 25;
  /// @brief Half the 16-bit counter range, the window an acknowledgement
  /// covers.
  static constexpr std::uint16_t sequence_half_range = 0x8000;

  /// @brief One held message.
  struct Packet {
    /// @brief The acknowledged counter that retires it: its counter plus
    /// one.
    std::uint16_t acknowledgement{};
    /// @brief Retransmissions so far.
    unsigned retries{};
    /// @brief When it was last sent, in monotonic microseconds.
    std::uint64_t last_send{};
    /// @brief The whole packet as sent; wiped on release.
    std::vector<std::byte, memory::WipingAllocator<std::byte>> bytes;
  };

  /// @brief Messages held.
  ///
  /// @return From 0 to capacity.
  auto size() const noexcept -> std::size_t { return packets_.size(); }
  /// @brief Whether a message with this key may be held.
  ///
  /// @param sequence Its acknowledgement key.
  /// @return False when the buffer is full or already holds that key.
  auto can_push(std::uint16_t sequence) const noexcept -> bool;
  /// @brief Holds a message just sent. The caller checks can_push() first.
  ///
  /// When the buffer was empty, the next retransmission check is scheduled
  /// poll_us from now.
  ///
  /// @param sequence Its acknowledgement key.
  /// @param bytes The packet as sent; copied.
  /// @param now When it was sent, in monotonic microseconds.
  void push(std::uint16_t sequence, std::span<const std::byte> bytes,
            std::uint64_t now);
  /// @brief Retires every message an acknowledgement covers.
  ///
  /// @param sequence The acknowledged counter.
  /// @return How many messages were retired.
  auto acknowledge(std::uint16_t sequence) -> std::size_t;

 private:
  /// @brief The reliable channel schedules and walks the held messages.
  friend class ReliableChannel;
  /// @brief The held messages, oldest first.
  std::deque<Packet> packets_;
  /// @brief When retransmissions are next checked, in monotonic microseconds.
  std::uint64_t next_poll_{};
};

/// @brief An HandshakeChannel that retransmits ctrl messages until the
/// console acknowledges them.
///
/// A sent packet that is exactly one ctrl_message frame is held in the
/// SendBuffer; every acknowledgement received retires what it covers, and
/// while waiting to receive, held messages older than 400 ms are sent again
/// every 200 ms, up to 25 times. Other packets pass through unheld. Borrows the
/// channel, buffer, clock and diagnostics, which must outlive it; use from one
/// thread.
class ReliableChannel final : public HandshakeChannel {
 public:
  /// @brief Wraps a channel.
  ///
  /// @param channel The underlying channel; must outlive this one.
  /// @param pending The buffer of held messages; must outlive this channel.
  /// @param clock The monotonic clock retransmissions are timed on.
  /// @param diagnostics Receives retransmission logs; may be null.
  ReliableChannel(HandshakeChannel& channel, SendBuffer& pending,
                  platform::ClockProvider& clock,
                  platform::DiagnosticsProvider* diagnostics = nullptr);

  /// @brief Sends a packet, holding it for retransmission when it is a single
  /// ctrl message.
  ///
  /// A ctrl message that cannot be held, because the buffer is full or
  /// already
  /// holds its key, is still sent and an error is logged.
  ///
  /// @param bytes The packet.
  /// @return The bytes sent.
  /// @retval net::transport::datagram::TransportError The send failed.
  auto send(std::span<const std::byte> bytes)
      -> Expected<std::size_t,
                  net::transport::datagram::TransportError> override;

  /// @brief Waits for one datagram, retransmitting held messages that are due
  /// while it waits, and retires what the datagram acknowledges.
  ///
  /// Blocks for up to `timeout` or until `stop` stops.
  ///
  /// @param timeout Longest wait; negative counts as zero.
  /// @param stop Ends the wait early.
  /// @return The datagram; nothing on timeout, stop, or a dropped datagram.
  /// @retval net::transport::datagram::TransportError The channel failed.
  auto receive_for(std::chrono::microseconds timeout,
                   const platform::StopToken& stop)
      -> Expected<std::optional<net::transport::datagram::OwnedDatagram>,
                  net::transport::datagram::TransportError> override;

 private:
  /// @brief Retires the held messages a received packet acknowledges.
  ///
  /// Reads the acknowledged counter at payload offset 2 of each ack frame.
  ///
  /// @param bytes The received packet.
  void retire_acknowledgements(std::span<const std::byte> bytes);
  /// @brief Sends again every held message that is due, when a check is due.
  ///
  /// A message past max_retries is dropped with everything its key covers.
  ///
  /// @param now The current monotonic instant, in microseconds.
  /// @param stop Ends the walk early.
  void service_retries(std::uint64_t now, const platform::StopToken& stop);
  /// @brief Writes one "remote RUDP" line to the diagnostics, if any.
  ///
  /// @param severity The line's severity.
  /// @param event What happened.
  /// @param count The number the event concerns.
  void log(platform::LogSeverity severity, std::string_view event,
           std::size_t count);
  /// @brief The borrowed underlying channel.
  HandshakeChannel& channel_;
  /// @brief The borrowed buffer of held messages.
  SendBuffer& pending_;
  /// @brief The borrowed monotonic clock.
  platform::ClockProvider& clock_;
  /// @brief Receives logs; may be null.
  platform::DiagnosticsProvider* diagnostics_;
};

/// @brief Why draining a transport failed.
enum class RemoteDatagramDrainError : std::uint8_t {
  /// @brief The poller could not be made or its wait failed.
  poller,
  /// @brief Receiving from the transport failed.
  receive
};

/// @brief Throws away datagrams already queued on a transport before it is
/// handed to the stream, so stale RUDP traffic is not read as Takion.
///
/// Stateless.
class RemoteDatagramDrain {
 public:
  /// @brief Longest a drain runs, in microseconds: 1 second.
  static constexpr std::uint64_t limit_us = 1'000'000;
  /// @brief How long the transport must stay silent for the drain to end: 200
  /// ms.
  static constexpr std::chrono::milliseconds quiet{200};

  /// @brief Reads and discards datagrams until the transport is quiet for
  /// 200 ms, 1 second has passed, or `stop` stops.
  ///
  /// Blocks for up to about 1.2 seconds.
  ///
  /// @param transport The transport to drain.
  /// @param network Makes the poller.
  /// @param clock The monotonic clock the limit is timed on.
  /// @param stop Ends the drain early.
  /// @return How many datagrams were discarded; 0 at once when the transport
  ///     cannot be polled.
  /// @retval RemoteDatagramDrainError::poller The poller could not be made or
  ///     the wait failed.
  /// @retval RemoteDatagramDrainError::receive Receiving failed.
  [[nodiscard]] static auto drain(
      net::transport::datagram::DatagramTransport& transport,
      platform::NetworkProvider& network, const platform::ClockProvider& clock,
      const platform::StopToken& stop)
      -> Expected<std::size_t, RemoteDatagramDrainError>;
};

}  // namespace libchiaking::net::transport::rudp
