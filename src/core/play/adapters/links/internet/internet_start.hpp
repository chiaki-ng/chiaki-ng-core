/// @file
/// @brief Declares InternetStart, the PSN half of reaching a console over the
/// internet: the account's Remote Play session, waking the console, and
/// hole-punching the control and data routes, with the machine that orders
/// the routes.
#pragma once

#include "libchiaking/detail/consoles/pairing.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/internet_route.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/platform/factory.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/deadline_stop_state.hpp"
#include "src/core/net/nat/holepunch/holepunch_candidate.hpp"
#include "src/core/net/nat/holepunch/holepunch_probe.hpp"
#include "src/core/net/nat/holepunch/limits.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"
#include "src/core/net/protocol/remote_play/stream/route.hpp"
#include "src/core/net/psn_signalling_limits.hpp"
#include "src/core/play/adapters/links/internet/route_failure.hpp"
#include "src/core/play/adapters/links/internet/wake_failure.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_accept.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_offer.hpp"
#include "src/core/play/adapters/links/internet/wire/psn_remote_registration.hpp"
#include "src/core/play/adapters/links/linked_stop.hpp"
#include "src/core/psn/remote_play/signalling_service.hpp"
#include "src/core/users/ports/play_calls.hpp"
#include "src/core/utils/diagnostics/trace.hpp"

#include <boost/sml.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::users {
class PsUser;
}

namespace libchiaking::play::internet_route_order {

/// @brief Effect: open the control route for an attempt.
struct StartControlRoute {
  /// @brief The attempt.
  std::uint64_t attempt{};
};
/// @brief Effect: open the data route for an attempt.
struct StartDataRoute {
  /// @brief The attempt.
  std::uint64_t attempt{};
};
/// @brief Effect: both routes are open for an attempt.
struct RoutesReady {
  /// @brief The attempt.
  std::uint64_t attempt{};
};
/// @brief What the route order asks of the start.
using Effect = std::variant<StartControlRoute, StartDataRoute, RoutesReady>;

/// @brief Where the route order stands.
enum class State : std::uint8_t {
  /// @brief No attempt began.
  idle,
  /// @brief Waiting for the console to be ready.
  waiting_start,
  /// @brief The control route is being opened.
  control_in_progress,
  /// @brief The data route is being opened.
  data_in_progress,
  /// @brief Both routes are open.
  complete,
  /// @brief The routes failed.
  failed,
  /// @brief The start was stopped.
  cancelled,
};

/// @brief Machine state: no attempt. The initial state.
struct Idle {};
/// @brief Machine state: waiting for the console.
struct Waiting {};
/// @brief Machine state: opening the control route.
struct Control {};
/// @brief Machine state: opening the data route.
struct Data {};
/// @brief Machine state: both routes open.
struct Complete {};
/// @brief Machine state: failed.
struct Failed {};
/// @brief Machine state: stopped.
struct Cancelled {};
/// @brief Event: an attempt begins.
struct Begin {
  /// @brief The attempt; 0 is refused.
  std::uint64_t attempt;
};
/// @brief Event: the console is ready.
struct Started {
  /// @brief The attempt.
  std::uint64_t attempt;
};
/// @brief Event: the control route is open.
struct ControlReady {
  /// @brief The attempt.
  std::uint64_t attempt;
};
/// @brief Event: the data route is open.
struct DataReady {
  /// @brief The attempt.
  std::uint64_t attempt;
};
/// @brief Event: a route failed.
struct Failure {
  /// @brief The attempt.
  std::uint64_t attempt;
};
/// @brief Event: the start was stopped.
struct Cancel {
  /// @brief The attempt.
  std::uint64_t attempt;
};
/// @brief What the machine's actions write.
struct Context {
  /// @brief The current attempt.
  std::uint64_t attempt{};
  /// @brief Where the order stands.
  State state{State::idle};
  /// @brief Effects not yet taken.
  std::vector<Effect> effects;
};
/// @brief The transition table of the route order: control, then data, then
/// ready, for the current attempt only; a failure or stop while waiting or
/// opening clears the effects. Stateless.
struct Machine {
  /// @brief Builds the transition table for Boost.SML.
  ///
  /// @return The table.
  auto operator()() const {
    using namespace boost::sml;
    const auto valid = [](const auto& event, const Context& ctx) {
      return event.attempt == ctx.attempt;
    };
    const auto begin = [](const Begin& event, Context& ctx) {
      ctx.attempt = event.attempt;
      ctx.state = State::waiting_start;
    };
    const auto control = [](Context& ctx) {
      ctx.effects.emplace_back(StartControlRoute{ctx.attempt});
      ctx.state = State::control_in_progress;
    };
    const auto data = [](Context& ctx) {
      ctx.effects.emplace_back(StartDataRoute{ctx.attempt});
      ctx.state = State::data_in_progress;
    };
    const auto ready = [](Context& ctx) {
      ctx.effects.emplace_back(RoutesReady{ctx.attempt});
      ctx.state = State::complete;
    };
    const auto fail = [](Context& ctx) {
      ctx.effects.clear();
      ctx.state = State::failed;
    };
    const auto cancel = [](Context& ctx) {
      ctx.effects.clear();
      ctx.state = State::cancelled;
    };
    return make_transition_table(
        *state<Idle> + boost::sml::event<Begin>[(
                           [](const Begin& e) { return e.attempt != 0; })] /
                           begin = state<Waiting>,
        state<Waiting> + boost::sml::event<Started>[valid] / control =
            state<Control>,
        state<Control> + boost::sml::event<ControlReady>[valid] / data =
            state<Data>,
        state<Data> + boost::sml::event<DataReady>[valid] / ready =
            state<Complete>,
        state<Waiting> + boost::sml::event<Failure>[valid] / fail =
            state<Failed>,
        state<Control> + boost::sml::event<Failure>[valid] / fail =
            state<Failed>,
        state<Data> + boost::sml::event<Failure>[valid] / fail = state<Failed>,
        state<Waiting> + boost::sml::event<Cancel>[valid] / cancel =
            state<Cancelled>,
        state<Control> + boost::sml::event<Cancel>[valid] / cancel =
            state<Cancelled>,
        state<Data> + boost::sml::event<Cancel>[valid] / cancel =
            state<Cancelled>);
  }
};

}  // namespace libchiaking::play::internet_route_order

