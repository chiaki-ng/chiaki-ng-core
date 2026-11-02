/// @file
/// @brief Declares ConsoleCommands, which holds every request the player
/// queues for the console and sends them from the session loop.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/keyboard_state.hpp"
#include "libchiaking/detail/play/session.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/play/adapters/links/cloud/cloud_messaging.hpp"
#include "src/core/play/adapters/session/audio_3d_pacing.hpp"
#include "src/core/play/adapters/session/audio_route_advertisement.hpp"
#include "src/core/play/adapters/session/command_route.hpp"
#include "src/core/play/adapters/session/prompts/console_keyboard_core.hpp"
#include "src/core/play/adapters/session/prompts/login_pin_core.hpp"
#include "src/core/play/adapters/session/prompts/microphone_core.hpp"
#include "src/core/play/adapters/session/rest_request.hpp"
#include "src/core/play/adapters/session/seats_core.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::play {

/// @brief Sizes of the command queues and the audio routes to report. Plain
/// data.
struct CommandSettings {
  /// @brief The most microphone frames queued.
  std::size_t microphone_depth{Microphone::default_depth};
  /// @brief How long a session keeps running after the console accepted a rest
  /// request, so the request is delivered before the stream closes.
  std::chrono::milliseconds rest_flush{RestRequest::default_flush};
  /// @brief Whether PS5 cloud is told that each controller's speaker plays,
  /// beside the main output.
  bool controller_speaker{};
};

/// @brief What one pass of ConsoleCommands::service found. Plain data.
struct CommandService {
  /// @brief Whether a rest request was sent and its flush time is over, so the
  /// session should end.
  bool rest_finished{};
  /// @brief Whether that rest was asked for because the host is going to sleep.
  bool for_sleep{};
  /// @brief How long the session loop may sleep before the next pass.
  std::chrono::milliseconds wait{};
  /// @brief Seat requests the console refused in this pass.
  std::vector<SeatRefusal> refusals;
};

/// @brief Every command the player can send a console or cloud session: going
/// home, rest mode, 3D audio, the keyboard and login prompts, the microphone
/// and seats.
///
/// Frontend threads queue requests and wake the session thread; the session
/// loop calls ConsoleCommands::service to send them, most only while
/// streaming. The flags are atomics and each part locks its own mutex, so
/// queuing is safe from any thread. Owned by the session core; the route,
/// lifecycle, clock and seats must outlive it.
class ConsoleCommands {
 public:
  /// @brief Builds the commands with nothing queued.
  ///
  /// @param threading Provider of the commands' mutexes and of their
  ///     prompts'; must outlive this object.
  /// @param route Where commands go; must outlive this object.
  /// @param lifecycle The session's lifecycle; must outlive this object.
  /// @param clock The monotonic clock pacing rest and 3D audio and timing
  ///     the login prompt; must outlive this object.
  /// @param seats The session's seats; must outlive this object.
  /// @param settings Queue sizes, the rest flush time and the audio routes to
  ///     report.
  ConsoleCommands(platform::ThreadingProvider& threading,
                  const CommandRoute& route,
                  const LockedSessionLifecycle& lifecycle,
                  platform::ClockProvider& clock, Seats::Core& seats,
                  CommandSettings settings = {});

