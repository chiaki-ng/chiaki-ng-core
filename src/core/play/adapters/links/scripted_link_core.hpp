/// @file
/// @brief Declares ScriptedLink::Core, the implementation behind the public
/// testing::ScriptedLink: a fake link a test scripts step by step, and the
/// Script and Wire it is built from.
#pragma once

#include "libchiaking/detail/play/session.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "libchiaking/testing/play/scripted_link.hpp"
#include "src/core/net/protocol/takion/source_tap.hpp"
#include "src/core/play/adapters/links/link.hpp"
#include "src/core/play/adapters/links/link_driver.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::play::testing {

/// @brief The implementation behind testing::ScriptedLink: hands the start
/// use case links that play a test's script, and records what the session
/// sent them.
///
/// Every link it builds shares one Script, so a retried attempt sees the
/// same script and records. The test thread queues actions; the session
/// thread runs them on its next pump, or at once when the test calls from
/// inside a pump. Owned by the public ScriptedLink; not copyable.
class ScriptedLink::Core {
 public:
  /// @brief The fake link the session drives; defined below.
  class Wire;
  /// @brief One step a test scripts, run on the link during a pump.
  using Action = std::function<void(Wire&)>;

  /// @brief Builds the core with an empty script.
  ///
  /// @param face The public object; must outlive the core.
  /// @param threading Provider of the script's mutex and condition variable;
  ///     must outlive the core.
  /// @param clock Monotonic clock the script's waits measure on; must
  ///     outlive the core.
  Core(ScriptedLink& face, platform::ThreadingProvider& threading,
       const platform::ClockProvider& clock);
  /// @brief Not copyable; it stands for one public object.
  Core(const Core&) = delete;
  /// @brief Not copy-assignable; it stands for one public object.
  auto operator=(const Core&) -> Core& = delete;

  /// @brief Builds a fresh scripted link for each attempt.
  using Opener =
      std::function<Expected<std::unique_ptr<Link>,
                             net::protocol::remote_play::StreamError>()>;

  /// @brief Builds one scripted link.
  ///
  /// @param stop Stops the link.
  /// @param where Where it pretends to run.
  /// @param old_service Whether a cloud link pretends to be LegacyCloudStream.
  /// @return The link.
  auto link(platform::StopSource stop, Where where, bool old_service = false)
      -> std::unique_ptr<Link>;
  /// @brief Builds an opener that makes a scripted link for each attempt of a
  /// reconnecting link.
  ///
  /// @param stop Stops each link.
  /// @param where Where they pretend to run.
  /// @return The opener.
  auto opener(platform::StopSource stop, Where where) -> Opener;
  /// @brief Builds a console waker that only records "wake_console".
  ///
  /// @return A waker that always succeeds.
  auto waker() -> std::function<bool()>;
  /// @brief Runs an action on the link now when called from inside a pump on
  /// the pumping thread, otherwise queues it for the next pump and wakes the
  /// link.
  ///
  /// @param action The action.
  void act(Action action);
  /// @brief Reports an event through the link.
  ///
  /// @param reported The event.
  void report(net::protocol::remote_play::StreamEvent reported);
  /// @brief Sets where scripted cloud launch progress and datacenter events go.
  ///
  /// @param events The session's events; must outlive the script.
  void report_launch_to(SessionEvents& events) noexcept;
  /// @brief Sets the share's tap and crypto, for forwarding scripted media,
  /// and resets stepping.
  ///
  /// @param tap The share's source tap; must outlive the script.
  /// @param crypto The crypto provider used to seal shared media; may be null.
  void share_through(net::protocol::takion::SourceTap& tap,
                     platform::CryptoProvider* crypto) noexcept;
  /// @brief Makes the link park in each wait until the test grants a turn.
  void step_by_hand();
  /// @brief Blocks until the link parks, ends or stops stepping.
  void settle();
  /// @brief Grants one pump while stepping by hand and blocks until the link
  /// parks again.
  ///
  /// @return `true` when the pump ran and the link parked; `false` when not
  ///     stepping or the link ended.
  auto turn() -> bool;
  /// @brief Reports cloud launch progress to the session's events.
  ///
  /// @param progress The progress.
  void launch_progress(LaunchProgress progress);
  /// @brief Offers datacenters to the session's events.
  ///
  /// @param offer The offer.
  void offer_datacenters(DatacenterOffer offer);
  /// @brief Reports a datacenter choice to the session's events.
  ///
  /// @param choice The choice.
  void choose_datacenter(DatacenterChoice choice);

