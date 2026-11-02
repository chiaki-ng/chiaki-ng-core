/// @file
/// @brief Declares LaunchObserver, the port through which the server
/// reservation reports progress and asks for the datacenter pick, and
/// LaunchReport, the helper that phrases its progress.
#pragma once

#include "libchiaking/detail/play/cloud_launch.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/play/adapters/datacenters/datacenters.hpp"
#include "src/core/psn/gk/measurement.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::play {

/// @brief The player's answer to a datacenter offer at launch. Plain data.
struct LaunchChoice {
  /// @enum Kind
  /// @brief How the wait for the pick ended.
  ///
  /// @var Kind automatic
  /// @brief The player asked for the lowest-latency datacenter.
  ///
  /// @var Kind named
  /// @brief The player picked LaunchChoice::name.
  ///
  /// @var Kind timed_out
  /// @brief The player did not pick in time; the launch chooses automatically.
  ///
  /// @var Kind cancelled
  /// @brief The launch was stopped while waiting.
  enum class Kind : std::uint8_t { automatic, named, timed_out, cancelled };
  /// @brief How the wait ended; Kind::timed_out by default.
  Kind kind{Kind::timed_out};
  /// @brief The picked datacenter; set only for Kind::named.
  std::string name{};
};

/// @brief What a server reservation reports to and asks of the launch that
/// runs it.
///
/// Every member but LaunchObserver::on_progress has a default that offers
/// nothing, asks nothing and remembers nothing, which is what
/// LaunchObserver::silent does. Called on the launching thread. Not
/// copyable.
class LaunchObserver {
 public:
  /// @brief One datacenter's measured round trips and path MTUs.
  using Ping = psn::gk::Ping;

  /// @brief Builds the observer.
  LaunchObserver() = default;
  /// @brief Not copyable; an observer stands for one launch.
  LaunchObserver(const LaunchObserver&) = delete;
  /// @brief Not copy-assignable; an observer stands for one launch.
  auto operator=(const LaunchObserver&) -> LaunchObserver& = delete;
  /// @brief Allows destruction through the interface.
  virtual ~LaunchObserver() = default;

  /// @brief Gives an observer that ignores everything.
  ///
  /// @return A shared observer that lives for the program.
  static auto silent() noexcept -> LaunchObserver&;

  /// @brief Told each time progress changes.
  ///
  /// @param progress The progress.
  /// @param stage The stream failure stage.
  /// @param text The text shown to the player.
  virtual void on_progress(const LaunchProgress& progress,
                           net::protocol::remote_play::FailureStage stage,
                           std::string_view text) = 0;
  /// @brief Told the measurements once the datacenters are measured; ignored by
  /// default.
  ///
  /// @param picker_rows The rows shown to the player.
  /// @param measured_rows The measurements taken.
  virtual void on_datacenters(const std::vector<Ping>& picker_rows,
                              const std::vector<Ping>& measured_rows);
  /// @brief Told the datacenter selected; ignored by default.
  ///
  /// @param selected The selected datacenter.
  /// @param forced Whether it was forced or picked.
  virtual void on_datacenter_selected(const Ping& selected, bool forced);
  /// @brief Asks whether the player picks the datacenter at launch.
  ///
  /// @return `false` by default.
  virtual auto selects_datacenter_at_launch() const noexcept -> bool;
  /// @brief Offers datacenter names to the player; ignored by default.
  ///
  /// @param names The offered names.
  virtual void offer_datacenters(const std::vector<std::string>& names);
  /// @brief Waits for the player's pick.
  ///
  /// @param stop Cancels the wait.
  /// @return By default, LaunchChoice::Kind::cancelled when @p stop fired and
  ///     LaunchChoice::Kind::timed_out otherwise, without waiting.
  virtual auto await_datacenter_choice(const platform::StopToken& stop)
      -> LaunchChoice;
  /// @brief Asks how long the player has to pick.
  ///
  /// @return Empty by default.
  virtual auto datacenter_choice_wait() const noexcept
      -> std::optional<std::chrono::milliseconds>;
  /// @brief Asks for a remembered bandwidth to reuse.
  ///
  /// @param selected The selected datacenter.
  /// @return Empty by default.
  virtual auto cached_bandwidth(const Ping& selected)
      -> std::optional<CachedBandwidth>;
  /// @brief Told a measured bandwidth; ignored by default.
  ///
  /// @param selected The selected datacenter.
  /// @param kbps The bandwidth, in kilobits per second.
  virtual void on_bandwidth_measured(const Ping& selected, int kbps);

