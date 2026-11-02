/// @file
/// @brief Declares BandwidthTest, the rules of Senkusha's downstream
/// bandwidth measurement, kept apart from the session that sends its
/// commands.
///
/// The test is a pure state machine: it is told when packets arrive, when
/// the far end reports how many it sent, and what time it is, and answers
/// with the next step (send a round, wait, or finished). Client turns
/// a Send step into a SenkushaBandwidthCommand on the Takion control channel.
#pragma once

#include "src/core/net/protocol/senkusha/types.hpp"

#include <cstdint>
#include <optional>
#include <variant>

namespace libchiaking::net::protocol::senkusha {

/// @brief One request for the far end to stream video packets at a rate.
///
/// It becomes a SenkushaBandwidthCommand with `id`, `time` = `duration_ms`,
/// `bandwidth` = `kbps`, `fps`, and `mtu` = the measured inbound MTU.
struct BandwidthRound {
  /// @brief Round number, starting at 1 and increasing by one per round;
  /// the far end's report echoes it.
  std::uint32_t id{};
  /// @brief Rate asked for, in kilobits per second.
  std::uint32_t kbps{};
  /// @brief How long to stream, in milliseconds.
  std::uint32_t duration_ms{};
  /// @brief Frame rate to pace packets at.
  std::uint32_t fps{};
  /// @brief Compares every field.
  ///
  /// @return Whether both rounds are the same.
  bool operator==(const BandwidthRound&) const = default;
};

/// @brief The steps a BandwidthTest asks its driver to take.
namespace bandwidth_step {
/// @brief Send this round's command now.
struct Send {
  /// @brief The round to request.
  BandwidthRound round;
};
/// @brief Do nothing until a time, then call BandwidthTest::tick().
struct Wait {
  /// @brief When the pause ends, in monotonic microseconds.
  std::uint64_t until_us{};
};
/// @brief The measurement is over.
struct Finished {
  /// @brief What it found; empty when the far end never streamed or nothing
  /// was received.
  std::optional<BandwidthMeasurement> measurement;
};
}  // namespace bandwidth_step

/// @brief Any step a BandwidthTest asks for.
using BandwidthStep = std::variant<bandwidth_step::Send, bandwidth_step::Wait,
                                   bandwidth_step::Finished>;

/// @brief Senkusha's downstream bandwidth measurement.
///
/// It runs a preliminary round at the preliminary rate; if any video arrives
/// it runs a main round at the target rate. When the far end reports how many
/// packets it sent, the loss is `1 - received / sent`. Loss within the
/// policy's maximum ends the test with the requested rate. Otherwise, after
/// `retry_delay`, a new round runs at the lower of the rate actually received
/// and the requested rate less `minimum_step_kbps`; the test ends with the
/// received rate when that would fall below the preliminary rate or the
/// round budget is spent. A round that runs past its duration plus
/// `reply_grace` without a report ends the test on what was received.
///
/// Valid from create(); a plain value with no shared state, used from one
/// thread. Every time is in monotonic microseconds.
class BandwidthTest {
 public:
  /// @brief Where the measurement is.
  enum class Phase : std::uint8_t {
    /// @brief Created, not started.
    idle,
    /// @brief The preliminary round is streaming.
    preliminary,
    /// @brief A main round is streaming.
    main,
    /// @brief Pausing between a lossy round and the next.
    resting,
    /// @brief Over; every further call is ignored or answers Finished.
    finished,
  };

  /// @brief Makes a test after checking the policy.
  ///
  /// @param policy The rates, durations, loss threshold and round budget.
  /// @return A test in Phase::idle.
  /// @retval Error::invalid_configuration A rate, duration, frame
  ///     rate or step is zero, the target is below the preliminary rate, the
  ///     retry delay is negative, the loss threshold is not within 0 to 1,
  ///     or fewer than 2 rounds are allowed.
  [[nodiscard]] static auto create(const BandwidthPolicy& policy)
      -> Expected<BandwidthTest, Error>;