  /// @brief Makes every later start fail with a failure.
  ///
  /// @param detail The failure.
  void refuse_start(LinkFailure detail);
  /// @brief Makes later seat account and join commands fail.
  ///
  /// @param error The error they fail with.
  void refuse_seat_requests(net::protocol::remote_play::StreamError error);
  /// @brief Makes later login code submissions fail.
  ///
  /// @param error The error they fail with.
  void refuse_login_pin(net::protocol::remote_play::StreamError error);
  /// @brief Sets the Takion version the link reports.
  ///
  /// @param version The version.
  void report_protocol_version(std::uint32_t version);

  /// @brief Reads every call the session made, by name, in order.
  ///
  /// @return The names.
  auto calls() const -> std::vector<std::string>;
  /// @brief Reads the controller states sent for a seat.
  ///
  /// @param seat The seat.
  /// @return The states in order.
  auto controller_samples(controllers::SeatNumber seat) const
      -> std::vector<controllers::ControllerState>;
  /// @brief Reads the seat requests sent.
  ///
  /// @return The requests in order.
  auto seat_requests() const -> std::vector<SeatRequest>;
  /// @brief Counts the keyframe requests.
  ///
  /// @return The count.
  auto keyframe_requests() const -> std::size_t;
  /// @brief Reads the last login code sent.
  ///
  /// @return A wiping copy of the digits; empty when none.
  auto login_pin() const -> SecretString;
  /// @brief Gives the public object.
  ///
  /// @return The public ScriptedLink.
  auto face() noexcept -> ScriptedLink& { return face_; }

 private:
  /// @brief The shared script and records; defined below.
  class Script;

  /// @brief The public object.
  ScriptedLink& face_;
  /// @brief The script shared with every link built.
  std::shared_ptr<Script> script_;
};

/// @brief What every scripted link shares: queued actions, stepping, the
/// share's tap and keys, refusals, and records of every call.
///
/// Every member locks one mutex, except the atomics read by deliveries, so
/// the test thread and the session thread may share it.
class ScriptedLink::Core::Script {
 public:
  /// @brief Makes an empty script.
  ///
  /// @param threading Provider of the script's mutex and condition variable
  ///     and of the crypto that seals shared media; must outlive the script.
  /// @param clock Monotonic clock the script's waits measure on; must
  ///     outlive the script.
  Script(platform::ThreadingProvider& threading,
         const platform::ClockProvider& clock)
      : threading_(threading), mutex_(threading), changed_(threading, clock) {}

  /// @brief Gives the threading provider the script was made with.
  ///
  /// @return The provider.
  auto threading() const noexcept -> platform::ThreadingProvider& {
    return threading_;
  }

  /// @brief Queues an action and wakes the waiting link.
  ///
  /// @param action The action.
  void enqueue(Action action);
  /// @brief Takes every queued action and clears the wake.
  ///
  /// @return The actions, oldest first.
  auto take() -> std::deque<Action>;
  /// @brief Waits for an action or a wake, up to a time; while stepping by
  /// hand, parks until granted a turn instead.
  ///
  /// @param timeout The longest wait.
  void wait(std::chrono::milliseconds timeout);
  /// @brief Wakes the waiting link.
  ///
  /// @param leaving Whether the link is stopping, which ends stepping by hand.
  void wake(bool leaving) noexcept;
  /// @brief Records the link being pumped on this thread.
  ///
  /// @param wire The link; null when the pump ends.
  void pumping(Wire* wire) noexcept;
  /// @brief Gives the link being pumped, when on the pumping thread.
  ///
  /// @return The link; null elsewhere or between pumps.
  auto pumped_here() const noexcept -> Wire*;

  /// @brief Records a call.
  ///
  /// @param call Its name.
  void note(std::string call);
  /// @brief Records a controller state, by its seat.
  ///
  /// @param state The wire state.
  void sample(const net::protocol::remote_play::ControllerState& state);
  /// @brief Records a seat request.
  ///
  /// @param request The request.
  void seat_request(SeatRequest request);
  /// @brief Records a keyframe request.
  void keyframe();
  /// @brief Records a login code.
  ///
  /// @param digits The code's digits, copied into wiping storage.
  void pin(std::span<const std::byte> digits);

