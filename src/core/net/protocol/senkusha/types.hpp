/// @file
/// @brief Declares the vocabulary of Senkusha, the connection test PS runs
/// over a short-lived Takion session before a stream: its tuning constants,
/// policies, errors, states, results and events.
///
/// Senkusha ("connection test") opens a plaintext Takion session to the
/// console's or datacenter's test port, exchanges BIG and BANG (or, for a PS
/// Cloud datacenter, has the BIG echoed back), and then measures the path the
/// stream will use: round-trip time with echoed audio probes, the inbound MTU
/// by asking the far end for video packets of a requested size, the outbound
/// MTU by sending audio probes of a candidate size for it to echo, and
/// optionally the downstream bandwidth. Client runs the test; this
/// header holds everything it is configured with and everything it reports.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "src/core/net/protocol/takion/client_types.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/net/protocol/takion/versions.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace libchiaking::net::protocol::senkusha {

/// @brief Round-trip probes sent per test unless the policy says otherwise.
inline constexpr std::uint32_t default_probe_count = 10;
/// @brief How long one round-trip probe waits for its echo after it left the
/// socket before it counts as lost.
inline constexpr std::chrono::milliseconds default_probe_timeout{1000};
/// @brief Smallest payload, in bytes after the AV header, an echoed probe may
/// carry and still be read; it covers the 4-byte tag at byte 4.
inline constexpr std::uint32_t probe_minimum_payload_size = 8;
/// @brief Byte offset of the 4-byte big-endian probe tag within a probe's
/// payload, counted from the end of the AV header.
inline constexpr std::uint32_t default_probe_tag_offset = 4;
/// @brief Smallest path MTU the search considers, in bytes of IP packet; the
/// IPv4 minimum reassembly size, assumed always to work.
inline constexpr std::uint32_t mtu_lower_bound = 576;
/// @brief Largest path MTU the search considers, in bytes of IP packet; the
/// search starts here for the inbound direction.
inline constexpr std::uint32_t mtu_upper_bound = 1454;
/// @brief Consecutive lost attempts that condemn one MTU candidate.
inline constexpr std::uint32_t mtu_attempts_per_candidate = 3;
/// @brief Bytes of IPv4 (20) and UDP (8) header between an MTU and the UDP
/// payload the Takion datagram fills.
inline constexpr std::uint32_t mtu_network_overhead = 28;
/// @brief Bytes in front of an outbound MTU probe's payload: the 1-byte packet
/// type plus the 17-byte (0x11) Takion version 7 audio AV header, 18 in all.
inline constexpr auto mtu_av_header_size = static_cast<std::uint32_t>(
    takion::packet_type_size + takion::audio_av_header_size_v7);
/// @brief An MTU attempt waits this many adjusted round-trip times for its
/// answer, before the floor and ceiling are applied.
inline constexpr std::uint32_t mtu_timeout_multiplier = 5;
/// @brief Shortest wait for one MTU attempt.
inline constexpr std::chrono::milliseconds mtu_timeout_floor{5};
/// @brief Longest wait for one MTU attempt.
inline constexpr std::chrono::milliseconds mtu_timeout_ceiling{500};
/// @brief Size, in bytes, of the UDP payload of a round-trip probe and the
/// cap on every control datagram of a Senkusha session: 576 - 28 = 548, so it
/// fits any path that honours the IPv4 minimum.
inline constexpr std::uint32_t minimum_datagram_size =
    mtu_lower_bound - mtu_network_overhead;