  /// @brief Begins the preliminary round.
  ///
  /// @param now_us The current time.
  /// @return A Send for round 1; Finished with nothing when already
  ///     started.
  [[nodiscard]] auto start(std::uint64_t now_us) -> BandwidthStep;
  /// @brief Counts one received video unit towards the current round.
  ///
  /// Ignored outside the preliminary and main phases.
  ///
  /// @param now_us When it arrived.
  /// @param bytes Its payload size, in bytes.
  void packet(std::uint64_t now_us, std::uint64_t bytes) noexcept;
  /// @brief Takes the far end's report on a round.
  ///
  /// A report on the preliminary round starts the main round, or finishes
  /// with nothing when no video arrived. A report on a main round with a
  /// zero count finishes on the received rate.
  ///
  /// @param round_id The round the report names.
  /// @param total_sent Packets the far end says it sent.
  /// @param now_us When the report arrived.
  /// @return The next step; nothing when the report is for another round
  ///     or no round is streaming.
  [[nodiscard]] auto reply(std::uint32_t round_id, std::uint32_t total_sent,
                           std::uint64_t now_us)
      -> std::optional<BandwidthStep>;
  /// @brief Acts on the passage of time: ends a pause, or ends a round
  /// whose report is overdue.
  ///
  /// @param now_us The current time.
  /// @return A Send when a pause is over; the next round or Finished when a
  ///     round is overdue (Finished with nothing if no video arrived);
  ///     nothing when no deadline has passed.
  [[nodiscard]] auto tick(std::uint64_t now_us) -> std::optional<BandwidthStep>;

  /// @brief Reads where the measurement is.
  ///
  /// @return The phase.
  Phase phase() const noexcept { return phase_; }
  /// @brief Reads when tick() next has work to do.
  ///
  /// @return The round's report deadline while streaming, the end of the
  ///     pause while resting, nothing when idle or finished.
  std::optional<std::uint64_t> deadline_us() const noexcept;
  /// @brief Reads the round streaming now.
  ///
  /// @return The round; nothing while idle, resting or finished.
  std::optional<BandwidthRound> current_round() const noexcept {
    return round_;
  }

 private:
  /// @brief Stores the already validated policy.
  ///
  /// @param policy The policy create() checked.
  explicit BandwidthTest(const BandwidthPolicy& policy) noexcept;
  /// @brief Starts a new round, resetting the received counters and arming
  /// its deadline at `now_us + (duration_ms + reply_grace)`.
  ///
  /// @param phase preliminary or main.
  /// @param kbps The rate to ask for.
  /// @param duration_ms How long to stream.
  /// @param now_us The current time.
  /// @return A Send for the new round; Finished with nothing when the round
  ///     budget is spent.
  auto begin(Phase phase, std::uint32_t kbps, std::uint32_t duration_ms,
             std::uint64_t now_us) -> BandwidthStep;
  /// @brief Computes the current round's received rate and loss.
  ///
  /// The rate is received bits over the span from the first to the last
  /// packet (the round's duration when fewer than two distinct arrival
  /// times), capped at the requested rate.
  ///
  /// @param total_sent Packets the far end reported; 0 means unknown and
  ///     gives zero loss.
  /// @return The measurement.
  auto measure(std::uint32_t total_sent) const noexcept -> BandwidthMeasurement;
  /// @brief Ends the test.
  ///
  /// @param measurement The answer to report, or nothing.
  /// @return Finished with `measurement`.
  auto conclude(std::optional<BandwidthMeasurement> measurement)
      -> BandwidthStep;
  /// @brief Treats a zero received rate as no answer.
  ///
  /// @param measurement A computed measurement.
  /// @return The measurement, or nothing when its rate is zero.
  static auto delivered(const BandwidthMeasurement& measurement) noexcept
      -> std::optional<BandwidthMeasurement>;
  /// @brief Decides, from a main round's measurement, whether to accept,
  /// step down, or give up.
  ///
  /// @param measurement The round's measurement with a known sent count.
  /// @param now_us The current time.
  /// @return Finished with the requested rate when loss is acceptable; a
  ///     Wait until the next round otherwise; Finished with the received
  ///     rate when no lower round is allowed.
  auto settle(const BandwidthMeasurement& measurement, std::uint64_t now_us)
      -> BandwidthStep;

  /// @brief The validated policy.
  BandwidthPolicy policy_;
  /// @brief Where the measurement is.
  Phase phase_{Phase::idle};
  /// @brief The round streaming now; empty while idle, resting or finished.
  std::optional<BandwidthRound> round_{};
  /// @brief Id the next round will get.
  std::uint32_t next_id_{1};
  /// @brief Rounds begun so far, the preliminary one included.
  std::uint32_t rounds_{};
  /// @brief Rate of the round that follows the current pause, in kilobits
  /// per second.
  std::uint32_t next_kbps_{};
  /// @brief When the current round's report is overdue, in monotonic
  /// microseconds.
  std::uint64_t round_deadline_us_{};
  /// @brief When the current pause ends, in monotonic microseconds.
  std::uint64_t rest_until_us_{};
  /// @brief Video units received in the current round.
  std::uint64_t received_packets_{};
  /// @brief Payload bytes received in the current round.
  std::uint64_t received_bytes_{};
  /// @brief When the current round's first unit arrived.
  std::optional<std::uint64_t> first_packet_us_{};
  /// @brief When the current round's latest unit arrived.
  std::uint64_t last_packet_us_{};
};

}  // namespace libchiaking::net::protocol::senkusha