  /// @brief Sets where launch events go.
  ///
  /// @param events The session's events; null for none.
  void report_launch_to(SessionEvents* events) noexcept;
  /// @brief Gives where launch events go.
  ///
  /// @return The events; null when unset.
  auto launch_events() const noexcept -> SessionEvents*;
  /// @brief Sets the share's tap and crypto and resets stepping.
  ///
  /// @param tap The tap; null for none.
  /// @param crypto The crypto provider; null for none.
  void share_through(net::protocol::takion::SourceTap* tap,
                     platform::CryptoProvider* crypto) noexcept;
  /// @brief Gives the share's tap.
  ///
  /// @return The tap; null when unset.
  auto tap() const noexcept -> net::protocol::takion::SourceTap*;
  /// @brief Gives the crypto provider.
  ///
  /// @return The provider; null when unset.
  auto crypto() const noexcept -> platform::CryptoProvider*;
  /// @brief Makes new receive keys from a fixed pseudo-random sequence and
  /// keeps them.
  ///
  /// @return A copy of the keys.
  auto fresh_keys() -> net::protocol::takion::SourceReceiveKeys;
  /// @brief Gives the current receive keys.
  ///
  /// @return A copy; empty before the first keys.
  auto keys() const -> std::optional<net::protocol::takion::SourceReceiveKeys>;
  /// @brief Starts stepping by hand.
  void step_by_hand();
  /// @brief Blocks until the link parks, ends or stops stepping.
  void settle();
  /// @brief Grants one turn and blocks until the link parks again.
  ///
  /// @return `true` when it parked again; `false` when not stepping or ended.
  auto turn() -> bool;
  /// @brief Marks the link ended or running, waking every waiter.
  ///
  /// @param finished Whether it ended.
  void over(bool finished) noexcept;

  /// @brief Keeps a start refusal.
  ///
  /// @param detail The failure.
  void refuse_start(LinkFailure detail);
  /// @brief Keeps a seat refusal.
  ///
  /// @param error The error.
  void refuse_seat_requests(net::protocol::remote_play::StreamError error);
  /// @brief Keeps a login code refusal.
  ///
  /// @param error The error.
  void refuse_login_pin(net::protocol::remote_play::StreamError error);
  /// @brief Keeps the Takion version to report.
  ///
  /// @param version The version.
  void report_protocol_version(std::uint32_t version);
  /// @brief Reads the start refusal.
  ///
  /// @return The failure; empty when none.
  auto start_refusal() const -> std::optional<LinkFailure>;
  /// @brief Reads the seat refusal.
  ///
  /// @return The error; empty when none.
  auto seat_refusal() const
      -> std::optional<net::protocol::remote_play::StreamError>;
  /// @brief Reads the login code refusal.
  ///
  /// @return The error; empty when none.
  auto pin_refusal() const
      -> std::optional<net::protocol::remote_play::StreamError>;
  /// @brief Reads the Takion version to report.
  ///
  /// @return The version; empty when none.
  auto protocol_version() const -> std::optional<std::uint32_t>;

  /// @brief Reads every call recorded.
  ///
  /// @return The names in order.
  auto calls() const -> std::vector<std::string>;
  /// @brief Reads a seat's controller states.
  ///
  /// @param seat The seat.
  /// @return The states in order.
  auto controller_samples(controllers::SeatNumber seat) const
      -> std::vector<controllers::ControllerState>;
  /// @brief Reads the seat requests.
  ///
  /// @return The requests in order.
  auto seat_requests() const -> std::vector<SeatRequest>;
  /// @brief Counts the keyframe requests.
  ///
  /// @return The count.
  auto keyframe_requests() const -> std::size_t;
  /// @brief Reads the last login code.
  ///
  /// @return A wiping copy of the digits; empty when none.
  auto login_pin() const -> SecretString;

