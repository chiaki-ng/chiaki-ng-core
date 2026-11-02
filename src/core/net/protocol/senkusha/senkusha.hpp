/// @file
/// @brief Declares Client, which runs one Senkusha connection test
/// against a console or a CloudStream datacenter and hands back what it found.
///
/// Local Remote Play runs the test on the route it is about to stream over
/// and keeps the socket for the stream (remote_play::LocalStart); the cloud
/// datacenter survey runs one per datacenter to rank them by round-trip time
/// and closes each socket afterwards.
#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/protocol/remote_play/stream/route.hpp"
#include "src/core/net/protocol/senkusha/types.hpp"
#include "src/core/net/protocol/senkusha/validator.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace libchiaking::net::protocol::senkusha {

/// @brief Everything a connection test is configured with.
struct ClientConfig {
  /// @brief What the far end expects: versions, envelope, the BIG and how
  /// it is answered.
  Profile profile{};
  /// @brief Timeouts and measurement settings.
  Policy policy{};
  /// @brief Limits handed to the Takion session. The fragment size is
  /// raised to at least the MTU upper bound and the control datagram size
  /// is pinned to minimum_datagram_size (548 bytes) whatever is given here.
  takion::TakionClientLimits takion_limits{};
  /// @brief How often and how patiently the Takion INIT and COOKIE are
  /// resent.
  takion::HandshakeRetryPolicy handshake_retry{};
  /// @brief What happens to the socket when the test fails: return_route
  /// keeps it for take_failed_route(), close_after_use closes it.
  TransportHandoffPolicy failure_handoff{
      TransportHandoffPolicy::close_after_use};
};

/// @brief The outcome of a Client operation: nothing, or why it failed.
using OperationResult = Expected<void, Error>;

/// @brief One Senkusha connection test over one UDP route.
///
/// The client owns a plaintext Takion session (crypto disabled, control on
/// channel 1, Senkusha commands on channel 8) over the transport it is given.
/// start() begins the handshake; the caller then calls pump_once() whenever
/// the socket is readable or a deadline from snapshot() passes, until the
/// state is completed or failed. The steps, in order: Takion handshake;
/// version negotiation unless the profile fixes one; BIG, answered by a BANG
/// or by its own echo; echo switched on; round-trip probes; echo switched
/// off; inbound MTU search; outbound MTU search; optional bandwidth rounds;
/// a best-effort Takion DISCONNECT ("Client Disconnecting"). Every step
/// change and the final outcome are reported to the EventSink.
///
/// Each start() is one attempt with its own id; a client runs one attempt
/// only. Not copyable or movable. Calls must come from one thread at a time,
/// and the sink is called on that thread. The socket is read without
/// waiting; a send the socket refuses with would-block waits for it to become
/// writable, up to the deadline the test attached to that send (the probe,
/// MTU or response timeout). The clock, random source and sink must outlive
/// the client.
class Client {
 public:
  /// @brief Prepares a test over a bare transport.
  ///
  /// After success the socket is available from take_route() when the
  /// profile's handoff is return_route.
  ///
  /// @param transport The UDP socket, already connected to the test port.
  /// @param clock The monotonic clock deadlines and samples are taken from.
  /// @param threading Provider of the Takion session's mutex.
  /// @param random The source of probe tags and Takion verification tags.
  /// @param events Receives the test's notifications.
  /// @param config How the test is run; validated by start().
  Client(std::unique_ptr<net::transport::datagram::DatagramTransport> transport,
         platform::ClockProvider& clock, platform::ThreadingProvider& threading,
         platform::SecureRandomProvider& random, EventSink& events,
         ClientConfig config = {});
  /// @brief Prepares a test over a route candidate, remembering its
  /// endpoints so the route can be handed back whole.
  ///
  /// After success the route is available from take_established_route();
  /// after failure, from take_failed_route() when `config.failure_handoff`
  /// is return_route.
  ///
  /// @param route The candidate route; its transport is taken over.
  /// @param clock The monotonic clock deadlines and samples are taken from.
  /// @param threading Provider of the Takion session's mutex.
  /// @param random The source of probe tags and Takion verification tags.
  /// @param events Receives the test's notifications.
  /// @param config How the test is run; validated by start().
  Client(net::protocol::remote_play::RouteCandidate route,
         platform::ClockProvider& clock, platform::ThreadingProvider& threading,
         platform::SecureRandomProvider& random, EventSink& events,
         ClientConfig config = {});
  /// @brief Stops the Takion session and closes any socket not handed out.
  ~Client();

