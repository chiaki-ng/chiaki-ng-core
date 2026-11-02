/// @file
/// @brief Declares usecases::LivePlay, the object that owns a started Session
/// and answers the link and the launch on its behalf.
#pragma once

#include "libchiaking/detail/consoles/known_consoles.hpp"
#include "libchiaking/detail/play/cloud_launch.hpp"
#include "libchiaking/detail/play/session.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "src/core/consoles/paired_play.hpp"
#include "src/core/play/adapters/cloud_launch/cloud_launch_core.hpp"
#include "src/core/play/adapters/links/internet/internet_start.hpp"
#include "src/core/play/adapters/links/reconnecting_link.hpp"
#include "src/core/play/adapters/links/stream_platform.hpp"
#include "src/core/play/adapters/session/session_core.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace libchiaking::play::usecases {

/// @brief One started session together with the player and console it was
/// started for, and the reports that flow back from its link.
///
/// The start use case builds it, seats the Session::Core in it with
/// LivePlay::seat and hands the caller a std::shared_ptr<Session> that shares
/// ownership of this object, so it lives exactly as long as anyone holds the
/// session. It forwards cloud launch progress and datacenter offers and
/// choices to the session's events, saves a pairing made during an internet
/// start and the console's reported name through consoles::PairedPlay::Reports,
/// and lets a ReconnectingLink wake the console and report its retries.
///
/// The link reports and watcher calls arrive on the session's run thread;
/// LivePlay::seat, LivePlay::reached_as and LivePlay::woken_by are called by
/// the start before the session begins. Not copyable.
class LivePlay final : private Session::Core::LinkReports,
                       private ReconnectingLink::Watcher,
                       private InternetStart::PairingListener {
 public:
  /// @brief Builds the holder for one start.
  ///
  /// @param known The known consoles, through which pairings and names are
  ///     saved; must outlive this object.
  /// @param player The user playing.
  /// @param console The console played on; null for a cloud or scripted start,
  ///     in which case nothing is saved and waking falls back to
  ///     LivePlay::woken_by.
  /// @param log Receives a warning when a pairing cannot be saved; may be null.
  LivePlay(consoles::KnownConsoles& known,
           std::shared_ptr<users::PsUser> player,
           std::shared_ptr<consoles::Console> console,
           platform::DiagnosticsProvider* log);
  /// @brief Not copyable; it owns the session.
  LivePlay(const LivePlay&) = delete;
  /// @brief Not copy-assignable; it owns the session.
  auto operator=(const LivePlay&) -> LivePlay& = delete;

  /// @brief Gives the watcher a ReconnectingLink reports to.
  ///
  /// @return This object, seen as the watcher.
  auto link_watcher() noexcept -> ReconnectingLink::Watcher& { return *this; }
  /// @brief Gives the listener told when an internet start pairs the console.
  ///
  /// @return This object, seen as the pairing listener.
  auto pairings() noexcept -> InternetStart::PairingListener* { return this; }
  /// @brief Records the device the internet start reached, so a pairing made
  /// on the way can be saved against it.
  ///
  /// @param device_uid The console's PSN device id.
  void reached_as(const consoles::DeviceUid& device_uid);
  /// @brief Gives the platform the session's link runs on, keeping it for as
  /// long as this object lives.
  ///
  /// Call once, before the link is built. A stream given its own provider
  /// also wakes the console through it.
  ///
  /// @param host The host's platform; must outlive this object.
  /// @param network The stream's own network provider, or null for the
  ///     host's.
  /// @return @p host when @p network is null; otherwise a StreamPlatform over
  ///     @p host, owned by this object and outliving the session.
  auto streams_over(platform::PlatformFactory& host,
                    std::shared_ptr<platform::NetworkProvider> network)
      -> platform::PlatformFactory&;
  /// @brief Replaces waking the console with a script, for scripted starts.
  ///
  /// @param waker Called instead of waking the console; returns whether the
  ///     wake worked.
  void woken_by(std::function<bool()> waker);

  /// @brief Takes ownership of the session core and builds the Session around
  /// it.
  ///
  /// Attaches this object as the core's link reports and, for a cloud start, a
  /// CloudLaunch face over @p launch. Call once.
  ///
  /// @param core The opened session core.
  /// @param launch The cloud launch the core's link drives; null for consoles.
  /// @return The seated core, owned by this object.
  auto seat(std::unique_ptr<Session::Core> core, CloudLaunch::Core* launch)
      -> Session::Core&;
  /// @brief Gives the session seated by LivePlay::seat.
  ///
  /// @return The session; calling it before LivePlay::seat is undefined.
  auto session() noexcept -> Session& { return *session_; }

 private:
  /// @brief Forwards the launch's progress to the session's events; ignored
  /// without a cloud launch.
  ///
  /// @param progress The link's setup event; the launch's own progress is sent
  ///     instead.
  void launch_progress(const net::protocol::remote_play::event::SetupProgress&
                           progress) override;
  /// @brief Offers the measured datacenters to the session's events, when the
  /// cloud launch has an offer; ignored without a cloud launch.
  ///
  /// @param measured The link's event; the launch's offer is sent instead.
  void datacenters_measured(
      const net::protocol::remote_play::event::CloudDatacenters& measured)
      override;
  /// @brief Reports the datacenter the cloud launch chose to the session's
  /// events; ignored without a cloud launch.
  ///
  /// @param selected The link's event; the launch's choice is sent instead.
  void datacenter_selected(
      const net::protocol::remote_play::event::CloudDatacenterSelected&
          selected) override;
  /// @brief Gives the reason the cloud launch was refused.
  ///
  /// @return The refusal; empty without a cloud launch or when it was not
  ///     refused.
  auto launch_refused() -> std::optional<LaunchRefused> override;
  /// @brief Saves the name the console reported; ignored without a console.
  ///
  /// @param name The console's name.
  void console_named(std::string_view name) override;
  /// @brief Saves a pairing an internet start made, against the device recorded
  /// by LivePlay::reached_as. Logs a warning when saving fails.
  ///
  /// A PS4 reached that the player has not paired is turned away unsaved; see
  /// consoles::PairedPlay::Reports::recognises.
  ///
  /// @param paired The new pairing.
  /// @return Nothing when the pairing was taken, saved or not;
  ///     PairingListener::Refusal::unpaired_console when it was turned away.
  [[nodiscard]] auto on_paired(consoles::PairedConsole paired)
      -> Expected<void, Refusal> override;
  /// @brief Wakes the console before a retry; blocks while the wake request is
  /// sent.
  ///
  /// The wake goes through the stream's own network provider when it has
  /// one, and the platform's otherwise.
  ///
  /// @return The scripted waker's answer when one is set; otherwise whether the
  ///     console accepted the wake; `false` without a console.
  auto wake_console() -> bool override;
  /// @brief Reports whether the player is leaving, so the link stops retrying.
  ///
  /// @return `true` once the session is in SessionState::leaving.
  auto leaving() const noexcept -> bool override;
  /// @brief Tells the session another attempt is about to start.
  ///
  /// @param attempt The number of the coming attempt, counting from 1.
  /// @param woke Whether the console was woken first.
  /// @param reconnecting Whether a stream had already started before the
  ///     failure.
  /// @param failure Why the previous attempt ended.
  /// @param rest How long the link rests before the attempt.
  void trying_again(std::uint32_t attempt, bool woke, bool reconnecting,
                    const StartPolicy::Failure& failure,
                    std::chrono::milliseconds rest) override;

  /// @brief The user playing.
  std::shared_ptr<users::PsUser> player_;
  /// @brief The console played on; null for cloud and scripted starts.
  std::shared_ptr<consoles::Console> console_;
  /// @brief Receives log lines; may be null.
  platform::DiagnosticsProvider* log_;
  /// @brief Saves pairings and names; present only with a console.
  std::optional<consoles::PairedPlay::Reports> reports_;
  /// @brief The device an internet start reached; set by LivePlay::reached_as.
  std::optional<consoles::DeviceUid> device_uid_;
  /// @brief The scripted replacement for waking the console; empty normally.
  std::function<bool()> scripted_wake_;
  /// @brief The platform of a stream with its own network provider; empty
  /// otherwise. Declared before the session so that it outlives it.
  std::optional<StreamPlatform> stream_platform_;
  /// @brief The stream's own network provider, which wakes go through;
  /// default for the platform's.
  consoles::HomeNetwork home_network_;
  /// @brief The seated core, owned by LivePlay::session_; null until seated.
  Session::Core* core_{};
  /// @brief The cloud launch of a cloud start, owned by its link; null
  /// otherwise.
  CloudLaunch::Core* launch_{};
  /// @brief The public face of the cloud launch, attached to the core.
  std::optional<CloudLaunch> face_;
  /// @brief The session, holding the core.
  std::optional<Session> session_;
};

}  // namespace libchiaking::play::usecases