  /// @brief Queues a request to show the console's home screen and wakes the
  /// session thread.
  ///
  /// @return Nothing when queued; the route's console refusal;
  ///     InputRefusal::not_ready while a rest request is under way.
  auto go_home() -> Expected<void, InputRefusal>;
  /// @brief Switches 3D audio on or off.
  ///
  /// The change is sent while streaming, at most once per
  /// Audio3dPacing::spacing; before the stream is described the setting is
  /// only remembered and sent when the stream is ready.
  ///
  /// @param enabled Whether 3D audio is wanted.
  /// @return Nothing, also when the setting did not change or the session is
  ///     not running; InputRefusal::wrong_route on LegacyCloudStream.
  auto set_audio_3d(bool enabled) -> Expected<void, InputRefusal>;
  /// @brief Tells a PS5 cloud service whether the client's own UI covers the
  /// stream or the stream cannot be seen.
  ///
  /// The change is sent while streaming as two deep links; before then it is
  /// only remembered and sent when the stream runs.
  ///
  /// @param state What the client shows over or instead of the stream.
  /// @return Nothing, also when the state did not change;
  ///     InputRefusal::wrong_route outside PS5 cloud.
  auto set_system_ui(Session::SystemUi state) -> Expected<void, InputRefusal>;
  /// @brief Queues changed accessibility settings for a PS5 cloud service;
  /// the latest settings replace ones not yet sent, and they are sent once
  /// the stream runs.
  ///
  /// @param accessibility The settings the player now wants.
  /// @return Nothing when queued or equal to the last ones queued;
  ///     InputRefusal::wrong_route outside PS5 cloud.
  auto set_accessibility(const AccessibilityRequest& accessibility)
      -> Expected<void, InputRefusal>;
  /// @brief Builds the `psaction:accessibility` deep link for settings.
  ///
  /// @param accessibility The settings to describe.
  /// @return The deep link text.
  static auto accessibility_deep_link(const AccessibilityRequest& accessibility)
      -> std::string;
  /// @brief Queues a Create button press for a PS5 cloud service; the latest
  /// press replaces one not yet sent.
  ///
  /// @param press How the button was pressed.
  /// @return Nothing when queued; InputRefusal::wrong_route outside PS5
  ///     cloud; InputRefusal::not_running when commands are not accepted.
  auto press_create(Session::CreatePress press) -> Expected<void, InputRefusal>;
  /// @brief Queues a request to open a LegacyCloudStream service's Application
  /// Saved Data Management screen; a request not yet sent absorbs another.
  ///
  /// @return Nothing when queued; InputRefusal::wrong_route outside
  /// LegacyCloudStream;
  ///     InputRefusal::not_running when commands are not accepted.
  auto open_saved_data() -> Expected<void, InputRefusal>;
  /// @brief Queues a game invitation for the PS5 cloud or LegacyCloudStream
  /// service to join; the latest invitation replaces one not yet sent.
  ///
  /// @param invitation_id The invitation's id.
  /// @param session_id The invited game session's id.
  /// @return Nothing when queued; InputRefusal::wrong_route outside the
  ///     cloud; InputRefusal::not_running when commands are not accepted.
  auto join_invitation(std::string invitation_id, std::string session_id)
      -> Expected<void, InputRefusal>;
  /// @brief Sends the keyboard's state to the console at once, on the calling
  /// thread.
  ///
  /// @param state The keys down.
  /// @return Nothing, also when sending failed, which is logged;
  ///     InputRefusal::not_running when commands are not accepted.
  auto send_keyboard(const KeyboardState& state)
      -> Expected<void, InputRefusal>;
  /// @brief Reads the 3D audio setting.
  ///
  /// @return Whether 3D audio is wanted.
  auto audio_3d() const noexcept -> bool;

  /// @brief Gives the rest request.
  ///
  /// @return The request, owned by this object.
  auto rest() noexcept -> RestRequest& { return rest_; }
  /// @brief Gives the rest request.
  ///
  /// @return The request, owned by this object.
  auto rest() const noexcept -> const RestRequest& { return rest_; }
  /// @brief Gives the login prompt.
  ///
  /// @return The prompt, owned by this object.
  auto pin() noexcept -> LoginPin::Core& { return pin_; }
  /// @brief Gives the keyboard prompt.
  ///
  /// @return The prompt, owned by this object.
  auto keyboard() noexcept -> ConsoleKeyboard::Core& { return keyboard_; }
  /// @brief Gives the microphone.
  ///
  /// @return The microphone, owned by this object.
  auto microphone() noexcept -> Microphone::Core& { return microphone_; }
  /// @brief Gives the microphone.
  ///
  /// @return The microphone, owned by this object.
  auto microphone() const noexcept -> const Microphone::Core& {
    return microphone_;
  }

  /// @brief Watches stream events for the keyboard prompt and the stream
  /// description; once the stream is described, wanted 3D audio is sent and
  /// PS5 cloud is told the audio routes.
  ///
  /// @param event An event from the link.
  void observe(const net::protocol::remote_play::StreamEvent& event);
  /// @brief Sends wanted 3D audio again after the audio profile changed.
  void audio_profile_changed();
  /// @brief Reports whether anything waits to be sent.
  ///
  /// @param streaming Whether the stream is streaming.
  /// @return `true` for a queued seat passcode or rest request; while
  ///     streaming also for queued seats, going home, microphone, keyboard,
  ///     due 3D audio or a due audio route report.
  auto pending(bool streaming) const -> bool;
  /// @brief Sends what is due; called by the session loop on its thread.
  ///
  /// The login code, the seat passcode and the rest request go in any state;
  /// seats, going home, microphone, keyboard, 3D audio and the audio routes
  /// only while streaming. Blocks on the link for each send.
  ///
  /// @param state The stream's state.
  /// @param wait The longest the loop would sleep.
  /// @return Whether the rest is over and why, how long the loop may sleep, and
  ///     the seat refusals.
  auto service(net::protocol::remote_play::StreamState state,
               std::chrono::milliseconds wait) -> CommandService;
  /// @brief Wakes threads waiting on the login prompt.
  void interrupt() noexcept;
  /// @brief Wakes the login prompt's waiters and drops queued seat requests, as
  /// the session ends.
  void conclude() noexcept;
  /// @brief Drops queued seat requests.
  void forget_seats() noexcept;

 private:
  /// @brief The deep link that tells the console what covers the game and
  /// which system services to show, before its one parameter.
  static constexpr std::string_view system_service_status =
      "psaction:systemservicestatus";
  /// @brief The deep link that sets the cloud console's accessibility
  /// options, before its parameters.
  static constexpr std::string_view accessibility_action =
      "psaction:accessibility";