namespace libchiaking::play {

/// @brief One internet start's PSN side: everything between the user's
/// Remote Play access and two hole-punched routes to the console.
///
/// The internet link calls it in order: begin, find the gateway, draw
/// secrets, take access, open the account session, listen for PSN
/// notifications, prepare the control route, wake the console, open the
/// control route, optionally pair over it, then open the data route on a
/// worker thread and collect it. Every step blocks on PSN or the network up
/// to the per-wait deadline. Leaving releases the PSN session, closes the
/// push connection and the gateway. Used on the link's thread, except the
/// data route work, which runs on its own thread and reaches the start's
/// clients. The platform and trace must outlive it. Not copyable.
class InternetStart {
 public:
  /// @brief A deadline for one wait.
  using Deadline = net::RemoteWaitDeadline;

  /// @brief The only attempt a start makes.
  static constexpr std::uint64_t this_attempt = 1;

  /// @brief The random values a start identifies itself with to the console.
  struct Secrets {
    /// @brief The local session id offered in hole-punch messages.
    std::uint16_t local_sid{};
    /// @brief The hashed id offered in hole-punch messages.
    SecretArray<net::holepunch_hashed_id_size> local_hashed_id;
    /// @brief The first custom data blob sent with the wake.
    SecretArray<psn_custom_data_size> data1;
    /// @brief The second custom data blob sent with the wake.
    SecretArray<psn_custom_data_size> data2;
  };

  /// @enum SecretsError
  /// @brief Why the secrets could not be drawn.
  ///
  /// @var SecretsError::random_failure
  /// @brief The random provider failed.
  enum class SecretsError : std::uint8_t { random_failure };

  /// @brief The account's open PSN session and the ready console.
  struct AccountSession {
    /// @brief The PSN session and its push connection.
    psn::remote_play::StartedSession opened;
    /// @brief What the console said once ready.
    psn::remote_play::ConsoleReady ready;
  };

  /// @brief Local sockets, a poller and an offer, prepared before the console
  /// is reached.
  struct PreparedRoute {
    /// @brief The bound local sockets.
    net::nat::holepunch::BoundSockets sockets;
    /// @brief Polls the sockets.
    std::unique_ptr<platform::Poller> poller;
    /// @brief The hole-punch offer.
    play::internet::holepunch::PreparedOffer offer;
  };

  /// @brief The order of the routes for one start.
  ///
  /// Must be used on the thread that built it; checked in debug builds.
  class RouteOrder {
   public:
    /// @brief Open the control route.
    using StartControlRoute = internet_route_order::StartControlRoute;
    /// @brief Open the data route.
    using StartDataRoute = internet_route_order::StartDataRoute;
    /// @brief Both routes are open.
    using RoutesReady = internet_route_order::RoutesReady;
    /// @brief What the order asks.
    using Effect = internet_route_order::Effect;
    /// @brief Where the order stands.
    using State = internet_route_order::State;

    /// @brief Begins an attempt.
    ///
    /// @param attempt The attempt; not 0.
    /// @return `true` when it began.
    auto begin(std::uint64_t attempt) -> bool;
    /// @brief Records that the console is ready, asking for the control route.
    ///
    /// @param attempt The attempt.
    void on_start_ready(std::uint64_t attempt);
    /// @brief Records that the control route is open, asking for the data
    /// route.
    ///
    /// @param attempt The attempt.
    void on_control_established(std::uint64_t attempt);
    /// @brief Records that the data route is open.
    ///
    /// @param attempt The attempt.
    void on_data_established(std::uint64_t attempt);
    /// @brief Records that a route failed.
    ///
    /// @param attempt The attempt.
    /// @return `true` when the order moved to failed.
    auto fail(std::uint64_t attempt) -> bool;
    /// @brief Records a timeout, as a failure.
    ///
    /// @param attempt The attempt.
    /// @return As RouteOrder::fail.
    auto timeout(std::uint64_t attempt) -> bool;
    /// @brief Records that the start was stopped.
    ///
    /// @param attempt The attempt.
    /// @return `true` when the order moved to cancelled.
    auto cancel(std::uint64_t attempt) -> bool;
    /// @brief Takes the effects asked for.
    ///
    /// @return The effects, oldest first.
    auto take_effects() -> std::vector<Effect>;
    /// @brief Reads where the order stands.
    ///
    /// @return The state.
    auto state() const noexcept -> State;

    /// @brief Takes the effects and checks the order asks for exactly one
    /// thing.
    ///
    /// @tparam Wanted The effect expected.
    /// @param attempt The attempt.
    /// @return `true` when the only effect is a @p Wanted for @p attempt.
    template <class Wanted>
    auto next_is(std::uint64_t attempt) -> bool {
      const auto effects = take_effects();
      return effects.size() == 1 &&
             std::holds_alternative<Wanted>(effects.front()) &&
             std::get<Wanted>(effects.front()).attempt == attempt;
    }

