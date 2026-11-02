/// @file
/// @brief Declares Client, one Takion session over one UDP socket: the
/// association handshake, version negotiation, the reliable control channel,
/// packet protection, media and controller packets, and link metrics.
///
/// Remote Play streams, LegacyCloudStream and CloudStream sessions and Senkusha
/// connection tests all run on it; the protocol layers above post commands and
/// react to its events.
#pragma once

#include "libchiaking/platform/net/poller.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/protocol/takion/client/inbound.hpp"
#include "src/core/net/protocol/takion/client/lifecycle.hpp"
#include "src/core/net/protocol/takion/client/outbound.hpp"
#include "src/core/net/protocol/takion/client_types.hpp"
#include "src/core/net/protocol/takion/control_codec.hpp"
#include "src/core/net/protocol/takion/generic_data_exchange.hpp"
#include "src/core/net/protocol/takion/handshake.hpp"
#include "src/core/net/protocol/takion/reliable_channel.hpp"
#include "src/core/net/protocol/takion/video_frame_metrics.hpp"

#include <rigtorp/MPMCQueue.h>
#include <boost/sml.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>

namespace libchiaking::net::protocol::takion {

/// @brief A Takion session.
///
/// start() picks a random verification tag (also the initial sequence
/// number) and sends INIT; the handshake completes over later pump_once()
/// calls, after which the reliable channel exists and a fixed version is
/// committed at once or the negotiation offer is sent. Commands are queued by
/// post() and executed by the next pump_once() or flush_commands(), in order.
/// pump_once() also retries datagrams the socket refused, resends what is due
/// and reads up to `maximum_datagrams_per_pump` datagrams, so the owner calls
/// it whenever the socket is readable or a deadline passes.
///
/// The thread that pumps owns the session. pump_once(), flush_commands(),
/// start(), the stops, release_transport(), note_readiness() and the
/// destructor are owner calls: they take the owner lock, which nothing else
/// takes, and should come from the pumping thread. post() takes no lock: it
/// pushes onto a bounded lock-free ring, which owner calls drain oldest first
/// before they run anything posted later, so each poster's commands run in
/// the order posted, and a full ring refuses at once. The readers
/// (snapshot(), next_deadline_us(), crypto_installed()) never wait for a
/// pump: they read what owner calls published, the lifecycle state after
/// every transition and key installation as atomics, the rest as a copy each
/// owner call leaves under a lock held only for the copy. Events reach the
/// sink synchronously during owner calls; the sink may post and read but must
/// not make an owner call. Nothing a pump does waits: a send the socket
/// refuses with would-block is kept in a bounded queue of unsent datagrams and
/// retried by later pumps, and later sends queue behind it. Only
/// request_stop_with_disconnect() waits, by request. Not copyable or movable.
/// The clock, threading provider, random source, sink, source tap and crypto
/// provider must outlive it.
class Client final : private client_detail::ClientInboundHost {
 public:
  /// @brief Prepares a session; nothing is sent until start().
  ///
  /// The configuration is validated now; an invalid one, or a null transport,
  /// makes start() fail with invalid_configuration.
  ///
  /// @param transport The socket, connected to the console or server.
  /// @param clock The monotonic clock; read from every thread that calls
  ///     next_deadline_us().
  /// @param threading Provider of the owner and snapshot locks.
  /// @param random The source of the verification tag.
  /// @param events Receives the session's events.
  /// @param config How the session is run.
  Client(std::unique_ptr<net::transport::datagram::DatagramTransport> transport,
         platform::ClockProvider& clock, platform::ThreadingProvider& threading,
         platform::SecureRandomProvider& random, EventSink& events,
         ClientConfig config = {});
  /// @brief Closes the socket if no stop or failure has, without reporting
  /// Closed; an owner call. Commands still in the ring are dropped.
  ~Client() override;

  /// @brief Not copyable.
  Client(const Client&) = delete;
  /// @brief Not copy-assignable.
  Client& operator=(const Client&) = delete;
  /// @brief Not movable; the receive path refers back to the client.
  Client(Client&&) = delete;
  /// @brief Not move-assignable.
  Client& operator=(Client&&) = delete;