  /// @brief Builds a system service status deep link.
  ///
  /// @param name The parameter's name.
  /// @param value The parameter's value.
  /// @return `psaction:systemservicestatus?<name>=<value>`, the value
  ///     percent-encoded.
  static auto system_service_status_link(std::string_view name,
                                         std::string_view value) -> std::string;
  /// @brief The PS5 cloud audio state selector for 3D audio.
  static constexpr std::uint8_t audio_3d_selector = 3;
  /// @brief The PS5 cloud audio state value meaning 3D audio is available.
  static constexpr std::uint32_t audio_3d_available = 0xff;
  /// @brief The console audio profile for stereo.
  static constexpr std::uint8_t stereo_profile = 0;
  /// @brief The console audio profile for 3D audio.
  static constexpr std::uint8_t audio_3d_profile = 4;

  /// @brief Sends the 3D audio setting: as a channel-count profile to a console
  /// or LegacyCloudStream, as audio state flags to PS5 cloud. Logs the result.
  ///
  /// @param enabled Whether 3D audio is on.
  /// @param reason Why it is sent, for the log.
  void send_audio_3d(bool enabled, std::string_view reason);
  /// @brief Reports whether a queued 3D audio change may be sent now.
  ///
  /// @return `true` when one is queued and the stream is not yet described or
  ///     the pacing allows it.
  auto audio_3d_due() const noexcept -> bool;
  /// @brief Tells PS5 cloud where audio plays: the main output and, when
  /// wanted, each controller's speaker. The first report also describes a
  /// 7.1 speaker layout. Logs the first results and any failure.
  ///
  /// @param first Whether this is the report sent when the stream is ready.
  /// @param reason Why it is sent, for the log.
  void report_audio_routes(bool first, std::string_view reason);

  /// @brief Where commands go.
  const CommandRoute& route_;
  /// @brief The monotonic clock.
  platform::ClockProvider& clock_;
  /// @brief Spaces 3D audio changes.
  Audio3dPacing audio_3d_pacing_;
  /// @brief Repeats the audio route report while a title starts.
  AudioRouteAdvertisement audio_routes_;
  /// @brief Whether each controller's speaker is reported as playing.
  bool controller_speaker_{};
  /// @brief The rest request.
  RestRequest rest_;
  /// @brief The login prompt.
  LoginPin::Core pin_;
  /// @brief The session's seats.
  Seats::Core& seats_;
  /// @brief The keyboard prompt.
  ConsoleKeyboard::Core keyboard_;
  /// @brief The microphone.
  Microphone::Core microphone_;
  /// @brief Whether going home is queued.
  std::atomic<bool> go_home_pending_{};
  /// @brief Whether 3D audio is wanted.
  std::atomic<bool> audio_3d_{};
  /// @brief Whether a 3D audio change is queued.
  std::atomic<bool> audio_3d_pending_{};
  /// @brief Whether the stream has been described, so audio state can be sent.
  std::atomic<bool> audio_configured_{};
  /// @brief What the client shows over or instead of the stream.
  std::atomic<Session::SystemUi> system_ui_{Session::SystemUi::game};
  /// @brief Whether a change of system_ui_ is queued.
  std::atomic<bool> system_ui_pending_{};
  /// @brief Guards accessibility_wanted_ and accessibility_sent_.
  mutable platform::Mutex accessibility_mutex_;
  /// @brief Accessibility settings queued and not yet sent.
  std::optional<AccessibilityRequest> accessibility_wanted_;
  /// @brief The accessibility settings last queued, so repeats are dropped;
  /// forgotten when sending them fails, so a repeat is sent again.
  std::optional<AccessibilityRequest> accessibility_sent_;
  /// @brief Whether accessibility_wanted_ holds settings, read without the
  /// lock.
  std::atomic<bool> accessibility_pending_{};
  /// @brief The queued Create press plus one; 0 when none is queued.
  std::atomic<std::uint8_t> create_pending_{};
  /// @brief Guards invitation_wanted_.
  mutable platform::Mutex invitation_mutex_;
  /// @brief The invitation to join, as its id and its session's id, until
  /// the session's thread sends it.
  std::optional<std::pair<std::string, std::string>> invitation_wanted_;
  /// @brief Whether invitation_wanted_ holds an invitation, read without the
  /// lock.
  std::atomic<bool> invitation_pending_{};
  /// @brief Whether an Application Saved Data Management request waits to be
  /// sent.
  std::atomic<bool> saved_data_pending_{};

  /// @brief Sends the deep links describing system_ui_.
  void send_system_ui_state();
  /// @brief Sends queued accessibility settings, if any.
  void flush_accessibility();
  /// @brief Gives the gateway press for a Create press.
  ///
  /// @param press The press.
  /// @return The gateway press.
  static auto capture_press(Session::CreatePress press) noexcept
      -> CloudMessaging::CapturePress;
};

}  // namespace libchiaking::play