   private:
    /// @brief Asserts the caller is the owning thread.
    void check_owner() const {
      assert(owner_ == platform::ThreadIdentity::current());
    }

    /// @brief The thread that built the order.
    [[maybe_unused]] const platform::ThreadIdentity owner_{
        platform::ThreadIdentity::current()};
    /// @brief What the machine's actions write.
    internet_route_order::Context context_;
    /// @brief The Boost.SML machine.
    boost::sml::sm<internet_route_order::Machine> machine_{context_};
  };

  /// @brief The data route opened on a worker thread while the control route
  /// runs.
  ///
  /// The work gets a stop that follows the start's and can be stopped on its
  /// own. The result is read once the worker finished. Destroying the task
  /// stops and joins the worker and closes an untaken route. Not copyable.
  class DataRouteTask {
   public:
    /// @brief The data route, or why it failed.
    using Result =
        Expected<net::protocol::remote_play::RouteCandidate, RouteFailure>;
    /// @brief Opens the data route; given the task's stop.
    using Work = std::function<Result(platform::StopToken)>;

    /// @brief A finished data route.
    struct Completion {
      /// @brief The attempt it belongs to.
      std::uint64_t attempt_id;
      /// @brief The route, or why it failed.
      Result result;
      /// @brief Set when the work threw.
      std::optional<platform::PlatformError> worker_error;
    };

    /// @brief Builds the task.
    ///
    /// @param attempt_id The attempt.
    /// @param parent The start's stop.
    /// @param work Opens the route.
    DataRouteTask(std::uint64_t attempt_id, platform::StopToken parent,
                  Work work);
    /// @brief Not copyable; the task owns its worker.
    DataRouteTask(const DataRouteTask&) = delete;
    /// @brief Not copy-assignable; the task owns its worker.
    auto operator=(const DataRouteTask&) -> DataRouteTask& = delete;
    /// @brief Stops and joins the worker and closes an untaken route.
    ~DataRouteTask();

    /// @brief Starts the worker.
    ///
    /// @param threading Starts the thread.
    /// @return Nothing; PlatformError::thread_failure when already started or
    ///     done; the threading provider's error.
    [[nodiscard]] auto start(platform::ThreadingProvider& threading)
        -> Expected<void, platform::PlatformError>;
    /// @brief Takes the result once the worker finished, joining it.
    ///
    /// @return The completion; empty while the worker runs.
    auto take_completion() -> std::optional<Completion>;
    /// @brief Asks the work to stop.
    void request_stop();
    /// @brief Stops the work and joins the worker; blocks until it returns.
    void cancel_and_join();

   private:
    /// @brief The worker's body.
    ///
    /// @param context The task.
    static void run(void* context) noexcept;

    /// @brief The attempt.
    std::uint64_t attempt_id_;
    /// @brief The work's stop.
    std::shared_ptr<LinkedStop> stop_;
    /// @brief Opens the route.
    Work work_;
    /// @brief The worker.
    std::unique_ptr<platform::native::Thread> thread_;
    /// @brief The result; set by the worker.
    std::optional<Result> result_;
    /// @brief Set when the work threw.
    std::optional<platform::PlatformError> worker_error_;
    /// @brief Set by the worker once the result is in.
    std::atomic<bool> done_{};
  };

  /// @brief Told when the start pairs with the console.
  class PairingListener {
   public:
    /// @brief Why the listener turned a pairing away.
    enum class Refusal : std::uint8_t {
      /// @brief The console reached is not one the player has paired.
      unpaired_console,
    };

    /// @brief Allows destruction through the interface.
    virtual ~PairingListener() = default;
    /// @brief Takes the new pairing, or turns it away.
    ///
    /// @param paired The pairing.
    /// @return Nothing when the pairing was taken; the refusal that ends the
    ///     start otherwise.
    [[nodiscard]] virtual auto on_paired(consoles::PairedConsole paired)
        -> Expected<void, Refusal> = 0;
  };

  /// @brief Told each step of opening a route.
  class RouteWatcher {
   public:
    /// @brief Allows destruction through the interface.
    virtual ~RouteWatcher() = default;
    /// @brief Takes a route step.
    ///
    /// @param route The step.
    virtual void on_route(const progress::InternetRoute& route) = 0;
  };

  /// @brief Holds the data route's steps, told on the worker thread, until the
  /// link's thread passes them on. Guarded by a mutex.
  class HeldRoutes final : public RouteWatcher {
   public:
    /// @brief Holds nothing yet.
    ///
    /// @param threading Provider of the mutex guarding the held steps; must
    ///     outlive the holder.
    explicit HeldRoutes(platform::ThreadingProvider& threading)
        : mutex_(threading) {}

    /// @brief Holds a step.
    ///
    /// @param route The step.
    void on_route(const progress::InternetRoute& route) override;
    /// @brief Passes the held steps on, in order, and forgets them.
    ///
    /// @param watcher Told the steps; null drops them.
    void release_to(RouteWatcher* watcher);

   private:
    /// @brief Guards the held steps.
    platform::Mutex mutex_;
    /// @brief The held steps.
    std::vector<progress::InternetRoute> held_;
  };