 private:
  /// @brief Makes the crypto that seals shared media.
  platform::ThreadingProvider& threading_;
  /// @brief Guards everything below but the atomics.
  mutable platform::Mutex mutex_;
  /// @brief Signalled on actions, wakes, turns, parking and ending.
  platform::ConditionVariable changed_;
  /// @brief Actions not yet run.
  std::deque<Action> actions_;
  /// @brief Whether a wake arrived.
  bool woken_{};
  /// @brief The link being pumped.
  Wire* wire_{};
  /// @brief The thread pumping it.
  platform::ThreadIdentity pump_thread_{};
  /// @brief Calls recorded, by name.
  std::vector<std::string> calls_;
  /// @brief Controller states recorded per seat.
  std::array<std::vector<controllers::ControllerState>,
             controllers::SeatNumber::count>
      samples_;
  /// @brief Seat requests recorded.
  std::vector<SeatRequest> seat_requests_;
  /// @brief Keyframe requests counted.
  std::size_t keyframes_{};
  /// @brief The last login code.
  SecretString pin_;
  /// @brief The start refusal.
  std::optional<LinkFailure> start_refusal_;
  /// @brief The seat refusal.
  std::optional<net::protocol::remote_play::StreamError> seat_refusal_;
  /// @brief The login code refusal.
  std::optional<net::protocol::remote_play::StreamError> pin_refusal_;
  /// @brief The Takion version to report.
  std::optional<std::uint32_t> protocol_version_;
  /// @brief Where launch events go.
  std::atomic<SessionEvents*> launch_events_{};
  /// @brief The share's tap.
  std::atomic<net::protocol::takion::SourceTap*> tap_{};
  /// @brief The crypto provider.
  std::atomic<platform::CryptoProvider*> crypto_{};
  /// @brief The current receive keys.
  std::optional<net::protocol::takion::SourceReceiveKeys> keys_;
  /// @brief The pseudo-random state receive keys are drawn from.
  std::uint64_t key_state_{0x5343524950544544ULL};
  /// @brief Whether the test steps the link by hand.
  bool stepped_{};
  /// @brief Whether the link is parked waiting for a turn.
  bool parked_{};
  /// @brief Whether the link ended.
  bool over_{};
  /// @brief Turns granted but not yet taken.
  std::size_t turns_{};
};

/// @brief A scripted link: starts into establishing the route, runs the
/// script's actions on each pump, records every call and refuses what the
/// script says.
///
/// Used on the session thread like any link.
class ScriptedLink::Core::Wire final : public Link {
 public:
  /// @brief The result of a link call.
  using Result = Expected<void, net::protocol::remote_play::StreamError>;

  /// @brief Builds the link.
  ///
  /// @param script The shared script.
  /// @param stop Stops the link.
  /// @param venue Where it pretends to run.
  Wire(std::shared_ptr<Script> script, platform::StopSource stop,
       std::optional<Venue> venue);

  /// @brief Starts into establishing the route.
  ///
  /// @return Nothing; StreamError::route_failed when already stopped; the
  ///     refusal's error when the script refuses starts.
  auto start() -> Result override;
  /// @brief Finishes when stopped while running; otherwise runs the queued
  /// actions.
  ///
  /// @return Nothing.
  auto pump_once() -> Result override;
  /// @brief Waits on the script.
  ///
  /// @param timeout The longest wait.
  /// @return Nothing.
  auto wait(std::chrono::milliseconds timeout) -> Result override;
  /// @brief Wakes the script.
  void wake() noexcept override;
  /// @brief Records "stop" and finishes.
  ///
  /// @return Nothing.
  auto stop() -> Result override;
  /// @brief Records "stop_for_sleep" and finishes.
  ///
  /// @return Nothing.
  auto stop_for_sleep() -> Result override;
  /// @brief Reads the state.
  ///
  /// @return The state.
  auto state() const noexcept
      -> net::protocol::remote_play::StreamState override {
    return state_;
  }
  /// @brief Records a controller state.
  ///
  /// @param state The state.
  /// @return Nothing.
  auto send_controller_state(
      const net::protocol::remote_play::ControllerState& state)
      -> Result override;
  /// @brief Records the login code.
  ///
  /// @param pin The code's bytes.
  /// @return Nothing; the script's refusal when it refuses codes.
  auto submit_pin(std::span<const std::byte> pin) -> Result override;
  /// @brief Records "video_loss".
  ///
  /// @param first Ignored.
  /// @param last Ignored.
  /// @return Nothing.
  auto report_video_loss(std::uint16_t first, std::uint16_t last)
      -> Result override;
  /// @brief Records a command, and a seat request for couch commands.
  ///
  /// @param command The command.
  /// @return Nothing; the script's seat refusal for account and join commands
  ///     when it refuses them.
  auto execute(const net::protocol::remote_play::StreamCommand& command)
      -> Result override;
  /// @brief Records "microphone_frame".
  ///
  /// @param frame Ignored.
  /// @return Nothing.
  auto send_microphone(std::span<const std::byte> frame) -> Result override;
  /// @brief Records "microphone_connect".
  ///
  /// @return Nothing.
  auto connect_microphone() -> Result override;
  /// @brief Records "microphone_mute" or "microphone_unmute".
  ///
  /// @param muted Whether to mute.
  /// @return Nothing.
  auto set_microphone_muted(bool muted) -> Result override;
  /// @brief Records a keyframe request.
  ///
  /// @return Nothing.
  auto request_idr() -> Result override;
  /// @brief Records "audio_state".
  ///
  /// @param kind Ignored.
  /// @param body Ignored.
  /// @return Nothing.
  auto send_audio_state(net::protocol::remote_play::AudioStateKind kind,
                        std::span<const std::byte> body) -> Result override;
  /// @brief Records "keyboard".
  ///
  /// @param state Ignored.
  /// @return Nothing.
  auto send_keyboard(const KeyboardState& state) -> Result override;
  /// @brief Sets where events go.
  ///
  /// @param sink The receiver; must outlive the link.
  void set_event_sink(LinkEventSink& sink) noexcept override { sink_ = &sink; }
  /// @brief Reads the Takion version the script reports.
  ///
  /// @return The version; empty when none was set.
  auto takion_version() const noexcept -> std::optional<std::uint32_t> override;

