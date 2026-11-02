/// @file
/// @brief Declares HomePairing, which pairs the user with a console on the
/// local network by the PIN the console shows: it knocks, sends the
/// registration request and turns the console's reply into a PairedConsole.
///
/// The order of an attempt lives in home_pairing_lifecycle; HomePairing owns
/// the knock and the TCP exchange, carries out the machine's effects and
/// reports progress in the PairingStep vocabulary frontends see.
#pragma once

#include "libchiaking/detail/consoles/firmware.hpp"
#include "libchiaking/detail/consoles/pairing.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/detail/wire_sizes.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/consoles/adapters/pairing/pairing_exchange.hpp"
#include "src/core/consoles/adapters/pairing/pairing_knock.hpp"
#include "src/core/consoles/model/pairing/home_pairing_lifecycle.hpp"
#include "src/core/net/ports.hpp"
#include "src/core/net/protocol/remote_play/pairing/material.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"
#include "src/core/net/transport/http/sleep_waiter.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::platform {
class ClockProvider;
class CryptoProvider;
class SecureRandomProvider;
}  // namespace libchiaking::platform

namespace libchiaking::consoles {

/// @brief One attempt to pair with a console at home.
///
/// run() is the whole attempt in one blocking call. The lower-level
/// start(), pump_once(), wait() and cancel() let a caller drive the same
/// attempt from its own loop. Either way an object serves one attempt: once
/// it is paired, cancelled or failed it cannot be started again. The PIN and
/// key material live in wiping buffers and are wiped when the attempt ends.
///
/// Not safe to share between threads: one thread drives an attempt; only
/// wake() may be called from another thread. The providers must outlive it.
class HomePairing {
 public:
  /// @brief Where an attempt stands.
  using Step = home_pairing_lifecycle::Step;
  /// @brief Why the lower-level calls failed.
  using Error = home_pairing_lifecycle::Error;
  /// @brief The user's PSN account id as the eight bytes the request carries.
  using AccountBytes = SecretArray<psn_account_id_size>;