  /// @brief How the start reaches the console. Plain data.
  struct Reach {
    /// @brief Whether IPv6 candidates may be offered; used for a PS5 only.
    bool ipv6{};
    /// @brief Whether NAT ports are guessed whatever the NAT.
    bool force_port_guessing{};
    /// @brief How many NAT ports are guessed.
    std::size_t port_guesses{net::nat::holepunch::default_port_guess_count};
    /// @brief How many sockets probe the NAT.
    std::size_t nat_probe_sockets{
        net::nat::holepunch::default_nat_probe_socket_count};
    /// @brief How long each wait may take, in microseconds.
    std::uint64_t wait_microseconds{
        net::nat::holepunch::default_remote_start_timeout_us};
  };

  /// @brief How leaving went.
  struct Left {
    /// @brief The push connection's close error, when it failed.
    std::optional<platform::WebSocketError> push_close_failure{};
    /// @brief Whether the gateway mapping was released.
    bool gateway_released{true};
  };

  /// @brief Draws the start's random values.
  ///
  /// @param random The random provider.
  /// @return The secrets; SecretsError::random_failure.
  [[nodiscard]] static auto generate_secrets(
      platform::SecureRandomProvider& random)
      -> Expected<Secrets, SecretsError>;

  /// @brief Asks PSN to start a PS5 and waits until it is ready.
  ///
  /// Blocks on PSN and the push connection until the console is ready or the
  /// deadline passes.
  ///
  /// @param opened The account's PSN session.
  /// @param session_client Sends PSN requests.
  /// @param push_client Receives PSN notifications.
  /// @param device_uid The console's PSN device id.
  /// @param data1 The first custom data blob.
  /// @param data2 The second custom data blob.
  /// @param clock The monotonic clock.
  /// @param parent_stop The start's stop.
  /// @param deadline_microseconds The deadline.
  /// @return What the console said; a refusal when the session is not open,
  ///     the start was stopped or the deadline passed; the PSN failure.
  [[nodiscard]] static auto wake_ps5(
      psn::remote_play::StartedSession& opened,
      psn::remote_play::SessionClient& session_client,
      psn::remote_play::PushClient& push_client,
      std::span<const std::byte, psn_device_uid_size> device_uid,
      std::span<const std::byte, psn_custom_data_size> data1,
      std::span<const std::byte, psn_custom_data_size> data2,
      const platform::ClockProvider& clock, platform::StopToken parent_stop,
      Deadline deadline_microseconds)
      -> Expected<psn::remote_play::ConsoleReady, WakeFailure>;

  /// @brief Asks PSN to wake a PS4 and waits until it is ready.
  ///
  /// Blocks like InternetStart::wake_ps5.
  ///
  /// @param opened The account's PSN session.
  /// @param session_client Sends PSN requests.
  /// @param push_client Receives PSN notifications.
  /// @param data1 The first custom data blob.
  /// @param data2 The second custom data blob.
  /// @param clock The monotonic clock.
  /// @param parent_stop The start's stop.
  /// @param deadline_microseconds The deadline.
  /// @return As InternetStart::wake_ps5.
  [[nodiscard]] static auto wake_ps4(
      psn::remote_play::StartedSession& opened,
      psn::remote_play::SessionClient& session_client,
      psn::remote_play::PushClient& push_client,
      std::span<const std::byte, psn_custom_data_size> data1,
      std::span<const std::byte, psn_custom_data_size> data2,
      const platform::ClockProvider& clock, platform::StopToken parent_stop,
      Deadline deadline_microseconds)
      -> Expected<psn::remote_play::ConsoleReady, WakeFailure>;

  /// @brief Binds local sockets, gathers candidates and prepares the control
  /// route's offer; blocks on STUN and the gateway up to the deadline.
  ///
  /// @param platform The host's providers.
  /// @param secrets The start's random values.
  /// @param parent_stop The start's stop.
  /// @param deadline_microseconds The deadline.
  /// @param enable_ipv6 Whether to bind IPv6 too.
  /// @param force_port_guessing Whether to guess ports whatever the NAT.
  /// @param guess_count How many ports to guess.
  /// @param gateway The gateway session; may be null.
  /// @param stun What STUN discovered so far; may be null.
  /// @return The prepared route; the route failure.
  [[nodiscard]] static auto prepare_control_route(
      platform::PlatformFactory& platform, const Secrets& secrets,
      platform::StopToken parent_stop, Deadline deadline_microseconds,
      bool enable_ipv6, bool force_port_guessing = false,
      std::size_t guess_count = net::nat::holepunch::default_port_guess_count,
      platform::GatewaySession* gateway = nullptr,
      net::nat::stun::StunDiscoveryState* stun = nullptr)
      -> Expected<PreparedRoute, RouteFailure>;

  /// @brief Opens the control route: offers candidates through PSN, probes
  /// the console's, and exchanges ACCEPT. Blocks up to the deadline.
  ///
  /// @param platform The host's providers.
  /// @param session_client Sends PSN requests.
  /// @param push_client Receives PSN notifications.
  /// @param started The account's session and ready console.
  /// @param secrets The start's random values.
  /// @param offer_req_id The offer's request id; 1 to 65535.
  /// @param accept_req_id The accept's request id; 1 to 65535, not the
  ///     offer's.
  /// @param parent_stop The start's stop.
  /// @param deadline_microseconds The deadline.
  /// @param enable_ipv6 Whether to bind IPv6 too.
  /// @param family PS4 or PS5.
  /// @param preparation The prepared route; prepared here when empty.
  /// @param nat_socket_count How many sockets probe the NAT.
  /// @param routes Told the steps; may be null.
  /// @return The control socket handoff with the console's session id;
  ///     RouteFailure::InvalidState when the exchange cannot run; the route
  ///     failure.
  [[nodiscard]] static auto establish_control_route(
      platform::PlatformFactory& platform,
      psn::remote_play::SessionClient& session_client,
      psn::remote_play::PushClient& push_client, AccountSession& started,
      const Secrets& secrets, std::uint64_t offer_req_id,
      std::uint64_t accept_req_id, platform::StopToken parent_stop,
      Deadline deadline_microseconds, bool enable_ipv6,
      psn::rest::ConsoleFamily family = psn::rest::ConsoleFamily::ps5,
      std::optional<PreparedRoute> preparation = std::nullopt,
      std::size_t nat_socket_count =
          net::nat::holepunch::default_nat_probe_socket_count,
      RouteWatcher* routes = nullptr)
      -> Expected<play::internet::holepunch::CtrlRudpHandoff, RouteFailure>;