  /// @brief Starts the handshake: picks the tag and sends the first INIT; an
  /// owner call.
  ///
  /// If that INIT cannot be sent the client fails, reporting TerminalFailure,
  /// but this call still succeeds; an INIT the socket refuses with
  /// would-block is kept and retried by the next pump.
  ///
  /// @return Nothing.
  /// @retval ClientError::invalid_configuration The configuration or
  ///     transport is unusable.
  /// @retval ClientError::invalid_state The client was already started.
  /// @retval ClientError::random_failure No tag could be drawn; the
  /// client
  ///     stays idle and may be started again.
  [[nodiscard]] Result<void> start();
  /// @brief Queues a command for the next pump without taking a lock or
  /// waiting for a pump; callable from any thread, the sink included.
  ///
  /// The command runs on a later owner call, after every command queued
  /// before it, and its outcome is reported through the sink. A command
  /// queued while the client stops is dropped unrun.
  ///
  /// @param command The command; left untouched when it is refused.
  /// @return Nothing.
  /// @retval ClientError::invalid_state The client is idle,
  ///     disconnecting, stopping, closed or failed, as last published.
  /// @retval ClientError::mailbox_full The ring holds
  ///     `mailbox_capacity` commands.
  [[nodiscard]] Result<void> post(TakionCommand command);
  /// @brief Drops queued commands, closes the socket and reports Closed; an
  /// owner call.
  ///
  /// @return Nothing; a stop of a closed or failed client does nothing.
  /// @retval ClientError The socket failed to close; the client failed.
  [[nodiscard]] Result<void> request_stop();
  /// @brief Drops queued commands, sends a DISCONNECT when connected (and
  /// allowed before keys), waits for it to be acknowledged, then stops as
  /// request_stop() does; an owner call.
  ///
  /// While a DISCONNECT fragment is unacknowledged the client is
  /// disconnecting: it keeps the socket open, reads only control packets,
  /// resends what is due and waits on the transport between passes, until
  /// nothing is outstanding or `drain` has passed. A transport that
  /// cannot wait, a clock that does not move or a failure ends the wait early.
  /// The owner lock is held throughout, so other owner calls wait for the
  /// drain; post() and the readers do not.
  ///
  /// @param message The DISCONNECT.
  /// @param drain The longest wait for the acknowledgement; 0, the default,
  ///     closes at once.
  /// @return Nothing.
  /// @retval ClientError The DISCONNECT could not be sent, the drain
  ///     failed, or the stop failed.
  [[nodiscard]] Result<void> request_stop_with_disconnect(
      Disconnect message,
      std::chrono::microseconds drain = std::chrono::microseconds{0});
  /// @brief Records what a poll of the transport found, so the next
  /// pump_once() skips reading the socket when the poll found nothing to
  /// read; an owner call.
  ///
  /// The note is used by the next pump only, and only when `polled` is the
  /// transport's own pollable and the transport's readiness is exact; any
  /// other note is dropped and the pump reads as usual.
  ///
  /// @param polled What was polled.
  /// @param readable Whether the poll found it readable.
  void note_readiness(const platform::Pollable& polled, bool readable);
  /// @brief Reads when pump_once() next has work that no datagram brings,
  /// without waiting for a pump; callable from any thread.
  ///
  /// @return Monotonic microseconds: the current time when commands are
  ///     queued; otherwise as the last owner call left it: the current time
  ///     of that call when held media can be replayed or a metrics report is
  ///     due, else the earliest of the unsent-datagram retry, the handshake
  ///     resend, the next resend pass, the generic-data timers, the video
  ///     report deadline and, while disconnecting, the drain deadline.
  ///     Nothing when no timer runs.
  std::optional<std::uint64_t> next_deadline_us() const;
  /// @brief Hands the socket over still open and stops the session, reporting
  /// Closed; used to reuse a measured route. An owner call.
  ///
  /// @return The socket.
  /// @retval ClientError::invalid_state It was already handed over.
  /// @retval ClientError The stop failed.
  [[nodiscard]] ClientResult<
      std::unique_ptr<net::transport::datagram::DatagramTransport>>
  release_transport();
  /// @brief Reads the session's state, queue, reliable channel and link
  /// metrics without waiting for a pump; callable from any thread.
  ///
  /// @return A copy of what the last owner call published, with the queued
  ///     command count read now.
  Snapshot snapshot() const;
  /// @brief Reads whether the session keys are installed, as the last owner
  /// call left it, without waiting for a pump.
  ///
  /// @return True once keys are installed.
  bool crypto_installed() const;