  /// @brief How long run() lets an attempt take by default.
  static constexpr std::chrono::milliseconds deadline{30'000};
  /// @brief The longest run() waits between two steps, so a stop request is
  /// noticed within about this long.
  static constexpr std::chrono::milliseconds wait_step{100};
  /// @brief Longest host text start_at() accepts, in bytes.
  static constexpr std::size_t host_limit = 255;

  /// @brief What the console said when it refused the request.
  struct Refusal {
    /// @brief The HTTP status of the refusal; empty until a refusal is
    /// read.
    std::optional<std::uint16_t> http_status{};
    /// @brief The RP-Application-Reason the console gave, such as a wrong
    /// PIN; empty when it gave none.
    std::optional<std::uint32_t> ps_reason{};
  };

  /// @brief Where to knock and connect.
  struct Door {
    /// @brief The console's address; must be specified and have a port.
    platform::SocketEndpoint destination{platform::IpAddress::any_ipv4(),
                                         rp_session_port};
    /// @brief Further addresses of the same host, tried when the one before
    /// cannot be opened.
    std::vector<platform::SocketEndpoint> fallbacks{};
    /// @brief Whether the knock is broadcast and the console's answer may
    /// come from any address.
    bool broadcast{};
  };

  /// @brief What the console is asked to register: the console variant, who
  /// the user is and the PIN.
  ///
  /// Move-only through the PIN it holds.
  class Request {
   public:
    /// @brief Builds the request for a user and the console's firmware.
    ///
    /// A PS4 below 7.0 is asked by the user's online id; every other
    /// console by the user's account id.
    ///
    /// @param as The user to pair.
    /// @param firmware The console's firmware, which picks the target.
    /// @param pin The PIN the console shows.
    /// @return The request.
    /// @retval PairingFailure::Reason::needs_online_id The console needs an
    ///     online id and the user has no PSN account.
    [[nodiscard]] static auto of(const users::PsUser& as, Firmware firmware,
                                 PairingPin pin)
        -> Expected<Request, PairingFailure>;
    /// @brief Builds a request from its parts, without checking them, for
    /// the internet start and tests.
    ///
    /// @param target The console variant.
    /// @param account The account id bytes, if the request carries them.
    /// @param online_id The online id, if the request carries it.
    /// @param pin The PIN.
    /// @param console_pin A PIN the console uses for later connections,
    ///     copied into the result; zero when there is none.
    /// @return The request.
    static auto with(net::protocol::remote_play::RegistrationTarget target,
                     std::optional<AccountBytes> account,
                     std::optional<std::string> online_id, PairingPin pin,
                     std::uint32_t console_pin = 0) -> Request;

    /// @brief The console variant the request is for.
    ///
    /// @return The registration target.
    auto target() const noexcept
        -> net::protocol::remote_play::RegistrationTarget {
      return target_;
    }

   private:
    /// @brief HomePairing reads the request's parts.
    friend class HomePairing;

    /// @brief Makes a request with no identity yet.
    ///
    /// @param target The console variant.
    /// @param pin The PIN.
    Request(net::protocol::remote_play::RegistrationTarget target,
            PairingPin pin) noexcept
        : target_(target), pin_(std::move(pin)) {}

    /// @brief The console variant.
    net::protocol::remote_play::RegistrationTarget target_;
    /// @brief The account id bytes; required for a PS4 on 8.0 or later and
    /// a PS5.
    std::optional<AccountBytes> account_;
    /// @brief The online id; used when there is no account id.
    std::optional<std::string> online_id_;
    /// @brief The PIN the console shows.
    PairingPin pin_;
    /// @brief The console PIN copied into the result; zero when none.
    std::uint32_t console_pin_{};
  };

  /// @brief Makes an idle attempt.
  ///
  /// @param network DNS, sockets and the poller the waits use; must outlive
  ///     this object.
  /// @param threading Makes the sleeper the waits use; must outlive this
  ///     object.
  /// @param crypto Encrypts the request and decrypts the reply; must outlive
  ///     this object.
  /// @param clock The monotonic clock deadlines and progress are measured
  ///     by; must outlive this object.
  /// @param random The source of the request's fresh key; must outlive this
  ///     object.
  /// @param log Where refusals are logged; null logs nothing. Must outlive
  ///     this object.
  HomePairing(platform::NetworkProvider& network,
              platform::ThreadingProvider& threading,
              platform::CryptoProvider& crypto,
              const platform::ClockProvider& clock,
              platform::SecureRandomProvider& random,
              platform::DiagnosticsProvider* log = nullptr);
  /// @brief Not copyable: it owns sockets and secrets.
  HomePairing(const HomePairing&) = delete;
  /// @brief Not copyable.
  auto operator=(const HomePairing&) -> HomePairing& = delete;
  /// @brief Closes the sockets and wipes the secrets.
  ~HomePairing();

  /// @brief Pairs with the console at `host`, blocking until the console
  /// accepts, refuses, the time runs out or `stop` fires.
  ///
  /// Reports to `watcher` every PairingStep reached, in order and each once,
  /// from reaching_console through waiting_for_answer, with the time since
  /// the call began. Blocks for up to `within`, waiting at most wait_step at
  /// a time on the socket of the current step.
  ///
  /// @param request What to register.
  /// @param host The console's address or host name; it is resolved and only
  ///     addresses of the same family as the text (IPv6 when it has a colon)
  ///     are tried.
  /// @param stop Cancels the attempt; noticed between steps.
  /// @param port The console's pairing port; zero means rp_session_port.
  /// @param within How long the attempt may take; must be positive.
  /// @param watcher Told of progress; may be null.
  /// @return The paired console, its firmware taken from the reply or else
  ///     the one asked for.
  /// @retval PairingFailure::Reason::cancelled `stop` fired.
  /// @retval PairingFailure::Reason::refused The console refused the request;
  ///     the failure carries its HTTP status and reason.
  /// @retval PairingFailure::Reason::timed_out The console did not answer in
  ///     time or `within` passed.
  /// @retval PairingFailure::Reason::malformed The request could not be built
  ///     or the reply was unusable.
  /// @retval PairingFailure::Reason::unreadable_reply A cryptographic step
  ///     failed.
  /// @retval PairingFailure::Reason::unreachable The host did not resolve or
  ///     the console could not be reached.
  /// @retval PairingFailure::Reason::busy The object was already used.
  [[nodiscard]] auto run(Request request, std::string_view host,
                         const platform::StopToken& stop,
                         std::uint16_t port = rp_session_port,
                         std::chrono::milliseconds within = deadline,
                         PairingWatcher* watcher = nullptr)
      -> Expected<PairedConsole, PairingFailure>;

  /// @brief Prepares the request and sends the knock.
  ///
  /// @param request What to register.
  /// @param door Where to knock and connect.
  /// @return Nothing once the knock is sent.
  /// @retval Error::invalid_state The object was already used.
  /// @retval Error::invalid_request The door's address is unusable, or the
  ///     PIN or identity could not be encoded.
  /// @retval Error::crypto_failure No random bytes, or encryption failed.
  /// @retval Error The knock failed; the attempt is now failed.
  [[nodiscard]] auto start(Request request, Door door) -> Expected<void, Error>;
  /// @brief Resolves a host and starts the attempt there; see start().
  ///
  /// Blocks while the host is resolved.
  ///
  /// @param request What to register.
  /// @param host The console's address or host name; at most host_limit
  ///     bytes, with no zero byte.
  /// @param port The pairing port; zero means rp_session_port.
  /// @param broadcast Whether the knock is broadcast.
  /// @return Nothing once the knock is sent.
  /// @retval Error::invalid_request The host is empty, too long or holds a
  ///     zero byte, or the request lacks the identity its target needs.
  /// @retval Error::invalid_state The object was already used.
  /// @retval Error::network_failure The host did not resolve to an address of
  ///     the right family.
  /// @retval Error Any failure of start().
  [[nodiscard]] auto start_at(Request request, std::string_view host,
                              std::uint16_t port = rp_session_port,
                              bool broadcast = false) -> Expected<void, Error>;
  /// @brief Advances the attempt by what the network allows now, without
  /// blocking.
  ///
  /// @return Nothing while the attempt goes on, or once it is paired.
  /// @retval Error::invalid_state The attempt was never started.
  /// @retval Error::cancelled The attempt was cancelled.
  /// @retval Error The attempt failed with this error.
  [[nodiscard]] auto pump_once() -> Expected<void, Error>;
  /// @brief Cancels the attempt and closes its sockets; does nothing once the
  /// attempt has ended.
  ///
  /// @return Nothing once cancelled or already ended.
  /// @retval Error::invalid_state The attempt was never started.
  /// @retval Error::cleanup_failure A socket did not close cleanly; the
  ///     attempt ends failed.
  [[nodiscard]] auto cancel() -> Expected<void, Error>;
  /// @brief Hands over the console's decoded reply, once.
  ///
  /// @return The registered console, with the request's console PIN.
  /// @retval Error::invalid_state The attempt is not paired, or the result
  ///     was already taken.
  [[nodiscard]] auto take_result()
      -> Expected<net::protocol::remote_play::RegisteredConsole, Error>;
  /// @brief Blocks until the current step's socket is ready, the settling
  /// pause ends, wake() is called or `timeout` passes.
  ///
  /// While a socket is awaited the wait is also capped at the protocol's 50
  /// ms timer tick, so deadlines are checked often.
  ///
  /// @param timeout The longest wait.
  void wait(std::chrono::milliseconds timeout);
  /// @brief Ends a wait() in progress early. Safe from any thread.
  void wake() noexcept;

  /// @brief Where the attempt stands.
  ///
  /// @return The current step.
  auto step() const noexcept -> Step { return lifecycle_.state(); }
  /// @brief Why the attempt ended, once it ended other than paired.
  ///
  /// @return The error, Error::cancelled for a cancelled attempt, or nothing.
  auto error() const noexcept -> std::optional<Error> { return last_error_; }
  /// @brief What the console said when it refused.
  ///
  /// @return The refusal; empty fields when the console did not refuse.
  auto refusal() const noexcept -> Refusal { return refusal_; }

  /// @brief Turns a console's decoded reply into a pairing the roster can
  /// keep.
  ///
  /// @param console The decoded reply.
  /// @param asked The firmware the request was made for; used when the reply
  ///     names an unknown target.
  /// @return The pairing, named by the reply's nickname with
  ///     ConsoleName::Source::pairing when it is usable.
  /// @retval PairingFailure::Reason::malformed The reply's MAC address or
  ///     keys are unusable.
  [[nodiscard]] static auto paired(
      net::protocol::remote_play::RegisteredConsole console, Firmware asked)
      -> Expected<PairedConsole, PairingFailure>;
  /// @brief Time between two monotonic clock readings.
  ///
  /// @param began_us The earlier reading, in microseconds.
  /// @param now_us The later reading, in microseconds.
  /// @return The time between them; zero when the clock went backwards.
  static auto elapsed_since(std::uint64_t began_us,
                            std::uint64_t now_us) noexcept
      -> std::chrono::microseconds;
  /// @brief The PairingStep a frontend is told for a step.
  ///
  /// @param step The step.
  /// @return reaching_console while knocking, settling or connecting;
  ///     sending_request while sending; waiting_for_answer while receiving;
  ///     nothing otherwise.
  static auto reported(Step step) noexcept -> std::optional<PairingStep>;

 private:
  /// @brief The attempt id every event carries; an object serves one attempt.
  static constexpr std::uint64_t attempt = 1;

  /// @brief The attempt error for a knock failure.
  ///
  /// @param error The knock's error.
  /// @return Error::timeout for a timeout, Error::network_failure otherwise.
  static auto stopped_by(PairingKnock::Error error) noexcept -> Error;
  /// @brief The attempt error for an exchange failure.
  ///
  /// @param error The exchange's error.
  /// @return Error::timeout, refused or crypto_failure for those errors;
  ///     Error::invalid_reply for a malformed, oversized or truncated reply;
  ///     Error::network_failure otherwise.
  static auto stopped_by(PairingExchange::Error error) noexcept -> Error;
  /// @brief The PairingFailure reason a frontend is told for an attempt
  /// error.
  ///
  /// @param error The attempt error.
  /// @return The matching reason; network and cleanup failures are
  ///     PairingFailure::Reason::unreachable.
  static auto reason_for(Error error) noexcept -> PairingFailure::Reason;
  /// @brief A short description of a console's RP-Application-Reason, for the
  /// log.
  ///
  /// @param reason The reason code.
  /// @return The description, or `unknown`.
  static auto ps_reason_text(std::uint32_t reason) noexcept -> std::string_view;
  /// @brief Whether a step ends the attempt.
  ///
  /// @param step The step.
  /// @return True for paired, cancelled and failed.
  static auto terminal(Step step) noexcept -> bool;

  /// @brief Derives fresh key material from random bytes and the PIN, then
  /// encodes and encrypts the request and stages it in the exchange.
  ///
  /// @param request What to register.
  /// @return Nothing once staged.
  /// @retval Error::invalid_request The target or door is unusable, or the
  ///     PIN or identity could not be encoded.
  /// @retval Error::crypto_failure No random bytes, or encryption failed.
  [[nodiscard]] auto prepare(const Request& request) -> Expected<void, Error>;
  /// @brief The result the lower-level calls report for the current step.
  ///
  /// @return Nothing unless the attempt failed or was cancelled.
  /// @retval Error The recorded error of a failed attempt.
  /// @retval Error::cancelled The attempt was cancelled.
  [[nodiscard]] auto outcome() const -> Expected<void, Error>;
  /// @brief The PairingFailure run() returns for an attempt error.
  ///
  /// @param error The attempt error.
  /// @param stop The attempt's stop token; when it fired, the failure is
  ///     cancelled whatever `error` says.
  /// @return The failure, carrying the refusal's status and reason.
  [[nodiscard]] auto failed(Error error, const platform::StopToken& stop) const
      -> Unexpected<PairingFailure>;
  /// @brief Wipes the staged request and the key material.
  void clear_prepared() noexcept;
  /// @brief Logs a line, when a log was given.
  ///
  /// @param severity The severity.
  /// @param message The line.
  void write(platform::LogSeverity severity, std::string_view message) noexcept;
  /// @brief Keeps the refusal's HTTP status and reason and logs them.
  void record_refusal();
  /// @brief Makes the exchange call the machine's current exchange part
  /// needs.
  ///
  /// @return What the exchange achieved.
  /// @retval PairingExchange::Error Any failure of that call.
  [[nodiscard]] auto exchange_once()
      -> Expected<PairingExchange::Progress, PairingExchange::Error>;
  /// @brief Feeds the machine the event an exchange result stands for,
  /// keeping a decoded reply as the result.
  ///
  /// @param progress What the exchange achieved.
  void follow(PairingExchange::Progress progress);
  /// @brief Fails the attempt with Error::timeout and runs the cleanup.
  void give_up();
  /// @brief Feeds the machine a failure.
  ///
  /// @param error What failed.
  void post_failure(Error error);
  /// @brief Carries out the machine's queued effects, oldest first, including
  /// any queued while doing so.
  void run_effects();
  /// @brief Sends the knock for the request's generation to the door.
  void handle(const home_pairing_lifecycle::BeginKnock&);
  /// @brief Starts the pause of search_delay (100 ms) before connecting.
  void handle(const home_pairing_lifecycle::WaitAfterKnock&);
  /// @brief Connects to the address that answered the knock, handing the key
  /// material to the exchange.
  void handle(const home_pairing_lifecycle::BeginConnect&);
  /// @brief Closes the knock and the exchange, wipes the prepared request and
  /// tells the machine whether both closed cleanly.
  void handle(const home_pairing_lifecycle::Cleanup&);
  /// @brief Nothing to do: the result was kept when the reply was decoded.
  void handle(const home_pairing_lifecycle::PublishPaired&);
  /// @brief Records Error::cancelled as the attempt's error.
  void handle(const home_pairing_lifecycle::PublishCancelled&);
  /// @brief Records the failure as the attempt's error.
  ///
  /// @param effect The effect, carrying the error.
  void handle(const home_pairing_lifecycle::PublishFailed& effect);

  /// @brief Resolves the host given to start_at().
  platform::DnsProvider& dns_;
  /// @brief Encrypts the request.
  platform::CryptoProvider& crypto_;
  /// @brief The monotonic clock deadlines and progress are measured by.
  const platform::ClockProvider& clock_;
  /// @brief The source of the request's fresh key.
  platform::SecureRandomProvider& random_;
  /// @brief Where refusals are logged; null when nothing is logged.
  platform::DiagnosticsProvider* log_;
  /// @brief What the console said when it refused.
  Refusal refusal_{};
  /// @brief The console variant of the request.
  net::protocol::remote_play::RegistrationTarget target_{
      net::protocol::remote_play::RegistrationTarget::ps5_1};
  /// @brief The request's console PIN, copied into the result.
  std::uint32_t console_pin_{};
  /// @brief Where to knock and connect.
  Door door_{};
  /// @brief The machine's queued effects and outcome.
  home_pairing_lifecycle::LifecycleContext context_;
  /// @brief The machine, deciding over #context_.
  home_pairing_lifecycle::Lifecycle lifecycle_;
  /// @brief The UDP knock.
  PairingKnock knock_;
  /// @brief The TCP exchange.
  PairingExchange exchange_;
  /// @brief The key material derived for the request, until the exchange
  /// takes it at connection.
  net::protocol::remote_play::RegistrationMaterial material_;
  /// @brief The address the console answered the knock from.
  platform::SocketEndpoint console_endpoint_{};
  /// @brief When the pause after the knock ends, on the monotonic clock.
  timing::Deadline settled_at_;
  /// @brief The decoded reply, until take_result() hands it over.
  std::optional<net::protocol::remote_play::RegisteredConsole> result_;
  /// @brief Why the attempt ended, once it ended other than paired.
  std::optional<Error> last_error_;
  /// @brief Waits on sockets and can be woken from another thread.
  net::http::SleepWaiter waiter_;
};

}  // namespace libchiaking::consoles