  /// @brief Passes an event to the sink.
  ///
  /// @param reported The event.
  void report(net::protocol::remote_play::StreamEvent reported);
  /// @brief Moves to a state and reports the change.
  ///
  /// @param state The state.
  void become(net::protocol::remote_play::StreamState state);
  /// @brief Fails the link with a failure and reports it.
  ///
  /// @param detail The failure.
  void fail(LinkFailure detail);
  /// @brief Records a cleanup failure.
  ///
  /// @param detail The failure.
  void fail_cleanup(LinkFailure detail);
  /// @brief Records the console's quit and finishes.
  ///
  /// @param reason The quit reason.
  /// @param text Its text.
  void quit(net::protocol::remote_play::QuitReason reason,
            std::string_view text);
  /// @brief Moves to stopped and reports it, unless already ended.
  void finish();
  /// @brief Gives the share fresh receive keys at the shared Takion version;
  /// records "shared_stream", or "shared_stream_untapped" without a tap.
  void start_shared_stream();
  /// @brief Re-seals the recorded datagrams with the share's keys and passes
  /// them to the tap; records "shared_media", or "shared_media_unsealed" when
  /// anything is missing or fails.
  void send_shared_media();

  /// @brief The Takion version of the shared stream.
  static constexpr std::uint32_t shared_protocol_version = 12;
  /// @brief The frame rate of the shared stream.
  static constexpr std::uint32_t shared_frames_per_second = 60;
  /// @brief The width of the shared stream, in pixels.
  static constexpr std::uint16_t shared_width = 1280;
  /// @brief The height of the shared stream, in pixels.
  static constexpr std::uint16_t shared_height = 720;
  /// @brief The H.264 header of the shared stream, in hex.
  static constexpr std::string_view shared_video_header =
      "000000016764002aacd9405005bb0110000003001000000780f18319600000000168ebe3"
      "cb"
      "22c0";
  /// @brief The audio header of the shared stream, in hex.
  static constexpr std::string_view shared_audio_header =
      "02100000bb80000003c000000001";
  /// @brief The shared stream's video frames, in hex.
  static constexpr std::array<std::string_view, 4> shared_video_frames{
      "000000016588840001112233",
      "000000016588840002112233",
      "000000016588840003112233",
      "000000016588840004112233",
  };
  /// @brief The shared stream's audio frames, in hex.
  static constexpr std::array<std::string_view, 4> shared_audio_frames{
      "f8fffe01a0b1c2d3",
      "f8fffe02a0b1c2d3",
      "f8fffe03a0b1c2d3",
      "f8fffe04a0b1c2d3",
  };
  /// @brief Decodes hex.
  ///
  /// @param hex The text; an odd trailing digit is ignored.
  /// @return The bytes.
  static auto bytes(std::string_view hex) -> std::vector<std::byte>;