  /// @brief Turns an accepted data socket into the stream route.
  ///
  /// @param network The network provider that owns the socket.
  /// @param socket The socket.
  /// @param selected_peer The console's endpoint.
  /// @return The route; the adoption failure.
  [[nodiscard]] static auto adopt_data_route(
      platform::NetworkProvider& network,
      std::unique_ptr<platform::UdpSocket> socket,
      platform::SocketEndpoint selected_peer)
      -> Expected<net::protocol::remote_play::RouteCandidate,
                  RouteFailure::Adoption>;

  /// @brief Hands the data socket over once ACCEPT completed.
  ///
  /// @param network The network provider.
  /// @param selected The selected candidate and its socket.
  /// @param accept_wait The ACCEPT wait.
  /// @return The route; RouteFailure::AcceptNotReady; the adoption failure.
  [[nodiscard]] static auto hand_off_data_route(
      platform::NetworkProvider& network,
      net::nat::holepunch::SelectedCandidate selected,
      const play::internet::holepunch::AcceptWait& accept_wait)
      -> Expected<net::protocol::remote_play::RouteCandidate, RouteFailure>;

  /// @brief Exchanges ACCEPT for the selected data candidate and hands its
  /// socket over; blocks up to the deadline.
  ///
  /// @param network The network provider.
  /// @param ours Our offer.
  /// @param console The console's offer.
  /// @param selected The selected candidate.
  /// @param attempt The attempt.
  /// @param local_req_id The accept's request id.
  /// @param session Sends PSN requests.
  /// @param push Receives PSN notifications.
  /// @param connection The push connection.
  /// @param bearer_token The PSN session's token.
  /// @param session_id The PSN session id.
  /// @param account_id The account id.
  /// @param console_uid The console's PSN device id.
  /// @param family PS4 or PS5.
  /// @param parent_stop The start's stop.
  /// @param clock The monotonic clock.
  /// @param deadline_microseconds The deadline.
  /// @param trace Records the steps; may be null.
  /// @return The route; the exchange's or handoff's failure.
  [[nodiscard]] static auto accept_data_route(
      platform::NetworkProvider& network,
      const play::internet::holepunch::PreparedOffer& ours,
      const net::nat::holepunch::ConnRequest& console,
      net::nat::holepunch::SelectedCandidate selected, std::uint64_t attempt,
      std::uint64_t local_req_id, psn::remote_play::SessionClient& session,
      psn::remote_play::PushClient& push,
      platform::WebSocketConnection& connection,
      const psn::auth::BearerToken& bearer_token, std::string_view session_id,
      std::int64_t account_id,
      std::span<const std::byte, psn_device_uid_size> console_uid,
      psn::rest::ConsoleFamily family, platform::StopToken parent_stop,
      const platform::ClockProvider& clock, Deadline deadline_microseconds,
      const diagnostics::Trace* trace = nullptr)
      -> Expected<net::protocol::remote_play::RouteCandidate, RouteFailure>;

  /// @brief Opens the data route: prepares sockets and an offer, meets the
  /// console through PSN and exchanges ACCEPT. Blocks up to the deadline; run
  /// on the data route worker.
  ///
  /// @param platform The host's providers.
  /// @param session_client Sends PSN requests.
  /// @param push_client Receives PSN notifications.
  /// @param started The account's session and ready console.
  /// @param secrets The start's random values.
  /// @param offer_req_id The offer's request id.
  /// @param accept_req_id The accept's request id.
  /// @param parent_stop The work's stop.
  /// @param deadline_microseconds The deadline.
  /// @param enable_ipv6 Whether to bind IPv6 too.
  /// @param family PS4 or PS5.
  /// @param force_port_guessing Whether to guess ports whatever the NAT.
  /// @param guess_count How many ports to guess.
  /// @param nat_socket_count How many sockets probe the NAT.
  /// @param gateway The gateway session; may be null.
  /// @param stun What STUN discovered so far; may be null.
  /// @param ctrl_console_sid The console's session id from the control
  ///     route.
  /// @param routes Told the steps; may be null.
  /// @return The route; RouteFailure::InvalidState when the exchange cannot
  ///     run; the route failure.
  [[nodiscard]] static auto establish_data_route(
      platform::PlatformFactory& platform,
      psn::remote_play::SessionClient& session_client,
      psn::remote_play::PushClient& push_client, AccountSession& started,
      const Secrets& secrets, std::uint64_t offer_req_id,
      std::uint64_t accept_req_id, platform::StopToken parent_stop,
      Deadline deadline_microseconds, bool enable_ipv6,
      psn::rest::ConsoleFamily family = psn::rest::ConsoleFamily::ps5,
      bool force_port_guessing = false,
      std::size_t guess_count = net::nat::holepunch::default_port_guess_count,
      std::size_t nat_socket_count =
          net::nat::holepunch::default_nat_probe_socket_count,
      platform::GatewaySession* gateway = nullptr,
      net::nat::stun::StunDiscoveryState* stun = nullptr,
      std::uint32_t ctrl_console_sid = 0, RouteWatcher* routes = nullptr)
      -> Expected<net::protocol::remote_play::RouteCandidate, RouteFailure>;