  /// @brief Moves the session along: retries datagrams the socket refused,
  /// runs queued commands, replays media held for keys, times out the
  /// handshake, polls generic data, resends due fragments, sends due metrics
  /// reports, reads datagrams, and commits a negotiated version once the
  /// channel is quiet. An owner call that never waits for the socket.
  ///
  /// Reading is skipped when note_readiness() last reported the socket not
  /// readable. While disconnecting, it runs one drain pass instead. Ends by
  /// publishing what the readers see.
  ///
  /// @return Nothing while the session runs, and once it is closed.
  /// @retval ClientError::invalid_state The client is idle or stopping.
  /// @retval ClientError The error the client has failed with, now or
  ///     earlier.
  [[nodiscard]] Result<void> pump_once();
  /// @brief Runs queued commands without reading the socket; an owner call.
  ///
  /// @return Nothing.
  /// @retval ClientError::invalid_state The client is idle or stopping.
  /// @retval ClientError The client's terminal error, or a refused
  ///     command's error.
  [[nodiscard]] Result<void> flush_commands();

 private:
  /// @brief How a pass of reads ended.
  enum class ReceivePass : std::uint8_t {
    /// @brief The transport had nothing more to give.
    drained,
    /// @brief `maximum_datagrams_per_pump` were read; more may wait.
    budget_spent,
  };

  /// @brief Commits a negotiated version once the channel is quiet and
  /// expires a negotiation that waited too long; the end of a pump.
  ///
  /// @return Nothing.
  /// @retval ClientError Committing failed; the client failed.
  ClientResult<void> finish_pass();
  /// @brief Does the work of pump_once() with the owner lock held.
  ///
  /// @return As pump_once().
  ClientResult<void> pump();
  /// @brief Does the work of flush_commands() with the owner lock held.
  ///
  /// @return As flush_commands().
  ClientResult<void> flush();
  /// @brief Offers the unsent datagrams to the socket again and reports
  /// those dropped as PacketRejected.
  ///
  /// @return Nothing.
  /// @retval ClientError The socket is closed; the client failed.
  ClientResult<void> retry_unsent();
  /// @brief Publishes what the readers see: key installation, the next
  /// deadline and the snapshot, the last under the snapshot lock held only
  /// for the copy.
  void publish();
  /// @brief Gathers a snapshot of the owner's state.
  ///
  /// @return The snapshot, with no queued command count.
  Snapshot gather_snapshot() const;
  /// @brief Drops every command in the ring unrun.
  void discard_commands();
  /// @brief Counts the commands in the ring now.
  ///
  /// @return The count; may be stale by the time it is read.
  std::size_t queued_command_count() const noexcept;
  /// @brief Feeds an event to the lifecycle machine and publishes the state
  /// it leaves.
  ///
  /// @tparam Input A lifecycle event type.
  /// @param event The event.
  template <class Input>
  void process(const Input& event) {
    lifecycle_machine_.process_event(event);
    published_state_.store(lifecycle_state(), std::memory_order_release);
  }
  /// @brief Resends the reliable fragments that are due and reports those
  /// given up on.
  ///
  /// @param now The current time, in monotonic microseconds.
  /// @return Nothing; transient send failures become PacketRejected.
  /// @retval ClientError A send failed for good; the client failed.
  ClientResult<void> resend_due(std::uint64_t now);
  /// @brief Reads and processes datagrams until the transport has none left or
  /// `limit` were read.
  ///
  /// @param scope Which packets are processed.
  /// @param limit The most datagrams to read.
  /// @return How the reads ended.
  /// @retval ClientError The transport or a datagram failed the client.
  ClientResult<ReceivePass> receive_datagrams(client_detail::InboundScope scope,
                                              std::size_t limit);
  /// @brief Describes the session to the receive path.
  ///
  /// @param scope Which packets are processed.
  /// @return The profile, association, reliable channel and keys in use.
  client_detail::InboundSession inbound_session(
      client_detail::InboundScope scope) const;
  /// @brief How many drain passes in a row may see the clock stand still
  /// before the drain gives up, as it cannot be timed.
  static constexpr std::size_t drain_still_pass_limit = 16;