/// @brief How long the Takion handshake may take before the test fails.
inline constexpr std::chrono::milliseconds default_connect_timeout{30'000};
/// @brief How long each request of the test (BIG, echo switch, MTU switch)
/// waits for its answer or acknowledgement.
inline constexpr std::chrono::milliseconds default_response_timeout{5'000};
/// @brief How long, in microseconds, a CloudStream datacenter is given to echo
/// the BIG back (2 s); when it does not, the test carries on regardless.
inline constexpr std::uint64_t fragmented_big_response_timeout_us = 2'000'000;
/// @brief Id of the SenkushaClientMtuCommand that switches the far end's
/// outbound MTU echo on; the far end confirms with a command of the same id.
inline constexpr std::uint32_t outbound_mtu_start_command_id = 1;
/// @brief Id of the SenkushaClientMtuCommand that switches the outbound MTU
/// echo off again.
inline constexpr std::uint32_t outbound_mtu_stop_command_id = 2;
/// @brief Until the transport confirms an inbound MTU request left the
/// socket, the attempt waits this many MTU timeouts; once it has, one.
inline constexpr std::uint64_t inbound_mtu_deadline_factor = 2;

/// @brief Why a connection test could not be configured, run or finished.
enum class Error : std::uint8_t {
  /// @brief The profile or policy breaks a rule: a zero or negative timeout,
  /// inverted MTU bounds, a probe count of zero or above 2048, a tag that
  /// does not fit the minimum payload, or a CloudStream echo profile not pinned
  /// to version 9 with only a session key.
  invalid_configuration,
  /// @brief A bootstrap, fixed or offered Takion version is not implemented
  /// or not available for console sessions.
  unsupported_version,
  /// @brief The negotiated version list names the same version twice.
  duplicate_version,
  /// @brief The BigResponsePolicy is not one the client knows.
  unsupported_big_response,
  /// @brief A deadline, counter, size or timing sum would overflow its type.
  arithmetic_overflow,
  /// @brief An operation was asked for in the wrong state, such as starting
  /// twice, pumping before start, or completing without all measurements.
  invalid_state,
  /// @brief cancel() stopped the test.
  cancelled,
  /// @brief The Takion session failed, closed, or refused or lost a message
  /// the test was waiting on.
  takion_failure,
  /// @brief A connect or response deadline passed with no answer.
  timeout,
  /// @brief The BANG refused the version or the encrypted key when the
  /// profile requires them to be accepted.
  negotiation_failed,
  /// @brief An answer could not be read: an unencodable BIG, a probe that
  /// cannot be built, or a BANG without the protocol acknowledgement the
  /// profile requires.
  malformed_response,
  /// @brief Every round-trip probe was lost.
  no_rtt_samples,
  /// @brief The MTU search could not reach a result. Reserved: nothing in
  /// the client reports it today, since the search always settles on its
  /// lower bound at worst.
  mtu_failed,
};

/// @brief The step a connection test is at, in the order the steps run.
enum class State : std::uint8_t {
  /// @brief Constructed, start() not yet called.
  idle,
  /// @brief The Takion INIT and COOKIE handshake is running.
  connecting_takion,
  /// @brief Connected; waiting for the Takion version negotiation to commit
  /// a version.
  negotiating_version,
  /// @brief The BIG is being sent; passed through at once.
  sending_big,
  /// @brief The BIG is sent; waiting for the BANG.
  awaiting_bang,
  /// @brief The BIG is sent to a CloudStream datacenter; waiting for its bytes
  /// to come back.
  awaiting_big_echo,
  /// @brief The echo switch-on command is sent; waiting for its reliable
  /// acknowledgement.
  enabling_echo,
  /// @brief Round-trip probes are being sent one at a time.
  measuring_rtt,
  /// @brief The echo switch-off command is sent; waiting for its
  /// acknowledgement.
  disabling_echo,
  /// @brief Asking the far end for video packets of candidate sizes.
  measuring_inbound_mtu,
  /// @brief The outbound MTU echo switch-on command is sent; waiting for
  /// the far end's confirmation.
  starting_outbound_mtu,
  /// @brief Sending audio probes of candidate sizes for the far end to echo.
  measuring_outbound_mtu,
  /// @brief The outbound MTU echo switch-off command is sent; waiting for
  /// its acknowledgement.
  finishing_outbound_mtu,
  /// @brief Bandwidth rounds are running.
  measuring_bandwidth,
  /// @brief Measurements are done; the session is being closed or its route
  /// kept.
  disconnecting,
  /// @brief The test succeeded; the result is in the snapshot.
  completed,
  /// @brief The test failed; the error is in the snapshot.
  failed,
};

/// @brief How the far end answers the BIG that opens a test.
enum class BigResponsePolicy : std::uint8_t {
  /// @brief A console answers with a BANG control message, which is then
  /// validated against BangValidationPolicy.
  typed_bang,
  /// @brief A CloudStream datacenter sends the encoded BIG's bytes back as
  /// control payload, possibly fragmented; the test goes on once that many
  /// bytes have arrived or after fragmented_big_response_timeout_us.
  fragmented_big_echo,
};

/// @brief What happens to the UDP socket when a test ends.
enum class TransportHandoffPolicy : std::uint8_t {
  /// @brief The socket is closed with the session.
  close_after_use,
  /// @brief The socket is kept open and handed back, so the stream can use
  /// the same local port and NAT binding the test measured.
  return_route,
};

/// @brief A correction applied to the mean round-trip time before it is
/// reported and used for MTU timeouts.
///
/// The adjusted mean is `raw_mean - subtract`, never less than `floor`. Both
/// must be zero or positive.
struct RttAdjustmentPolicy {
  /// @brief Subtracted from the raw mean, for example a known processing
  /// delay at the far end.
  std::chrono::microseconds subtract{0};
  /// @brief The least the adjusted mean may be.
  std::chrono::microseconds floor{0};
  /// @brief Compares every field.
  ///
  /// @return Whether both policies are the same.
  constexpr bool operator==(const RttAdjustmentPolicy&) const noexcept =
      default;
};

/// @brief How the round-trip measurement is run.
///
/// Probes go out one at a time; each carries a fresh random tag and waits
/// `timeout` for an echoed audio unit carrying the same tag.
struct ProbePolicy {
  /// @brief Probes sent; 1 to 2048, since each probe's unit index must fit
  /// the 11-bit field of the AV header.
  std::uint32_t count{default_probe_count};
  /// @brief How long each probe waits for its echo, counted from when the
  /// transport reports it sent.
  std::chrono::milliseconds timeout{default_probe_timeout};
  /// @brief Smallest echoed payload, in bytes after the AV header, accepted;
  /// shorter units are ignored.
  std::uint32_t minimum_payload_size{probe_minimum_payload_size};
  /// @brief Offset of the tag within an echoed payload; the 4 tag bytes must
  /// end within `minimum_payload_size`.
  std::uint32_t tag_offset{default_probe_tag_offset};
  /// @brief Whether an echo must also carry the probe's unit index, not
  /// just its tag.
  bool require_unit_index{true};
  /// @brief Compares every field.
  ///
  /// @return Whether both policies are the same.
  constexpr bool operator==(const ProbePolicy&) const noexcept = default;
};

/// @brief How the inbound and outbound MTU searches are run.
///
/// Both searches are a binary search between the bounds that assumes the
/// lower bound always works (ConservativeMtuSearch). Sizes are IP packet
/// sizes in bytes; the UDP payload is `network_overhead` smaller.
struct MtuPolicy {
  /// @brief Size assumed to work; must be above zero and at least
  /// `network_overhead` + 18 + 8.
  std::uint32_t lower_bound{mtu_lower_bound};
  /// @brief Largest size tried; must exceed `lower_bound`.
  std::uint32_t upper_bound{mtu_upper_bound};
  /// @brief Consecutive losses after which a candidate is judged too big;
  /// at least 1.
  std::uint32_t attempts_per_candidate{mtu_attempts_per_candidate};
  /// @brief Bytes of IP and UDP header subtracted from a candidate to size
  /// the datagram.
  std::uint32_t network_overhead{mtu_network_overhead};
  /// @brief Adjusted round-trip times an attempt waits; at least 1.
  std::uint32_t timeout_multiplier{mtu_timeout_multiplier};
  /// @brief Shortest wait per attempt; positive, not above the ceiling.
  std::chrono::milliseconds timeout_floor{mtu_timeout_floor};
  /// @brief Longest wait per attempt; positive.
  std::chrono::milliseconds timeout_ceiling{mtu_timeout_ceiling};
  /// @brief Compares every field.
  ///
  /// @return Whether both policies are the same.
  constexpr bool operator==(const MtuPolicy&) const noexcept = default;
};

/// @brief How the optional downstream bandwidth measurement is run.
///
/// The client asks the far end for a stream of video packets at a rate for a
/// time, counts what arrives, and compares it with the count the far end
/// reports it sent. A short preliminary round checks the far end answers at
/// all; then rounds at the target rate step down until the loss is
/// acceptable. See BandwidthTest.
struct BandwidthPolicy {
  /// @brief Rate of the preliminary round, in kilobits per second; also the
  /// lowest rate a later round may step down to. Above zero.
  std::uint32_t preliminary_kbps{1000};
  /// @brief Length of the preliminary round; positive.
  std::chrono::milliseconds preliminary_duration{1000};
  /// @brief Rate of the first main round, in kilobits per second; not below
  /// `preliminary_kbps`.
  std::uint32_t target_kbps{15000};
  /// @brief Length of each main round; positive.
  std::chrono::milliseconds duration{3000};
  /// @brief Frame rate the far end is asked to pace packets at; above zero.
  std::uint32_t fps{30};
  /// @brief Highest loss fraction, 0 to 1, at which a round's requested
  /// rate is accepted as the answer.
  double maximum_loss{0.05};
  /// @brief Least a failed round's rate is lowered by for the next round, in
  /// kilobits per second; above zero.
  std::uint32_t minimum_step_kbps{1000};
  /// @brief Pause between a failed round and the next; zero or positive.
  std::chrono::milliseconds retry_delay{500};
  /// @brief Extra time after a round's duration to wait for the far end's
  /// report; positive.
  std::chrono::milliseconds reply_grace{1000};
  /// @brief Rounds allowed in all, the preliminary one included; at least 2.
  std::uint32_t maximum_rounds{5};
  /// @brief Compares every field.
  ///
  /// @return Whether both policies are the same.
  constexpr bool operator==(const BandwidthPolicy&) const noexcept = default;
};

/// @brief The timing and measurement settings of one connection test.
struct Policy {
  /// @brief How long the Takion handshake may take; positive.
  std::chrono::milliseconds connect_timeout{default_connect_timeout};
  /// @brief How long each request waits for its answer; positive.
  std::chrono::milliseconds response_timeout{default_response_timeout};
  /// @brief Round-trip measurement settings.
  ProbePolicy probe{};
  /// @brief MTU search settings.
  MtuPolicy mtu{};
  /// @brief Bandwidth measurement settings; empty skips the measurement.
  std::optional<BandwidthPolicy> bandwidth{};
  /// @brief Compares every field.
  ///
  /// @return Whether both policies are the same.
  constexpr bool operator==(const Policy&) const noexcept = default;
};

/// @brief Which BANG flags a console must set for the test to go on.
///
/// Every flag false accepts any BANG, which is what local Remote Play uses.
struct BangValidationPolicy {
  /// @brief Fail with negotiation_failed unless `version_accepted` is set.
  bool require_version_accepted{};
  /// @brief Fail with negotiation_failed unless `encrypted_key_accepted` is
  /// set.
  bool require_encrypted_key_accepted{};
  /// @brief Fail with malformed_response unless the BANG carries a Takion
  /// protocol request acknowledgement.
  bool require_protocol_ack{};
  /// @brief Compares every field.
  ///
  /// @return Whether both policies are the same.
  constexpr bool operator==(const BangValidationPolicy&) const noexcept =
      default;
};

/// @brief What the far end expects of the test session: Takion versions,
/// envelope, the BIG and how it is answered.
///
/// The defaults suit a console: bootstrap version 7, negotiation over
/// versions 12, 15, 18 and 20, a typed BANG, and the route handed back. A PS
/// Cloud datacenter survey pins version 9 instead and expects the BIG echoed.
/// Probes are always encoded with the version 7 AV layout, whatever version
/// is committed.
struct Profile {
  /// @brief Version the handshake and probes are framed with before any
  /// negotiation; must be available for console sessions.
  std::uint32_t bootstrap_version{takion::takion_bootstrap_version};
  /// @brief A fixed version, or the versions offered for negotiation.
  takion::ProfileSelection profile_selection{takion::NegotiatedTakionProfiles{
      {takion::default_negotiated_takion_versions.begin(),
       takion::default_negotiated_takion_versions.end()}}};
  /// @brief Framing added around each outgoing datagram, such as the 4-byte
  /// PSN prefix.
  takion::PacketEnvelope envelope{takion::PlainEnvelope{}};
  /// @brief Framing expected on incoming datagrams; empty means the same as
  /// `envelope`.
  std::optional<takion::PacketEnvelope> receive_envelope;
  /// @brief How the BIG is answered.
  BigResponsePolicy big_response{BigResponsePolicy::typed_bang};
  /// @brief The BIG sent once connected; its client version defaults to 9.
  takion::Big big{.client_version = takion::senkusha_client_takion_version};
  /// @brief Which BANG flags must be set.
  BangValidationPolicy bang_validation{};
  /// @brief Correction applied to the measured round-trip time.
  RttAdjustmentPolicy rtt_adjustment{};
  /// @brief What happens to the socket after a successful test.
  TransportHandoffPolicy handoff{TransportHandoffPolicy::return_route};
};

/// @brief What the round-trip measurement found.
///
/// All times are durations in microseconds measured on the monotonic clock,
/// from the moment the transport reported the probe sent to the moment its
/// echo was readable.
struct RttStatistics {
  /// @brief Probes sent.
  std::uint32_t attempted{};
  /// @brief Probes whose echo came back in time.
  std::uint32_t successful{};
  /// @brief Probes lost: `attempted - successful`.
  std::uint32_t lost{};
  /// @brief Mean of the successful samples, rounded down.
  std::chrono::microseconds raw_mean{0};
  /// @brief Fastest successful sample.
  std::chrono::microseconds minimum{0};
  /// @brief Slowest successful sample.
  std::chrono::microseconds maximum{0};
  /// @brief Population variance of the successful samples, in square
  /// microseconds.
  long double variance_us_squared{};
  /// @brief The raw mean after RttAdjustmentPolicy; this is the figure
  /// reported and used to time MTU attempts.
  std::chrono::microseconds adjusted_mean{0};
};

/// @brief What a bandwidth measurement found.
struct BandwidthMeasurement {
  /// @brief Usable downstream rate, in kilobits per second: the requested
  /// rate when loss was acceptable, otherwise the rate actually received,
  /// capped at the requested one.
  std::uint32_t kbps{};
  /// @brief Fraction of the far end's packets that did not arrive, 0 to 1;
  /// zero when the far end did not report a count.
  double loss{};
  /// @brief Rate the final round asked for, in kilobits per second.
  std::uint32_t requested_kbps{};
  /// @brief Compares every field.
  ///
  /// @return Whether both measurements are the same.
  bool operator==(const BandwidthMeasurement&) const = default;
};

/// @brief Everything a successful connection test found.
struct Result {
  /// @brief The Takion version committed for the test session.
  std::uint32_t takion_version;
  /// @brief Round-trip measurement.
  RttStatistics round_trip_time;
  /// @brief Largest IP packet, in bytes, the far end could send to us.
  std::uint32_t inbound_mtu;
  /// @brief Largest IP packet, in bytes, we could send to the far end.
  std::uint32_t outbound_mtu;
  /// @brief The wire profile of `takion_version`.
  takion::WireProfile wire_profile;
  /// @brief Downstream bandwidth; empty when not asked for or when it could
  /// not be measured.
  std::optional<BandwidthMeasurement> bandwidth{};
};

/// @brief A point-in-time view of a connection test.
struct Snapshot {
  /// @brief The current step.
  State state{State::idle};
  /// @brief Id of the running attempt; empty before start and once it ends.
  std::optional<std::uint64_t> attempt_id{};
  /// @brief When the current wait expires, in monotonic microseconds; empty
  /// when nothing is being waited for.
  std::optional<std::uint64_t> deadline_us{};
  /// @brief Round-trip result, once measured.
  std::optional<RttStatistics> rtt_statistics{};
  /// @brief Inbound MTU in bytes, once measured.
  std::optional<std::uint32_t> inbound_mtu{};
  /// @brief Outbound MTU in bytes, once measured.
  std::optional<std::uint32_t> outbound_mtu{};
  /// @brief The full result, once the test has completed.
  std::optional<Result> result{};
  /// @brief Why the test failed, once it has.
  std::optional<Error> error{};
};

/// @brief The notifications a connection test sends its EventSink.
namespace event {
/// @brief The test moved to a new step.
struct StateChanged {
  /// @brief The step it is now at.
  State state{State::idle};
};
/// @brief The test succeeded.
struct Completed {
  /// @brief What it found.
  Result result;
};
/// @brief The test failed; no further events follow.
struct Failed {
  /// @brief Why.
  Error error{Error::invalid_state};
};
}  // namespace event

/// @brief Any notification of a connection test.
using Event =
    std::variant<event::StateChanged, event::Completed, event::Failed>;

/// @brief Receives a connection test's notifications.
///
/// Client calls it synchronously from inside start(), pump_once()
/// and cancel(), on the thread making that call. It must outlive the client
/// and must not call back into the client that is notifying it.
class EventSink {
 public:
  /// @brief Releases the sink.
  virtual ~EventSink() = default;
  /// @brief Takes one notification.
  ///
  /// @param event What happened; a StateChanged for every step, then one
  ///     Completed or one Failed.
  virtual void on_event(Event event) = 0;
};

}  // namespace libchiaking::net::protocol::senkusha