  /// @brief Counts the offered candidates for progress.
  ///
  /// @param offered The candidates.
  /// @return Local, public and port-guessed counts; with more than one mapped
  ///     candidate the mapped ones count as guessed.
  static auto candidates_of(
      const std::vector<net::nat::holepunch::Candidate>& offered) noexcept
      -> progress::InternetRoute::Candidates;
  /// @brief Names the kind of route chosen.
  ///
  /// @param console The console's offer.
  /// @param selected The selected candidate.
  /// @return Local, public address, port guessed or discovered.
  static auto kind_of(
      const net::nat::holepunch::ConnRequest& console,
      const net::nat::holepunch::SelectedCandidate& selected) noexcept
      -> progress::InternetRoute::Kind;

  /// @brief Turns the console's registration into a pairing.
  ///
  /// @param console The registered console.
  /// @param asked The firmware target registered with.
  /// @return The pairing; empty for an unknown target or an invalid
  ///     registration.
  [[nodiscard]] static auto paired_console(
      const net::protocol::remote_play::RegisteredConsole& console,
      net::protocol::remote_play::RegistrationTarget asked)
      -> std::optional<consoles::PairedConsole>;

  /// @brief Builds the start.
  ///
  /// @param platform The host's providers; must outlive the start and offer
  ///     HTTP and websocket providers.
  /// @param family PS4 or PS5.
  /// @param reach How the start reaches the console.
  /// @param stop The start's stop.
  /// @param trace Records the steps; must outlive the start.
  /// @param pairings Told a new pairing; may be null.
  /// @param routes Told the route steps; may be null.
  InternetStart(platform::PlatformFactory& platform,
                psn::rest::ConsoleFamily family, Reach reach,
                platform::StopToken stop, const diagnostics::Trace& trace,
                PairingListener* pairings = nullptr,
                RouteWatcher* routes = nullptr);
  /// @brief Not copyable; the start owns PSN state.
  InternetStart(const InternetStart&) = delete;
  /// @brief Not copy-assignable; the start owns PSN state.
  auto operator=(const InternetStart&) -> InternetStart& = delete;
  /// @brief Stops signalling and leaves; blocks while the PSN session is
  /// released.
  ~InternetStart();