  /// @brief Services a DISCONNECT drain until it settles, waiting on the
  /// transport between passes until the next deadline; gives up when the
  /// clock does not move across drain_still_pass_limit passes.
  ///
  /// @return Nothing once the drain settled or the client left
  ///     disconnecting.
  /// @retval ClientError A pass failed the client.
  ClientResult<void> drain_disconnect();
  /// @brief Runs one drain pass: resends what is due, reads control packets,
  /// and settles the drain once nothing is outstanding or its deadline passed.
  ///
  /// @return Nothing.
  /// @retval ClientError The pass failed the client.
  ClientResult<void> service_drain();
  /// @brief Ends the current drain and closes the socket.
  ///
  /// @return Nothing; nothing happens without a drain.
  /// @retval ClientError Closing failed.
  ClientResult<void> settle_drain();
  /// @brief Computes the next deadline from the owner's state.
  ///
  /// @return As next_deadline_us().
  std::optional<std::uint64_t> due_us() const;
  /// @brief Gathers the link metrics for a snapshot.
  ///
  /// @return The metrics; features and per-feature metrics only once a version
  ///     is committed.
  LinkMetrics link_metrics() const noexcept;
  /// @brief Maps the lifecycle machine to the public state.
  ///
  /// @return The state.
  ClientState lifecycle_state() const;
  /// @brief Runs up to `maximum_commands_per_pump` queued commands, oldest
  /// first.
  ///
  /// @return Nothing.
  /// @retval ClientError A refused command's error.
  ClientResult<void> drain_commands();
  /// @brief Fails the client for good.
  ///
  /// @param error Why.
  /// @return `error`, as a failure.
  ClientResult<void> fail_terminal(ClientError error);
  /// @brief Runs the lifecycle's queued effects.
  ///
  /// @return Nothing.
  /// @retval ClientError A refusal one of them produced.
  ClientResult<void> run_effects();
  /// @brief Draws the verification tag and feeds it to the lifecycle.
  ///
  /// @return Always nothing; a failure becomes an event.
  std::optional<ClientError> run_effect(client_detail::GenerateStartMaterial);
  /// @brief Starts the handshake and sends the first INIT.
  ///
  /// @param handshake The tag, sequence number and time.
  /// @return Always nothing; a failure becomes an event.
  std::optional<ClientError> run_effect(
      client_detail::BeginHandshake handshake);
  /// @brief Creates the reliable channel, reports Connected, and commits a
  /// fixed version or sends the negotiation offer.
  ///
  /// @param activation What was agreed.
  /// @return Always nothing; a failure becomes an event.
  std::optional<ClientError> run_effect(
      client_detail::ActivateSession activation);
  /// @brief Executes a command.
  ///
  /// @param command The command.
  /// @return An error to refuse the current call with, or nothing.
  std::optional<ClientError> run_effect(TakionCommand command);
  /// @brief Closes the socket, if still held, and completes the stop.
  ///
  /// @param close Whether to report Closed.
  /// @return Always nothing; a failure becomes an event.
  std::optional<ClientError> run_effect(client_detail::CloseTransport close);
  /// @brief Reports TerminalFailure and closes the socket once.
  ///
  /// @param failure Why.
  /// @return Always nothing.
  std::optional<ClientError> run_effect(
      client_detail::PublishTerminalFailure failure);
  /// @brief Reports Closed.
  ///
  /// @return Always nothing.
  std::optional<ClientError> run_effect(client_detail::PublishClosed);
  /// @brief Refuses the current call.
  ///
  /// @param refusal Why.
  /// @return The error.
  std::optional<ClientError> run_effect(client_detail::RefuseRequest refusal);
  /// @brief Sizes and creates the reliable channel and control codec for an
  /// established association.
  ///
  /// @param handshake What was agreed.
  /// @return Nothing.
  /// @retval ClientError::invalid_configuration The budgets leave no
  /// room.
  /// @retval ClientError Committing or sending the offer failed.
  ClientResult<void> activate_session(const EstablishedHandshake& handshake);
  /// @brief Installs the session's keys, hands the receive keys to the source
  /// tap, and sends the acknowledgements held for them.
  ///
  /// @param command The keys and positions.
  /// @return Nothing.
  /// @retval ClientError::crypto_failure Installation failed.
  /// @retval ClientError A held acknowledgement could not be sent.
  ClientResult<void> install_crypto(InstallCrypto command);
  /// @brief Checks a command against the state and queues it as an effect.
  ///
  /// Commands other than InstallCrypto and SendCongestion need a connected
  /// client with a committed version; a command that breaks that fails the
  /// client.
  ///
  /// @param command The command.
  /// @return Nothing.
  /// @retval ClientError A refusal.
  ClientResult<void> process_command(TakionCommand command);
  /// @brief Sends a control message, tolerating transient failures.
  ///
  /// @param command The message, id and deadline.
  /// @return An error to refuse with: control_codec_failure or
  ///     crypto_not_installed; others fail the client or become events.
  std::optional<ClientError> execute(SendControl command);
  /// @brief Sends a Senkusha probe and reports ProbeSent.
  ///
  /// @param command The probe.
  /// @return An error to refuse with: packet_encode_failure; others fail the
  ///     client or become events.
  std::optional<ClientError> execute(SendProbe command);
  /// @brief Sends a congestion report.
  ///
  /// @param command The report.
  /// @return An error to refuse with: packet_encode_failure.
  std::optional<ClientError> execute(SendCongestion command);
  /// @brief Sends the controller state.
  ///
  /// @param command The state.
  /// @return An error to refuse with: packet_encode_failure.
  std::optional<ClientError> execute(SendFeedbackState command);
  /// @brief Sends controller events.
  ///
  /// @param command The events.
  /// @return An error to refuse with: packet_encode_failure.
  std::optional<ClientError> execute(SendFeedbackHistory command);
  /// @brief Sends a microphone packet.
  ///
  /// @param command The packet.
  /// @return An error to refuse with: packet_encode_failure.
  std::optional<ClientError> execute(SendMicrophone command);
  /// @brief Sends a length-prefixed packet.
  ///
  /// @param command The payload.
  /// @return An error to refuse with: packet_encode_failure or
  ///     crypto_not_installed.
  std::optional<ClientError> execute(SendLengthPrefixed command);
  /// @brief Sends a GKTRACE packet.
  ///
  /// @param command The sequence.
  /// @return An error to refuse with: packet_encode_failure or
  ///     crypto_not_installed.
  std::optional<ClientError> execute(SendTrace command);
  /// @brief Queues a generic-data blob.
  ///
  /// @param command The blob.
  /// @return An error to refuse with: packet_encode_failure or
  ///     crypto_not_installed.
  std::optional<ClientError> execute(SendGenericData command);
  /// @brief Sends an extended message.
  ///
  /// @param command The body.
  /// @return An error to refuse with: packet_encode_failure or
  ///     unsupported_version.
  std::optional<ClientError> execute(SendExtendedMessage command);
  /// @brief Installs keys; a failure fails the client.
  ///
  /// @param command The keys.
  /// @return Always nothing.
  std::optional<ClientError> execute(InstallCrypto command);
  /// @brief Does nothing; Stop is handled before execution.
  ///
  /// @return Always nothing.
  std::optional<ClientError> execute(Stop);
  /// @brief Carries out what the handshake asked for.
  ///
  /// @param action The action.
  /// @return Nothing.
  /// @retval ClientError::handshake_failure The handshake failed.
  /// @retval ClientError A send failure.
  ClientResult<void> handle_handshake_action(const HandshakeAction& action);
  /// @brief Sends a reliable fragment, resending a recorded datagram unchanged.
  ///
  /// @param fragment The fragment.
  /// @param encrypted_tail Bytes at the end of a first send encrypted as a
  ///     direct message's data; 0 for none.
  /// @return Nothing.
  /// @retval ClientError::invalid_state No association.
  /// @retval ClientError A send failure.
  ClientResult<void> send_fragment(const OutboundFragment& fragment,
                                   std::size_t encrypted_tail = 0);
  /// @brief Sends a DATA_ACK, or holds it until keys are installed when plain
  /// control is allowed before them (keeping the newest
  /// `receive_reorder_size`).
  ///
  /// @param acknowledgement What to acknowledge.
  /// @return Nothing.
  /// @retval ClientError::invalid_state No association.
  /// @retval ClientError A non-transient send failure.
  ClientResult<void> send_ack(const DataAcknowledgement& acknowledgement);
  /// @brief Tells whether acknowledgements must wait for keys.
  ///
  /// @return Whether crypto is required, plain control is allowed before keys,
  ///     a version is committed and keys are not installed.
  bool acks_await_crypto() const noexcept;
  /// @brief Sends the acknowledgements held for keys.
  ///
  /// @return Nothing.
  /// @retval ClientError A send failure; the rest are dropped.
  ClientResult<void> flush_deferred_acks();
  /// @brief Turns a transient send failure into a PacketRejected event.
  ///
  /// @param result The send result.
  /// @return Nothing for success or a transient failure; the failure otherwise.
  ClientResult<void> tolerate_transient_send(ClientResult<void> result);
  /// @brief Settles a command's send result: transient failures become events,
  /// listed ones refuse the call, others fail the client.
  ///
  /// @param result The send result.
  /// @param recoverable Errors that refuse the call without failing the client.
  /// @return The error to refuse with, or nothing.
  std::optional<ClientError> settle_send(
      ClientResult<void> result,
      std::initializer_list<ClientError> recoverable);
  /// @brief Queues a generic-data blob and runs the resulting actions.
  ///
  /// @param command The blob.
  /// @return Nothing.
  /// @retval ClientError::packet_encode_failure The version has no
  ///     generic data or the blob was refused.
  /// @retval ClientError A failure of the actions.
  ClientResult<void> submit_generic_data(SendGenericData command);
  /// @brief Carries out the generic-data actions: sends control messages and
  /// chunks, and reports deliveries and ended transfers.
  ///
  /// @return Nothing; transient and encoding failures become events.
  /// @retval ClientError Any other send failure.
  ClientResult<void> run_generic_data_actions();
  /// @brief Encodes a control message and sends it on its channel: Senkusha on
  /// the Senkusha channel, STREAMINFOACK on its channel, CORRUPTFRAME and
  /// IDRREQUEST on channel 2, the rest on the control channel. Only the BIG is
  /// split across fragments.
  ///
  /// @param message The message.
  /// @param operation_id An id to follow it by.
  /// @return Nothing.
  /// @retval ClientError::invalid_state No reliable channel.
  /// @retval ClientError::crypto_not_installed Keys are required and the
  ///     message is not allowed before them.
  /// @retval ClientError::control_codec_failure It cannot be encoded or
  /// is
  ///     too large for one fragment.
  /// @retval ClientError See send_protobuf().
  ClientResult<void> send_control(
      const ControlMessage& message,
      std::optional<ControlOperationId> operation_id = std::nullopt);
  /// @brief Sends an encoded protobuf message through the reliable channel and
  /// reports ControlSent when every fragment left.
  ///
  /// @param encoded The message bytes.
  /// @param channel The channel.
  /// @param operation_id An id to follow it by.
  /// @param fragmentation Whether it may be split.
  /// @return Nothing; transient fragment failures become PacketRejected.
  /// @retval ClientError::invalid_state No reliable channel.
  /// @retval ClientError::reliable_channel_failure The channel refused
  /// it.
  /// @retval ClientError A non-transient send failure.
  ClientResult<void> send_protobuf(
      std::span<const std::byte> encoded, std::uint16_t channel,
      std::optional<ControlOperationId> operation_id,
      MessageFragmentation fragmentation);
  /// @brief Feeds an INIT_ACK to the handshake.
  ///
  /// @param chunk The chunk.
  /// @return Nothing.
  /// @retval ClientError The handshake failed or a send failed.
  ClientResult<void> inbound_handle_init_ack(
      const InitAcknowledgementChunk& chunk) override;
  /// @brief Feeds a COOKIE_ACK to the handshake.
  ///
  /// @param chunk The chunk.
  /// @return Nothing.
  /// @retval ClientError The handshake failed or a send failed.
  ClientResult<void> inbound_handle_cookie_ack(
      const CookieAcknowledgementChunk& chunk) override;
  /// @brief Sends a DATA_ACK for the receive path.
  ///
  /// @param acknowledgement What to acknowledge.
  /// @return Nothing.
  /// @retval ClientError See send_ack().
  ClientResult<void> inbound_send_ack(
      const DataAcknowledgement& acknowledgement) override;
  /// @brief Sends a control message for the receive path.
  ///
  /// @param message The message.
  /// @return Nothing.
  /// @retval ClientError See send_control().
  ClientResult<void> inbound_send_control(
      const ControlMessage& message) override;
  /// @brief Resends a fragment for the receive path, tolerating transient
  /// failures.
  ///
  /// @param fragment The fragment.
  /// @return Nothing.
  /// @retval ClientError A non-transient send failure.
  ClientResult<void> inbound_send_fragment(
      const OutboundFragment& fragment) override;
  /// @brief Starts traffic-class counting once, when the version has the
  /// feature, the session is connected with keys, and the socket can read
  /// traffic classes.
  ///
  /// @param now The current time, in monotonic microseconds.
  void enable_received_tos_metrics(std::uint64_t now);
  /// @brief Counts a received datagram's traffic class and sends a report when
  /// due.
  ///
  /// @param datagram The datagram.
  void account_received_tos(std::span<const std::byte> datagram);
  /// @brief Sends a video frame metrics report when due, for versions with the
  /// feature and once keys are installed.
  ///
  /// @param now The current time, in monotonic microseconds.
  void run_video_frame_reports(std::uint64_t now);
  /// @brief Sends an extended message.
  ///
  /// @param message The body.
  /// @return Nothing.
  /// @retval ClientError::unsupported_version The version has no
  ///     extended messages.
  /// @retval ClientError See ClientOutbound::send_extended_message().
  ClientResult<void> send_extended_message(std::span<const std::byte> message);
  /// @brief Sends an extended message for the receive path, tolerating
  /// transient failures.
  ///
  /// @param message The body.
  /// @return Nothing.
  /// @retval ClientError A non-transient send failure.
  ClientResult<void> inbound_send_extended_message(
      std::span<const std::byte> message) override;
  /// @brief Hands a GENERICDATA control message to the exchange.
  ///
  /// @param control The message.
  /// @return Nothing; a refused message becomes PacketRejected.
  /// @retval ClientError A failure of the resulting actions.
  ClientResult<void> inbound_generic_data_control(
      const GenericDataControl& control) override;
  /// @brief Hands a generic-data chunk to the exchange.
  ///
  /// @param chunk The chunk.
  /// @return Nothing; a refused chunk becomes PacketRejected.
  /// @retval ClientError A failure of the resulting actions.
  ClientResult<void> inbound_generic_data_chunk(
      GenericDataChunk chunk) override;
  /// @brief Fails the client for the receive path.
  ///
  /// @param error Why.
  void inbound_fail_terminal(ClientError error) override;
  /// @brief Tells the receive path whether the client is connected.
  ///
  /// @return Whether it is.
  bool inbound_connected() const override;
  /// @brief Commits a version: switches the framing and control codec to it
  /// unless the bootstrap framing is kept, and reports ProfileCommitted.
  ///
  /// @param version The version.
  /// @return Nothing.
  /// @retval ClientError::unsupported_version It is not implemented.
  /// @retval ClientError::invalid_configuration Its budgets do not match
  ///     the reliable channel.
  ClientResult<void> commit_profile(std::uint32_t version);
  /// @brief Reports a non-fatal failure.
  ///
  /// @param error Why.
  void reject(ClientError error);
  /// @brief Checks the transport and configuration.
  ///
  /// @param transport The transport.
  /// @param config The configuration.
  /// @return Whether the transport is present, the configuration valid, and the
  ///     three channels distinct.
  static bool valid_configuration(
      const net::transport::datagram::DatagramTransport* transport,
      const ClientConfig& config);