  /// @brief Not copyable; the client owns a live session.
  Client(const Client&) = delete;
  /// @brief Not copy-assignable.
  Client& operator=(const Client&) = delete;
  /// @brief Not movable; the Takion session refers back into the client.
  Client(Client&&) = delete;
  /// @brief Not move-assignable.
  Client& operator=(Client&&) = delete;

  /// @brief Validates the configuration and starts the Takion handshake,
  /// arming the connect deadline.
  ///
  /// @return Nothing once the handshake's INIT is on its way.
  /// @retval Error::invalid_state The test was already started.
  /// @retval Error::invalid_configuration The profile or policy
  ///     breaks a rule; see Validator::validate_configuration.
  /// @retval Error::unsupported_version A configured version is not
  ///     available.
  /// @retval Error::arithmetic_overflow A timeout or deadline does
  ///     not fit in microseconds.
  /// @retval Error::takion_failure The Takion session would not
  ///     start; the test has failed.
  [[nodiscard]] OperationResult start();
  /// @brief Moves the test along: pumps the Takion session, handles what it
  /// received, runs any bandwidth timer, and acts on an expired deadline.
  ///
  /// An expired deadline while a probe or MTU attempt is outstanding counts
  /// that attempt as lost and sends the next one; any other expired
  /// deadline fails the test with timeout. Blocks only while a refused send
  /// waits for the socket, up to that send's deadline.
  ///
  /// @return Nothing while the test runs or once it has completed.
  /// @retval Error::invalid_state start() was not called.
  /// @retval Error The error the test has failed with, now or
  ///     earlier.
  [[nodiscard]] OperationResult pump_once();
  /// @brief Tells the client when the caller first saw the socket readable,
  /// so an echo's arrival time is not inflated by the delay before the next
  /// pump.
  ///
  /// The earliest mark since the last pump is kept; the next pump_once()
  /// uses it as the receive time of a probe echo when it lies after the
  /// probe was sent and before the echo was read, and clears it.
  ///
  /// @param ready_at_us When the socket became readable, in monotonic
  ///     microseconds on the client's clock.
  void mark_readable(std::uint64_t ready_at_us) noexcept;
  /// @brief Stops a running test, sending a best-effort DISCONNECT, and
  /// closes the kept socket, if any.
  ///
  /// The sink is told the test failed with cancelled. A test not running is
  /// left as it is, except that a kept socket is still closed.
  ///
  /// @return Always nothing.
  [[nodiscard]] OperationResult cancel();
  /// @brief Reads the test's progress and outcome.
  ///
  /// @return The current step, deadline, measurements so far, and the
  ///     result or error once there is one.
  Snapshot snapshot() const;
  /// @brief Hands back the socket of a successful test made from a bare
  /// transport.
  ///
  /// @return The socket, once; nothing when the test has not completed,
  ///     the client was made from a route candidate, the profile closed the
  ///     socket, or it was already taken.
  [[nodiscard]] std::unique_ptr<net::transport::datagram::DatagramTransport>
  take_route();
  /// @brief Hands back the measured route of a successful test made from a
  /// route candidate.
  ///
  /// @return The route with its socket, endpoints and the test's result,
  ///     once; nothing when the test has not completed, the client was made
  ///     from a bare transport, the profile closed the socket, or it was
  ///     already taken.
  [[nodiscard]] std::optional<net::protocol::remote_play::EstablishedRoute>
  take_established_route();
  /// @brief Hands back the route of a failed test so another stage can try
  /// it.
  ///
  /// @return The route with its still-open socket, once; nothing when the
  ///     test has not failed, was cancelled, the client was made from a
  ///     bare transport, `failure_handoff` was close_after_use, or the
  ///     socket was already closed or taken.
  [[nodiscard]] std::optional<net::protocol::remote_play::RouteCandidate>
  take_failed_route();

 private:
  /// @brief The test's state and logic, kept out of the header.
  class Impl;
  /// @brief The test; never null.
  std::unique_ptr<Impl> impl_;
};

}  // namespace libchiaking::net::protocol::senkusha