  /// @brief Begins the route order.
  ///
  /// @return `true` when it began.
  auto begin() -> bool;
  /// @brief Looks for a gateway that can map ports; blocks on discovery.
  ///
  /// @return `true` when the platform has a gateway provider, whether or not
  ///     one was found.
  auto find_gateway() -> bool;
  /// @brief Draws the start's random values.
  ///
  /// @return Nothing; SecretsError::random_failure.
  [[nodiscard]] auto draw_secrets() -> Expected<void, SecretsError>;
  /// @brief Takes the user's Remote Play access token; may block on a token
  /// refresh.
  ///
  /// @param user The user.
  /// @return Nothing; a cancelled start error; an invalid token error when
  ///     access is refused.
  [[nodiscard]] auto take_access(users::PsUser& user)
      -> Expected<void, psn::remote_play::StartError>;
  /// @brief Opens the account's PSN Remote Play session and push connection;
  /// blocks on PSN.
  ///
  /// An unauthorised reply marks the access as refused by PS.
  ///
  /// @return Nothing; an invalid token error without access; the open's
  ///     error.
  [[nodiscard]] auto open_account_session()
      -> Expected<void, psn::remote_play::StartError>;
  /// @brief Starts servicing PSN notifications on a thread.
  ///
  /// @return Nothing; the threading provider's error.
  [[nodiscard]] auto listen() -> Expected<void, platform::PlatformError>;
  /// @brief Prepares the control route before waking the console.
  ///
  /// @return Nothing; the route failure.
  [[nodiscard]] auto prepare_control_route() -> Expected<void, RouteFailure>;
  /// @brief Wakes the console and waits until it is ready.
  ///
  /// @param device_uid The console's PSN device id.
  /// @return Nothing; the wake failure.
  [[nodiscard]] auto wake(
      std::span<const std::byte, psn_device_uid_size> device_uid)
      -> Expected<void, WakeFailure>;
  /// @brief Reports whether the order now asks for the control route.
  ///
  /// @return `true` when it does.
  auto control_route_is_next() -> bool;
  /// @brief Opens the control route with the prepared offer.
  ///
  /// @return The control socket handoff; the route failure.
  [[nodiscard]] auto control_route()
      -> Expected<play::internet::holepunch::CtrlRudpHandoff, RouteFailure>;
  /// @brief Registers the client with the console over the control route;
  /// blocks on the exchange.
  ///
  /// @param channel The RUDP handshake channel.
  /// @param crypto The crypto provider.
  /// @param clock The monotonic clock.
  /// @param target The firmware target to register with.
  /// @param account_id The account id.
  /// @param local_address The local address sent to the console.
  /// @return The registration; the registration failure.
  [[nodiscard]] auto pair(net::transport::rudp::HandshakeChannel& channel,
                          platform::CryptoProvider& crypto,
                          platform::ClockProvider& clock,
                          net::protocol::remote_play::RegistrationTarget target,
                          std::span<const std::byte> account_id,
                          std::string_view local_address)
      -> Expected<play::internet::PsnRemoteRegistrationResult,
                  play::internet::PsnRemoteRegistrationFailure>;
  /// @brief Tells the listener the pairing a registration granted.
  ///
  /// @param console The registered console.
  /// @param asked The firmware target registered with.
  /// @return Nothing when there is no listener, the registration is not a
  ///     valid pairing, or the listener took it; the listener's refusal
  ///     otherwise.
  [[nodiscard]] auto keep(
      const net::protocol::remote_play::RegisteredConsole& console,
      net::protocol::remote_play::RegistrationTarget asked)
      -> Expected<void, PairingListener::Refusal>;
  /// @brief Records the control route open and reports whether the data route
  /// is asked for.
  ///
  /// @return `true` when it is.
  auto data_route_is_next() -> bool;
  /// @brief Builds the data route task.
  void plan_data_route();
  /// @brief Drops the data route task, stopping and joining it.
  void drop_data_route() noexcept;
  /// @brief Starts the data route worker.
  ///
  /// @return Nothing; the threading error.
  [[nodiscard]] auto launch_data_route()
      -> Expected<void, platform::PlatformError>;
  /// @brief Reports whether a data route task exists.
  ///
  /// @return `true` until it is taken or dropped.
  auto data_route_is_planned() const noexcept -> bool;
  /// @brief Takes the data route once done and passes on its held steps.
  ///
  /// @return The completion; empty while it runs.
  auto take_data_route() -> std::optional<DataRouteTask::Completion>;
  /// @brief Records the data route open and reports whether both routes are
  /// ready, moving PSN servicing to the active phase.
  ///
  /// @param completed_attempt The attempt the route belongs to.
  /// @return `true` when both are ready.
  auto routes_are_ready(std::uint64_t completed_attempt) -> bool;
  /// @brief Records that a route failed.
  void route_failed();
  /// @brief Reads the notification service's failure.
  ///
  /// @return The failure; empty while it runs.
  auto signalling_failure() const -> std::optional<psn::remote_play::PushError>;
  /// @brief Reports whether the PSN session is open.
  ///
  /// @return `true` once opened, until left.
  auto is_open() const noexcept -> bool;
  /// @brief Reports whether the secrets were drawn.
  ///
  /// @return `true` once drawn, until left.
  auto has_secrets() const noexcept -> bool;
  /// @brief Reports whether IPv6 is used.
  ///
  /// @return `true` when asked for and the console is a PS5.
  auto ipv6_enabled() const noexcept -> bool;
  /// @brief Stops the order, the data route and PSN servicing.
  ///
  /// @param close_push_first Whether to close the push connection now, as
  ///     for host sleep, which skips releasing the PSN session.
  /// @return The close error, when it failed.
  [[nodiscard]] auto stop_signalling(bool close_push_first)
      -> std::optional<platform::WebSocketError>;
  /// @brief Releases the PSN session and waits for its deletion, closes the
  /// push connection and releases the gateway; blocks on PSN for up to the
  /// deletion timeout.
  ///
  /// @return How it went.
  [[nodiscard]] auto leave() -> Left;

 private:
  /// @brief How candidates are gathered.
  struct Gathering {
    /// @brief Whether ports are guessed whatever the NAT.
    bool force_port_guessing{};
    /// @brief How many ports are guessed.
    std::size_t guess_count{net::nat::holepunch::default_port_guess_count};
    /// @brief The gateway session; may be null.
    platform::GatewaySession* gateway{};
    /// @brief What STUN discovered so far; may be null.
    net::nat::stun::StunDiscoveryState* stun{};
  };

  /// @brief The console's offer and the candidate that answered.
  struct Met {
    /// @brief The console's offer.
    net::nat::holepunch::ConnRequest peer;
    /// @brief The selected candidate.
    net::nat::holepunch::SelectedCandidate selected;
  };