  /// @brief The next deadline published when none runs.
  static constexpr std::uint64_t no_deadline =
      std::numeric_limits<std::uint64_t>::max();

  /// @brief Serialises the owner calls; post() and the readers never take it.
  platform::Mutex owner_mutex_;
  /// @brief The socket; null once handed over.
  std::unique_ptr<net::transport::datagram::DatagramTransport> transport_;
  /// @brief The monotonic clock.
  platform::ClockProvider& clock_;
  /// @brief The source of the verification tag.
  platform::SecureRandomProvider& random_;
  /// @brief Receives the events.
  EventSink& events_;
  /// @brief How the session is run.
  ClientConfig config_;
  /// @brief The lifecycle's memory.
  client_detail::LifecycleContext lifecycle_context_;
  /// @brief The lifecycle machine.
  boost::sml::sm<client_detail::LifecycleFsm> lifecycle_machine_;
  /// @brief The association handshake.
  ConnectionHandshake handshake_;
  /// @brief Encodes and decodes control messages; resized on activation and
  /// commit.
  ControlCodec control_codec_;
  /// @brief Key positions and cipher directions.
  CryptoContext crypto_;
  /// @brief The profile packets are framed with: the bootstrap one, then the
  /// committed one unless the bootstrap framing is kept.
  std::optional<WireProfile> profile_;
  /// @brief The association, once up.
  std::optional<EstablishedHandshake> established_;
  /// @brief The reliable channel, once up.
  std::unique_ptr<ReliableChannel> reliable_;
  /// @brief Commands posted and not yet run, oldest first; pushed from any
  /// thread, popped only by owner calls.
  rigtorp::MPMCQueue<TakionCommand> commands_;
  /// @brief Acknowledgements held until keys are installed.
  std::deque<DataAcknowledgement> deferred_acks_;
  /// @brief Fragments sent untracked because the window was full.
  std::uint64_t untracked_fragments_{};
  /// @brief Generic-data transfers.
  GenericDataExchange generic_data_;
  /// @brief The send path.
  client_detail::ClientOutbound outbound_;
  /// @brief The receive path.
  client_detail::ClientInbound inbound_;
  /// @brief Decides when video frame metrics are reported; created once
  /// reporting begins.
  std::optional<VideoFrameReportWorker> video_report_worker_;
  /// @brief Whether starting traffic-class counting was tried.
  bool tos_enable_attempted_{};
  /// @brief What the last poll of the transport found; after a pump uses it,
  /// not readable until the next poll reports; empty when no poll has
  /// reported.
  std::optional<bool> readiness_;
  /// @brief The lifecycle state, stored after every lifecycle event.
  std::atomic<ClientState> published_state_{ClientState::idle};
  /// @brief Whether keys are installed, as the last owner call left it.
  std::atomic<bool> published_crypto_installed_{};
  /// @brief due_us() as the last owner call left it; no_deadline for none.
  std::atomic<std::uint64_t> published_due_us_{no_deadline};
  /// @brief Guards published_snapshot_; held only to copy it.
  mutable platform::Mutex snapshot_mutex_;
  /// @brief The snapshot the last owner call left.
  Snapshot published_snapshot_;
};

}  // namespace libchiaking::net::protocol::takion