 private:
  /// @brief A datagram recorded from a real stream.
  struct Recorded {
    /// @brief Whether it carries video or audio.
    net::protocol::takion::SourceMedia media{
        net::protocol::takion::SourceMedia::video};
    /// @brief Its position in the key stream.
    std::uint64_t key_position{};
    /// @brief The datagram, in hex.
    std::string_view datagram;
  };

  /// @brief The receive key the recorded datagrams were sealed with, in hex.
  static constexpr std::string_view recorded_key =
      "202122232425262728292a2b2c2d2e2f";
  /// @brief The receive IV the recorded datagrams were sealed with, in hex.
  static constexpr std::string_view recorded_iv =
      "303132333435363738393a3b3c3d3e3f";
  /// @brief The recorded datagrams, in order.
  static constexpr std::array<Recorded, 9> recorded{
      Recorded{net::protocol::takion::SourceMedia::video, 44800ULL,
               "0200000001000000000607f4969c0000af000000008ed31add5370d0a428e08"
               "ee70af7"},
      Recorded{net::protocol::takion::SourceMedia::audio, 44928ULL,
               "03000000010000080105a5eb78400000af800000e4dd93ee79aff8ec"},
      Recorded{net::protocol::takion::SourceMedia::audio, 45056ULL,
               "0300010002000008010582fe9d6f0000b00000006a0c1788be4d46f0"},
      Recorded{net::protocol::takion::SourceMedia::video, 45184ULL,
               "02000100020000000006485f048e0000b080000000e4e6951e5a18caa3b131c"
               "d959d21"},
      Recorded{net::protocol::takion::SourceMedia::audio, 45312ULL,
               "030002000300000801058b1982740000b1000000464254c349d0252a"},
      Recorded{net::protocol::takion::SourceMedia::video, 45440ULL,
               "02000200030000040006ff87e2860000b1800000003b4c91be661d4330"},
      Recorded{net::protocol::takion::SourceMedia::video, 45568ULL,
               "02000300030020040006bf8158e20000b2000000007cd9813320eba2e7"},
      Recorded{net::protocol::takion::SourceMedia::audio, 45696ULL,
               "0300030004000008010599679de40000b2800000b759718096ec6815"},
      Recorded{net::protocol::takion::SourceMedia::video, 45824ULL,
               "0200040004000000000695238f900000b300000000803270453593416d87249"
               "a1c7070"},
  };

  /// @brief Reports whether the link is running.
  ///
  /// @return `true` from establishing the route until stopped or failed.
  auto running() const noexcept -> bool;
  /// @brief Records "rest".
  ///
  /// @param command The command.
  void note(const net::protocol::remote_play::command::GotoBed& command);
  /// @brief Records "go_home".
  ///
  /// @param command The command.
  void note(const net::protocol::remote_play::command::GoHome& command);
  /// @brief Records "keyboard_text".
  ///
  /// @param command The command.
  void note(
      const net::protocol::remote_play::command::KeyboardSetText& command);
  /// @brief Records "keyboard_accept".
  ///
  /// @param command The command.
  void note(const net::protocol::remote_play::command::KeyboardAccept& command);
  /// @brief Records "keyboard_reject".
  ///
  /// @param command The command.
  void note(const net::protocol::remote_play::command::KeyboardReject& command);
  /// @brief Records an account seat request and "seat_account".
  ///
  /// @param command The command.
  void note(
      const net::protocol::remote_play::command::CouchSetAccountId& command);
  /// @brief Records a join seat request, with its controller kind, and
  /// "seat_join".
  ///
  /// @param command The command.
  void note(const net::protocol::remote_play::command::CouchJoin& command);
  /// @brief Records a removal seat request and "seat_remove".
  ///
  /// @param command The command.
  void note(const net::protocol::remote_play::command::CouchRemove& command);
  /// @brief Records a passcode seat request and "seat_passcode".
  ///
  /// @param command The command.
  void note(const net::protocol::remote_play::command::CouchPasscode& command);
  /// @brief Records "command" for any other command.
  ///
  /// @tparam Other The command's type.
  template <class Other>
  void note(const Other&) {
    script_->note("command");
  }

  /// @brief The shared script.
  std::shared_ptr<Script> script_;
  /// @brief Stops the link.
  platform::StopSource stop_;
  /// @brief Where events go.
  LinkEventSink* sink_{};
  /// @brief The link's state.
  net::protocol::remote_play::StreamState state_{
      net::protocol::remote_play::StreamState::idle};
};

}  // namespace libchiaking::play::testing