  /// @brief Bounds a wake by the deadline.
  ///
  /// @param opened The account's PSN session.
  /// @param clock The monotonic clock.
  /// @param parent_stop The start's stop.
  /// @param deadline_microseconds The deadline.
  /// @return A stop that fires with the start or at the deadline; a refusal
  ///     when the session is not open, the start was stopped or the deadline
  ///     passed.
  static auto wake_window(const psn::remote_play::StartedSession& opened,
                          const platform::ClockProvider& clock,
                          platform::StopToken parent_stop,
                          Deadline deadline_microseconds)
      -> Expected<platform::StopToken, WakeFailure>;
  /// @brief Waits until the console reports ready.
  ///
  /// @param opened The account's PSN session.
  /// @param session_client Sends PSN requests.
  /// @param push_client Receives PSN notifications.
  /// @param barrier Which notification counts as ready.
  /// @param clock The monotonic clock.
  /// @param parent_stop The start's stop.
  /// @param bounded_stop The bounded stop.
  /// @param deadline_microseconds The deadline.
  /// @param diagnose Whether to ask PSN about the start command when the wait
  ///     fails.
  /// @return What the console said; the wait failure.
  static auto await_console(psn::remote_play::StartedSession& opened,
                            psn::remote_play::SessionClient& session_client,
                            psn::remote_play::PushClient& push_client,
                            psn::remote_play::ConsoleReadyBarrier barrier,
                            const platform::ClockProvider& clock,
                            platform::StopToken parent_stop,
                            const platform::StopToken& bounded_stop,
                            Deadline deadline_microseconds, bool diagnose)
      -> Expected<psn::remote_play::ConsoleReady, WakeFailure>;
  /// @brief Bounds a route step by the deadline.
  ///
  /// @param platform The host's providers.
  /// @param parent_stop The start's stop.
  /// @param deadline_microseconds The deadline.
  /// @return A bounded stop; RouteFailure::NoHttpProvider, Cancelled or
  ///     TimedOut.
  static auto route_window(platform::PlatformFactory& platform,
                           platform::StopToken parent_stop,
                           Deadline deadline_microseconds)
      -> Expected<platform::StopToken, RouteFailure>;
  /// @brief Checks the PSN exchange can run.
  ///
  /// @param family PS4 or PS5.
  /// @param started The account's session.
  /// @param offer_req_id The offer's request id.
  /// @param accept_req_id The accept's request id.
  /// @return `true` for a known family, an open session and distinct ids in
  ///     range.
  static auto exchange_ready(psn::rest::ConsoleFamily family,
                             AccountSession& started,
                             std::uint64_t offer_req_id,
                             std::uint64_t accept_req_id) noexcept -> bool;
  /// @brief Binds sockets, gathers candidates and prepares an offer.
  ///
  /// @param platform The host's providers.
  /// @param trace Records the steps.
  /// @param secrets The start's random values.
  /// @param bounded_stop The bounded stop.
  /// @param enable_ipv6 Whether to bind IPv6 too.
  /// @param gathering How candidates are gathered.
  /// @param console_sid The console's session id; 0 for the control route.
  /// @return The prepared route; the route failure.
  static auto prepare_route(platform::PlatformFactory& platform,
                            const diagnostics::Trace& trace,
                            const Secrets& secrets,
                            const platform::StopToken& bounded_stop,
                            bool enable_ipv6, Gathering gathering,
                            std::uint32_t console_sid)
      -> Expected<PreparedRoute, RouteFailure>;
  /// @brief Offers candidates through PSN, takes the console's offer and
  /// probes for a working pair.
  ///
  /// @param platform The host's providers.
  /// @param trace Records the steps.
  /// @param session_client Sends PSN requests.
  /// @param push_client Receives PSN notifications.
  /// @param started The account's session.
  /// @param route The prepared route; its sockets are consumed.
  /// @param offer_req_id The offer's request id.
  /// @param family PS4 or PS5.
  /// @param bounded_stop The bounded stop.
  /// @param deadline_microseconds The deadline.
  /// @param nat_socket_count How many sockets probe the NAT.
  /// @param routes Told the steps; may be null.
  /// @param purpose The control or stream route.
  /// @return The console's offer and the selected candidate; the route
  ///     failure.
  static auto meet_console(
      platform::PlatformFactory& platform, const diagnostics::Trace& trace,
      psn::remote_play::SessionClient& session_client,
      psn::remote_play::PushClient& push_client, AccountSession& started,
      PreparedRoute& route, std::uint64_t offer_req_id,
      psn::rest::ConsoleFamily family, const platform::StopToken& bounded_stop,
      Deadline deadline_microseconds, std::size_t nat_socket_count,
      RouteWatcher* routes, progress::InternetRoute::Purpose purpose)
      -> Expected<Met, RouteFailure>;
  /// @brief Tells a watcher a route step.
  ///
  /// @param routes The watcher; may be null.
  /// @param route The step.
  static void tell(RouteWatcher* routes, progress::InternetRoute route);

  /// @brief Gives the deadline of the next wait.
  ///
  /// @return A deadline Reach::wait_microseconds from now.
  auto next_deadline() const noexcept -> Deadline;
  /// @brief Closes the push connection.
  ///
  /// @return The close error, when it failed.
  auto close_push() -> std::optional<platform::WebSocketError>;
  /// @brief Releases the PSN session and waits for its deletion notice.
  void leave_account_session();
  /// @brief Writes one log line.
  ///
  /// @param severity The line's severity.
  /// @param line The line.
  void log(platform::LogSeverity severity, std::string_view line) const;

  /// @brief The host's providers.
  platform::PlatformFactory& platform_;
  /// @brief PS4 or PS5.
  psn::rest::ConsoleFamily family_;
  /// @brief How the start reaches the console.
  Reach reach_;
  /// @brief The start's stop.
  platform::StopToken stop_;
  /// @brief Records the steps.
  const diagnostics::Trace& trace_;
  /// @brief Told a new pairing; may be null.
  PairingListener* pairings_;
  /// @brief Told the route steps; may be null.
  RouteWatcher* routes_watcher_;
  /// @brief The data route's held steps.
  HeldRoutes held_routes_;
  /// @brief The gateway session; null when none.
  std::unique_ptr<platform::GatewaySession> gateway_;
  /// @brief What STUN discovered.
  net::nat::stun::StunDiscoveryState stun_;
  /// @brief The route order.
  RouteOrder routes_;
  /// @brief Sends PSN requests.
  psn::remote_play::SessionClient session_client_;
  /// @brief Receives PSN notifications.
  psn::remote_play::PushClient push_client_;
  /// @brief The Remote Play access token, until the session opens.
  std::optional<users::RemotePlayAccess> access_;
  /// @brief The start's random values.
  std::optional<Secrets> secrets_;
  /// @brief The account's session.
  std::optional<AccountSession> account_;
  /// @brief The prepared control route.
  std::optional<PreparedRoute> prepared_control_route_;
  /// @brief The data route task.
  std::unique_ptr<DataRouteTask> data_route_task_;
  /// @brief The console's session id from the control route.
  std::uint32_t control_console_sid_{};
  /// @brief Whether the push connection was open when signalling stopped.
  bool push_was_open_{};
};

}  // namespace libchiaking::play