 private:
  /// @brief The observer that ignores everything; defined in the
  /// implementation.
  class Silent;
};

/// @brief Phrases launch progress for one service and tells an observer.
///
/// A small value holding a borrowed observer, which must outlive it. Progress
/// for an activity the service does not have is not reported.
class LaunchReport {
 public:
  /// @brief Step, attempt, measurement and wait counts of a progress report.
  using Counts = LaunchProgress::Counts;

  /// @brief Builds a report that tells nobody, for PS5 cloud.
  LaunchReport() noexcept;
  /// @brief Builds a report for one service.
  ///
  /// @param observer Told the progress; must outlive the report.
  /// @param service The service the launch runs on.
  LaunchReport(LaunchObserver& observer,
               catalog::ServiceGeneration service) noexcept
      : observer_(&observer), service_(service) {}

  /// @brief Reads the service.
  ///
  /// @return The service the progress is phrased for.
  auto service() const noexcept -> catalog::ServiceGeneration {
    return service_;
  }

  /// @brief Reports an activity.
  ///
  /// @param activity What the launch is doing.
  /// @param stage The stream failure stage.
  /// @param text The text shown to the player.
  /// @param counts Its counts.
  void say(LaunchActivity activity,
           net::protocol::remote_play::FailureStage stage,
           std::string_view text, Counts counts = {}) const;
  /// @brief Reports an activity with its text cut to a length.
  ///
  /// @param activity What the launch is doing.
  /// @param stage The stream failure stage.
  /// @param text The text shown to the player.
  /// @param capacity The most characters kept.
  /// @param counts Its counts.
  void say(LaunchActivity activity,
           net::protocol::remote_play::FailureStage stage, std::string text,
           std::size_t capacity, Counts counts = {}) const;
  /// @brief Reports a numbered step.
  ///
  /// @param activity What the launch is doing.
  /// @param stage The stream failure stage.
  /// @param label The step's label.
  /// @param step The step, from 1.
  /// @param total The number of steps.
  void step(LaunchActivity activity,
            net::protocol::remote_play::FailureStage stage,
            std::string_view label, int step, int total) const;
  /// @brief Reports a numbered step with its text cut to a length.
  ///
  /// @param activity What the launch is doing.
  /// @param stage The stream failure stage.
  /// @param label The step's label.
  /// @param step The step, from 1.
  /// @param total The number of steps.
  /// @param capacity The most characters kept.
  void step(LaunchActivity activity,
            net::protocol::remote_play::FailureStage stage,
            std::string_view label, int step, int total,
            std::size_t capacity) const;

  /// @brief Phrases a numbered step.
  ///
  /// @param label The step's label.
  /// @param step The step.
  /// @param total The number of steps.
  /// @return "label - Step step of total".
  static auto step_text(std::string_view label, int step, int total)
      -> std::string;
  /// @brief Counts a numbered step.
  ///
  /// @param step The step, from 1.
  /// @param total The number of steps.
  /// @return The counts; no step when @p step is outside 1 to @p total.
  static auto counted(int step, int total) noexcept -> Counts;
  /// @brief Counts an attempt.
  ///
  /// @param attempt The attempt, from 1.
  /// @return The counts; no attempt when @p attempt is below 1.
  static auto attempted(int attempt) noexcept -> Counts;
  /// @brief Counts an attempt that waits.
  ///
  /// @param reason Why it waits.
  /// @param attempt The attempt, from 1.
  /// @param delay_seconds How long it waits, in seconds; negative counts as 0.
  /// @return The counts.
  static auto waiting(progress::WaitReason reason, int attempt,
                      int delay_seconds) noexcept -> Counts;
  /// @brief Counts measurement progress within a numbered step.
  ///
  /// @param done Datacenters measured so far.
  /// @param total Datacenters to measure.
  /// @param step The step, from 1.
  /// @param steps The number of steps.
  /// @return The counts.
  static auto measured(std::size_t done, std::size_t total, int step,
                       int steps) noexcept -> Counts;

 private:
  /// @brief Told the progress; never null.
  LaunchObserver* observer_;
  /// @brief The service the progress is phrased for.
  catalog::ServiceGeneration service_{catalog::ServiceGeneration::ps5_cloud};
};

}  // namespace libchiaking::play
