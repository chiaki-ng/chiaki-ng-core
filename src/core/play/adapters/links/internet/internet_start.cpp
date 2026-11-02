#include "src/core/play/adapters/links/internet/internet_start.hpp"

#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/consoles/adapters/pairing/home_pairing.hpp"
#include "src/core/net/protocol/remote_play/stream/limits.hpp"
#include "src/core/net/transport/datagram/udp_transport.hpp"
#include "src/core/net/transport/http/status.hpp"
#include "src/core/users/ports/play_calls.hpp"
#include "src/core/utils/bytes/endian.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace libchiaking::play {

auto InternetStart::RouteOrder::begin(std::uint64_t attempt) -> bool {
  check_owner();
  return machine_.process_event(internet_route_order::Begin{attempt});
}

void InternetStart::RouteOrder::on_start_ready(std::uint64_t attempt) {
  check_owner();
  machine_.process_event(internet_route_order::Started{attempt});
}

void InternetStart::RouteOrder::on_control_established(std::uint64_t attempt) {
  check_owner();
  machine_.process_event(internet_route_order::ControlReady{attempt});
}

void InternetStart::RouteOrder::on_data_established(std::uint64_t attempt) {
  check_owner();
  machine_.process_event(internet_route_order::DataReady{attempt});
}

auto InternetStart::RouteOrder::fail(std::uint64_t attempt) -> bool {
  check_owner();
  return machine_.process_event(internet_route_order::Failure{attempt});
}

auto InternetStart::RouteOrder::timeout(std::uint64_t attempt) -> bool {
  return fail(attempt);
}

auto InternetStart::RouteOrder::cancel(std::uint64_t attempt) -> bool {
  check_owner();
  return machine_.process_event(internet_route_order::Cancel{attempt});
}

auto InternetStart::RouteOrder::take_effects() -> std::vector<Effect> {
  check_owner();
  auto out = std::move(context_.effects);
  context_.effects.clear();
  return out;
}

auto InternetStart::RouteOrder::state() const noexcept -> State {
  check_owner();
  return context_.state;
}

InternetStart::DataRouteTask::DataRouteTask(std::uint64_t attempt_id,
                                            platform::StopToken parent,
                                            Work work)
    : attempt_id_(attempt_id),
      stop_(std::make_shared<LinkedStop>(std::move(parent))),
      work_(std::move(work)) {}

InternetStart::DataRouteTask::~DataRouteTask() {
  cancel_and_join();
  if (result_ && *result_ && result_->value().transport)
    (void)result_->value().transport.close();
}

auto InternetStart::DataRouteTask::start(platform::ThreadingProvider& threading)
    -> Expected<void, platform::PlatformError> {
  if (thread_ || done_.load(std::memory_order_acquire))
    return unexpected(platform::PlatformError::thread_failure);
  platform::native::ThreadEntry entry{&run, this};
  entry.role = platform::ThreadRole::holepunch;
  entry.name = "chiaki-data-route";
  auto thread = threading.start_thread(entry);
  if (!thread) return unexpected(thread.error());
  if (!thread.value())
    return unexpected(platform::PlatformError::system_failure);
  thread_ = std::move(thread.value());
  return {};
}

auto InternetStart::DataRouteTask::take_completion()
    -> std::optional<Completion> {
  if (!done_.load(std::memory_order_acquire) || !result_) return std::nullopt;
  if (thread_ && thread_->joinable()) (void)thread_->join();
  thread_.reset();
  auto result = std::move(*result_);
  result_.reset();
  return Completion{attempt_id_, std::move(result), worker_error_};
}

void InternetStart::DataRouteTask::request_stop() { stop_->cancel(); }

void InternetStart::DataRouteTask::cancel_and_join() {
  request_stop();
  if (thread_ && thread_->joinable()) (void)thread_->join();
  thread_.reset();
}

void InternetStart::DataRouteTask::run(void* context) noexcept {
  auto& task = *static_cast<DataRouteTask*>(context);
  try {
    task.result_.emplace(task.work_(platform::StopToken{task.stop_}));
  } catch (...) {
    task.worker_error_ = platform::PlatformError::system_failure;
    task.result_.emplace(
        unexpected(RouteFailure{RouteFailure::InvalidState{}}));
  }
  task.done_.store(true, std::memory_order_release);
}

auto InternetStart::generate_secrets(platform::SecureRandomProvider& random)
    -> Expected<Secrets, SecretsError> {
  std::array<std::byte, 4> sid_bytes{};
  if (!random.fill_bytes(sid_bytes))
    return unexpected(SecretsError::random_failure);
  Secrets result{};
  result.local_sid = bytes::load_le<std::uint16_t>(sid_bytes);
  if (!random.fill_bytes(
          {result.local_hashed_id.data(), result.local_hashed_id.size()}) ||
      !random.fill_bytes({result.data1.data(), result.data1.size()}) ||
      !random.fill_bytes({result.data2.data(), result.data2.size()}))
    return unexpected(SecretsError::random_failure);
  return result;
}

auto InternetStart::wake_window(const psn::remote_play::StartedSession& opened,
                                const platform::ClockProvider& clock,
                                platform::StopToken parent_stop,
                                Deadline deadline_microseconds)
    -> Expected<platform::StopToken, WakeFailure> {
  if (opened.attempt() == 0 || !opened.connection() ||
      opened.lease().release_attempted())
    return unexpected(WakeFailure{WakeFailure::Refusal::invalid_state});
  if (parent_stop.stop_requested())
    return unexpected(WakeFailure{WakeFailure::Refusal::cancelled});
  if (clock.now_microseconds() >= deadline_microseconds.absolute)
    return unexpected(WakeFailure{WakeFailure::Refusal::timed_out});
  return platform::StopToken{std::make_shared<net::DeadlineStopState>(
      parent_stop, clock, deadline_microseconds.absolute)};
}

auto InternetStart::await_console(
    psn::remote_play::StartedSession& opened,
    psn::remote_play::SessionClient& session_client,
    psn::remote_play::PushClient& push_client,
    psn::remote_play::ConsoleReadyBarrier barrier,
    const platform::ClockProvider& clock, platform::StopToken parent_stop,
    const platform::StopToken& bounded_stop, Deadline deadline_microseconds,
    bool diagnose) -> Expected<psn::remote_play::ConsoleReady, WakeFailure> {
  auto ready = push_client.await_console_ready(
      barrier, opened.attempt(), *opened.connection(), parent_stop, clock,
      deadline_microseconds, [&] {
        return session_client.observe_session(
            opened.lease().bearer_token(), opened.lease().created().session_id,
            psn::remote_play::SessionView::status, bounded_stop);
      });
  if (ready) return std::move(ready.value());
  if (diagnose &&
      ready.error().kind != psn::remote_play::CreationWaitError::cancelled)
    session_client.diagnose_start_command(opened.lease().bearer_token(),
                                          opened.lease().created().session_id,
                                          parent_stop);
  return unexpected(WakeFailure{
      PsnFailure{PsnFailure::Wait{ready.error().kind, ready.error().push}}});
}

auto InternetStart::wake_ps5(
    psn::remote_play::StartedSession& opened,
    psn::remote_play::SessionClient& session_client,
    psn::remote_play::PushClient& push_client,
    std::span<const std::byte, psn_device_uid_size> device_uid,
    std::span<const std::byte, psn_custom_data_size> data1,
    std::span<const std::byte, psn_custom_data_size> data2,
    const platform::ClockProvider& clock, platform::StopToken parent_stop,
    Deadline deadline_microseconds)
    -> Expected<psn::remote_play::ConsoleReady, WakeFailure> {
  auto window = wake_window(opened, clock, parent_stop, deadline_microseconds);
  if (!window) return unexpected(window.error());
  const auto& bounded_stop = window.value();
  auto started = session_client.start_ps5_session(
      opened.lease().bearer_token(), opened.lease().created(), device_uid,
      data1, data2, bounded_stop);
  if (!started) return unexpected(WakeFailure{PsnFailure{started.error()}});
  std::array<std::byte, psn_device_uid_size> expected_uid{};
  std::ranges::copy(device_uid, expected_uid.begin());
  return await_console(
      opened, session_client, push_client,
      psn::remote_play::ConsoleReadyBarrier{opened.attempt(), expected_uid},
      clock, parent_stop, bounded_stop, deadline_microseconds, true);
}

auto InternetStart::wake_ps4(
    psn::remote_play::StartedSession& opened,
    psn::remote_play::SessionClient& session_client,
    psn::remote_play::PushClient& push_client,
    std::span<const std::byte, psn_custom_data_size> data1,
    std::span<const std::byte, psn_custom_data_size> data2,
    const platform::ClockProvider& clock, platform::StopToken parent_stop,
    Deadline deadline_microseconds)
    -> Expected<psn::remote_play::ConsoleReady, WakeFailure> {
  auto window = wake_window(opened, clock, parent_stop, deadline_microseconds);
  if (!window) return unexpected(window.error());
  const auto& bounded_stop = window.value();
  auto profile = session_client.get_ps4_profile_base_url(
      opened.lease().bearer_token(), bounded_stop);
  if (!profile) return unexpected(WakeFailure{PsnFailure{profile.error()}});
  auto woken = session_client.wake_ps4_session(
      opened.lease().bearer_token(), opened.lease().created().session_id,
      profile.value(), opened.online_id(), data1, data2, bounded_stop);
  if (!woken) return unexpected(WakeFailure{PsnFailure{woken.error()}});
  return await_console(
      opened, session_client, push_client,
      psn::remote_play::ConsoleReadyBarrier{opened.attempt(), std::nullopt},
      clock, parent_stop, bounded_stop, deadline_microseconds, false);
}

auto InternetStart::route_window(platform::PlatformFactory& platform,
                                 platform::StopToken parent_stop,
                                 Deadline deadline_microseconds)
    -> Expected<platform::StopToken, RouteFailure> {
  if (!platform.supplies(platform::PlatformCapability::http))
    return unexpected(RouteFailure{RouteFailure::NoHttpProvider{}});
  if (parent_stop.stop_requested())
    return unexpected(RouteFailure{RouteFailure::Cancelled{}});
  auto& clock = platform.clock();
  if (clock.now_microseconds() >= deadline_microseconds.absolute)
    return unexpected(RouteFailure{RouteFailure::TimedOut{}});
  return platform::StopToken{std::make_shared<net::DeadlineStopState>(
      parent_stop, clock, deadline_microseconds.absolute)};
}

auto InternetStart::exchange_ready(psn::rest::ConsoleFamily family,
                                   AccountSession& started,
                                   std::uint64_t offer_req_id,
                                   std::uint64_t accept_req_id) noexcept
    -> bool {
  return (family == psn::rest::ConsoleFamily::ps4 ||
          family == psn::rest::ConsoleFamily::ps5) &&
         started.opened.attempt() != 0 && started.opened.connection() &&
         !started.opened.lease().release_attempted() && offer_req_id != 0 &&
         offer_req_id <= UINT16_MAX && accept_req_id != 0 &&
         accept_req_id <= UINT16_MAX && offer_req_id != accept_req_id;
}

auto InternetStart::prepare_route(platform::PlatformFactory& platform,
                                  const diagnostics::Trace& trace,
                                  const Secrets& secrets,
                                  const platform::StopToken& bounded_stop,
                                  bool enable_ipv6, Gathering gathering,
                                  std::uint32_t console_sid)
    -> Expected<PreparedRoute, RouteFailure> {
  auto& clock = platform.clock();
  auto& network = platform.network();
  auto bound =
      diagnostics::observe(&trace, diagnostics::Operation::sockets, [&] {
        return net::nat::holepunch::BoundSockets::create(network.sockets(),
                                                         enable_ipv6);
      });
  if (!bound) return unexpected(RouteFailure{bound.error()});
  auto poller = network.pollers().create_poller();
  if (!poller || !poller.value())
    return unexpected(RouteFailure{RouteFailure::PollerUnavailable{
        poller ? platform::NetError::not_supported : poller.error()}});
  net::nat::stun::StunDiscoveryState transient_stun;
  auto gathered = diagnostics::observe(
      &trace, diagnostics::Operation::candidate_gather, [&] {
        return net::nat::holepunch::CandidateGathering::gather(
            {*bound.value().ipv4, *poller.value(), network.interfaces(), clock,
             platform.random(), platform.http(), network.dns(),
             gathering.force_port_guessing, gathering.guess_count,
             net::nat::stun::stun_reply_timeout, gathering.gateway},
            bounded_stop, gathering.stun ? *gathering.stun : transient_stun,
            &trace);
      });
  if (!gathered) return unexpected(RouteFailure{gathered.error()});
  trace.record(diagnostics::Operation::candidate_summary,
               diagnostics::Outcome::observed,
               gathered.value().offer_candidates.size());
  trace.record(diagnostics::Operation::nat_random,
               diagnostics::Outcome::observed,
               gathered.value().allocation.random_allocation ? 1 : 0);
  SecretArray<net::holepunch_hashed_id_size> hashed_id;
  std::ranges::copy(secrets.local_hashed_id, hashed_id.begin());
  auto prepared =
      diagnostics::observe(&trace, diagnostics::Operation::offer_prepare, [&] {
        return play::internet::holepunch::Offer::prepare(
            std::move(gathered.value()), secrets.local_sid, console_sid,
            std::move(hashed_id));
      });
  if (!prepared) return unexpected(RouteFailure{prepared.error()});
  return PreparedRoute{std::move(bound.value()), std::move(poller.value()),
                       std::move(prepared.value())};
}

auto InternetStart::meet_console(
    platform::PlatformFactory& platform, const diagnostics::Trace& trace,
    psn::remote_play::SessionClient& session_client,
    psn::remote_play::PushClient& push_client, AccountSession& started,
    PreparedRoute& route, std::uint64_t offer_req_id,
    psn::rest::ConsoleFamily family, const platform::StopToken& bounded_stop,
    Deadline deadline_microseconds, std::size_t nat_socket_count,
    RouteWatcher* routes, progress::InternetRoute::Purpose purpose)
    -> Expected<Met, RouteFailure> {
  using Route = progress::InternetRoute;
  auto& clock = platform.clock();
  auto& network = platform.network();
  const auto& created = started.opened.lease().created();
  const auto& token = started.opened.lease().bearer_token();
  tell(routes, {Route::Step::exchanging_offers, purpose,
                candidates_of(route.offer.request.candidates)});
  auto peer =
      diagnostics::observe(&trace, diagnostics::Operation::offer_exchange, [&] {
        return play::internet::holepunch::Offer::run_psn(
            started.opened.attempt(), offer_req_id, session_client, push_client,
            *started.opened.connection(), token, created.session_id,
            created.account_id, started.ready.device_uid, family,
            route.offer.request, bounded_stop, clock, deadline_microseconds,
            &trace);
      });
  if (!peer) return unexpected(RouteFailure{peer.error()});
  if (route.offer.allocation.random_allocation)
    net::nat::holepunch::BoundSockets::add_nat_sockets(
        route.sockets, network.sockets(), nat_socket_count, bounded_stop);
  trace.record(diagnostics::Operation::nat_sockets,
               diagnostics::Outcome::observed, route.sockets.probes.size());
  tell(routes, {Route::Step::testing_candidates, purpose,
                candidates_of(peer.value().candidates)});
  auto selected = diagnostics::observe(
      &trace, diagnostics::Operation::candidate_probe, [&] {
        return play::internet::holepunch::Offer::probe_peer(
            route.offer.request, peer.value(), std::move(route.sockets),
            network.dns(), *route.poller, clock, platform.random(),
            bounded_stop);
      });
  if (!selected) return unexpected(RouteFailure{selected.error()});
  trace.record(diagnostics::Operation::candidate_selected,
               diagnostics::Outcome::observed,
               selected.value().candidate_index);
  const auto chosen = kind_of(peer.value(), selected.value());
  tell(routes, {Route::Step::route_chosen, purpose, std::nullopt, chosen});
  tell(routes, {Route::Step::confirming_route, purpose, std::nullopt, chosen});
  return Met{std::move(peer.value()), std::move(selected.value())};
}

auto InternetStart::prepare_control_route(
    platform::PlatformFactory& platform, const Secrets& secrets,
    platform::StopToken parent_stop, Deadline deadline_microseconds,
    bool enable_ipv6, bool force_port_guessing, std::size_t guess_count,
    platform::GatewaySession* gateway, net::nat::stun::StunDiscoveryState* stun)
    -> Expected<PreparedRoute, RouteFailure> {
  const diagnostics::Trace trace{
      platform.supplies(platform::PlatformCapability::diagnostics)
          ? &platform.diagnostics()
          : nullptr,
      platform.clock(),
      diagnostics::Component::control_route,
      parent_stop};
  auto window = route_window(platform, parent_stop, deadline_microseconds);
  if (!window) return unexpected(window.error());
  return prepare_route(platform, trace, secrets, window.value(), enable_ipv6,
                       {force_port_guessing, guess_count, gateway, stun}, 0);
}

auto InternetStart::establish_control_route(
    platform::PlatformFactory& platform,
    psn::remote_play::SessionClient& session_client,
    psn::remote_play::PushClient& push_client, AccountSession& started,
    const Secrets& secrets, std::uint64_t offer_req_id,
    std::uint64_t accept_req_id, platform::StopToken parent_stop,
    Deadline deadline_microseconds, bool enable_ipv6,
    psn::rest::ConsoleFamily family, std::optional<PreparedRoute> preparation,
    std::size_t nat_socket_count, RouteWatcher* routes)
    -> Expected<play::internet::holepunch::CtrlRudpHandoff, RouteFailure> {
  const diagnostics::Trace trace{
      platform.supplies(platform::PlatformCapability::diagnostics)
          ? &platform.diagnostics()
          : nullptr,
      platform.clock(),
      diagnostics::Component::control_route,
      parent_stop};
  if (!exchange_ready(family, started, offer_req_id, accept_req_id))
    return unexpected(RouteFailure{RouteFailure::InvalidState{}});
  auto window = route_window(platform, parent_stop, deadline_microseconds);
  if (!window) return unexpected(window.error());
  const auto& bounded_stop = window.value();
  if (!preparation) {
    auto prepared = prepare_control_route(platform, secrets, parent_stop,
                                          deadline_microseconds, enable_ipv6);
    if (!prepared) return unexpected(prepared.error());
    preparation.emplace(std::move(prepared.value()));
  }
  if (!preparation->sockets.ipv4 || !preparation->poller)
    return unexpected(RouteFailure{RouteFailure::InvalidState{}});
  auto met = meet_console(platform, trace, session_client, push_client, started,
                          *preparation, offer_req_id, family, bounded_stop,
                          deadline_microseconds, nat_socket_count, routes,
                          progress::InternetRoute::Purpose::control);
  if (!met) return unexpected(met.error());
  const auto& created = started.opened.lease().created();
  auto accepted = diagnostics::observe(
      &trace, diagnostics::Operation::accept_exchange, [&] {
        return play::internet::holepunch::Accept::transport_ctrl(
            platform.network(), preparation->offer, met.value().peer,
            std::move(met.value().selected), started.opened.attempt(),
            accept_req_id, session_client, push_client,
            *started.opened.connection(), started.opened.lease().bearer_token(),
            created.session_id, created.account_id, started.ready.device_uid,
            family, bounded_stop, platform.clock(), deadline_microseconds,
            &trace);
      });
  if (!accepted) return unexpected(RouteFailure::of(accepted.error()));
  accepted.value().console_sid = met.value().peer.sid;
  return std::move(accepted.value());
}

auto InternetStart::adopt_data_route(
    platform::NetworkProvider& network,
    std::unique_ptr<platform::UdpSocket> socket,
    platform::SocketEndpoint selected_peer)
    -> Expected<net::protocol::remote_play::RouteCandidate,
                RouteFailure::Adoption> {
  using Error = RouteFailure::Adoption;
  if (!socket) return unexpected(Error::null_socket);
  if (&socket->owner() != &network) return unexpected(Error::foreign_provider);
  if (selected_peer.port == 0 || selected_peer.address.is_unspecified())
    return unexpected(Error::invalid_peer);

  const auto local = socket->local_endpoint();
  if (!local.has_value() || local.value().port == 0)
    return unexpected(Error::unbound_socket);
  if (local.value().address.family() != selected_peer.address.family())
    return unexpected(Error::invalid_peer);

  const auto capabilities = network.capabilities();
  const bool dont_fragment = selected_peer.address.is_ipv4()
                                 ? capabilities.socket_dont_fragment_ipv4
                                 : capabilities.socket_dont_fragment_ipv6;
  if (dont_fragment &&
      !socket->set_boolean_option(platform::SocketOption::dont_fragment, true))
    return unexpected(Error::transport_error);

  auto transport =
      net::transport::datagram::ProviderUdpTransport::adopt_connected(
          network, std::move(socket), selected_peer);
  if (!transport.has_value()) return unexpected(Error::transport_error);

  std::unique_ptr<net::transport::datagram::DatagramTransport> route_transport{
      std::move(transport.value())};
  return net::protocol::remote_play::RouteCandidate{
      std::move(route_transport), local.value(), selected_peer,
      net::protocol::remote_play::RouteKind::remote_rudp,
      dont_fragment ? net::protocol::remote_play::DontFragmentPolicy::enabled
                    : net::protocol::remote_play::DontFragmentPolicy::disabled};
}

auto InternetStart::hand_off_data_route(
    platform::NetworkProvider& network,
    net::nat::holepunch::SelectedCandidate selected,
    const play::internet::holepunch::AcceptWait& accept_wait)
    -> Expected<net::protocol::remote_play::RouteCandidate, RouteFailure> {
  if (accept_wait.state() != play::internet::holepunch::AcceptWaitState::ready)
    return unexpected(RouteFailure{RouteFailure::AcceptNotReady{}});
  auto route =
      adopt_data_route(network, std::move(selected.socket), selected.peer);
  if (!route) return unexpected(RouteFailure{route.error()});
  return std::move(route.value());
}

auto InternetStart::accept_data_route(
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
    const diagnostics::Trace* trace)
    -> Expected<net::protocol::remote_play::RouteCandidate, RouteFailure> {
  auto exchanged = play::internet::holepunch::Accept::run_selected_exchange(
      network, ours, console, std::move(selected), attempt, local_req_id,
      session, push, connection, bearer_token, session_id, account_id,
      console_uid, family, std::move(parent_stop), clock, deadline_microseconds,
      trace);
  if (!exchanged) return unexpected(RouteFailure{exchanged.error()});
  return hand_off_data_route(network, std::move(exchanged.value().selected),
                             exchanged.value().ready);
}

auto InternetStart::establish_data_route(
    platform::PlatformFactory& platform,
    psn::remote_play::SessionClient& session_client,
    psn::remote_play::PushClient& push_client, AccountSession& started,
    const Secrets& secrets, std::uint64_t offer_req_id,
    std::uint64_t accept_req_id, platform::StopToken parent_stop,
    Deadline deadline_microseconds, bool enable_ipv6,
    psn::rest::ConsoleFamily family, bool force_port_guessing,
    std::size_t guess_count, std::size_t nat_socket_count,
    platform::GatewaySession* gateway, net::nat::stun::StunDiscoveryState* stun,
    std::uint32_t ctrl_console_sid, RouteWatcher* routes)
    -> Expected<net::protocol::remote_play::RouteCandidate, RouteFailure> {
  const diagnostics::Trace trace{
      platform.supplies(platform::PlatformCapability::diagnostics)
          ? &platform.diagnostics()
          : nullptr,
      platform.clock(),
      diagnostics::Component::data_route,
      parent_stop};
  if (!exchange_ready(family, started, offer_req_id, accept_req_id))
    return unexpected(RouteFailure{RouteFailure::InvalidState{}});
  auto window = route_window(platform, parent_stop, deadline_microseconds);
  if (!window) return unexpected(window.error());
  const auto& bounded_stop = window.value();
  auto prepared = prepare_route(
      platform, trace, secrets, bounded_stop, enable_ipv6,
      {force_port_guessing, guess_count, gateway, stun}, ctrl_console_sid);
  if (!prepared) return unexpected(prepared.error());
  auto met = meet_console(platform, trace, session_client, push_client, started,
                          prepared.value(), offer_req_id, family, bounded_stop,
                          deadline_microseconds, nat_socket_count, routes,
                          progress::InternetRoute::Purpose::stream);
  if (!met) return unexpected(met.error());
  const auto& created = started.opened.lease().created();
  return diagnostics::observe(
      &trace, diagnostics::Operation::accept_exchange, [&] {
        return accept_data_route(
            platform.network(), prepared.value().offer, met.value().peer,
            std::move(met.value().selected), started.opened.attempt(),
            accept_req_id, session_client, push_client,
            *started.opened.connection(), started.opened.lease().bearer_token(),
            created.session_id, created.account_id, started.ready.device_uid,
            family, bounded_stop, platform.clock(), deadline_microseconds,
            &trace);
      });
}

void InternetStart::HeldRoutes::on_route(const progress::InternetRoute& route) {
  const platform::Lock lock(mutex_);
  held_.push_back(route);
}

void InternetStart::HeldRoutes::release_to(RouteWatcher* watcher) {
  std::vector<progress::InternetRoute> released;
  {
    const platform::Lock lock(mutex_);
    released.swap(held_);
  }
  if (watcher == nullptr) return;
  for (const auto& route : released) watcher->on_route(route);
}

void InternetStart::tell(RouteWatcher* routes, progress::InternetRoute route) {
  if (routes != nullptr) routes->on_route(route);
}

auto InternetStart::candidates_of(
    const std::vector<net::nat::holepunch::Candidate>& offered) noexcept
    -> progress::InternetRoute::Candidates {
  using Type = net::nat::holepunch::CandidateType;
  const auto count = [&](Type type) {
    return static_cast<std::uint32_t>(std::ranges::count(
        offered, type, &net::nat::holepunch::Candidate::type));
  };
  const auto mapped = count(Type::stun);
  const auto fixed = count(Type::static_) + count(Type::derived);
  progress::InternetRoute::Candidates candidates;
  candidates.local = count(Type::local);
  candidates.public_address = mapped > 1 ? fixed : mapped + fixed;
  candidates.port_guessed = mapped > 1 ? mapped : 0;
  return candidates;
}

auto InternetStart::kind_of(
    const net::nat::holepunch::ConnRequest& console,
    const net::nat::holepunch::SelectedCandidate& selected) noexcept
    -> progress::InternetRoute::Kind {
  using Kind = progress::InternetRoute::Kind;
  using Type = net::nat::holepunch::CandidateType;
  if (selected.derived || selected.candidate_index >= console.candidates.size())
    return Kind::discovered;
  switch (console.candidates[selected.candidate_index].type) {
    case Type::local:
      return Kind::local;
    case Type::stun:
      return candidates_of(console.candidates).port_guessed != 0
                 ? Kind::port_guessed
                 : Kind::public_address;
    case Type::derived:
      return Kind::discovered;
    case Type::static_:
      break;
  }
  return Kind::public_address;
}

auto InternetStart::paired_console(
    const net::protocol::remote_play::RegisteredConsole& console,
    net::protocol::remote_play::RegistrationTarget asked)
    -> std::optional<consoles::PairedConsole> {
  const auto firmware = consoles::Firmware::from(asked);
  if (!firmware) return std::nullopt;
  net::protocol::remote_play::RegisteredConsole copy;
  copy.target = console.target;
  copy.nickname = console.nickname;
  copy.mac = console.mac;
  copy.registration_key = console.registration_key.clone();
  copy.key = console.key.clone();
  copy.key_type = console.key_type;
  auto paired = consoles::HomePairing::paired(std::move(copy), *firmware);
  if (!paired) return std::nullopt;
  return std::move(paired.value());
}

InternetStart::InternetStart(platform::PlatformFactory& platform,
                             psn::rest::ConsoleFamily family, Reach reach,
                             platform::StopToken stop,
                             const diagnostics::Trace& trace,
                             PairingListener* pairings, RouteWatcher* routes)
    : platform_(platform),
      family_(family),
      reach_(reach),
      stop_(std::move(stop)),
      trace_(trace),
      pairings_(pairings),
      routes_watcher_(routes),
      held_routes_(platform.threading()),
      session_client_(platform.http(), platform.clock(), platform.random(),
                      platform.supplies(platform::PlatformCapability::diagnostics)
                          ? &platform.diagnostics()
                          : nullptr),
      push_client_(platform.websocket(),
                   platform.supplies(platform::PlatformCapability::diagnostics)
                       ? &platform.diagnostics()
                       : nullptr) {}

InternetStart::~InternetStart() {
  (void)stop_signalling(false);
  (void)leave();
}

auto InternetStart::begin() -> bool { return routes_.begin(this_attempt); }

auto InternetStart::find_gateway() -> bool {
  if (platform_.supplies(platform::PlatformCapability::gateway_mapping)) {
    auto& provider = platform_.gateway();
    auto discovered =
        diagnostics::observe(&trace_, diagnostics::Operation::gateway_discover,
                             [&] { return provider.discover(stop_); });
    if (discovered) gateway_ = std::move(discovered.value());
    return true;
  }
  trace_.record(diagnostics::Operation::gateway_discover,
                diagnostics::Outcome::observed,
                static_cast<unsigned>(platform::GatewayError::unavailable));
  return false;
}

auto InternetStart::draw_secrets() -> Expected<void, SecretsError> {
  auto secrets = generate_secrets(platform_.random());
  if (!secrets) return unexpected(secrets.error());
  secrets_.emplace(std::move(secrets.value()));
  return {};
}

auto InternetStart::take_access(users::PsUser& user)
    -> Expected<void, psn::remote_play::StartError> {
  auto access = users::PlayCalls{user}.remote_play_access(stop_);
  if (!access) {
    if (access.error().reason == users::Unanswered::Reason::cancelled)
      return unexpected(psn::remote_play::StartError{
          psn::remote_play::StartError::Cancelled{}});
    return unexpected(psn::remote_play::StartError{
        psn::remote_play::StartError::PushOpenFailed{
            psn::remote_play::PushOpenError::InvalidToken{}}});
  }
  access_.emplace(std::move(access.value()));
  return {};
}

auto InternetStart::open_account_session()
    -> Expected<void, psn::remote_play::StartError> {
  if (!access_)
    return unexpected(psn::remote_play::StartError{
        psn::remote_play::StartError::PushOpenFailed{
            psn::remote_play::PushOpenError::InvalidToken{}}});
  psn::remote_play::CreationBarrier barrier{this_attempt};
  auto opened = psn::remote_play::StartedSession::open(
      session_client_, push_client_, barrier, access_->bearer().clone(),
      this_attempt, platform_.clock(), stop_, next_deadline());
  if (!opened) {
    using E = psn::remote_play::StartError;
    const auto* push_open = opened.error().as<E::PushOpenFailed>();
    const auto* fqdn =
        push_open ? push_open->push_open
                        .as<psn::remote_play::PushOpenError::GetFqdnFailed>()
                  : nullptr;
    const auto* create = opened.error().as<E::CreateFailed>();
    if ((fqdn && net::HttpStatus{fqdn->session.status} ==
                     net::HttpStatus::unauthorized) ||
        (create && net::HttpStatus{create->create.status} ==
                       net::HttpStatus::unauthorized))
      access_->refused_by_service();
    access_.reset();
    return unexpected(opened.error());
  }
  access_.reset();
  account_.emplace(AccountSession{std::move(opened.value()), {}});
  return {};
}

auto InternetStart::listen() -> Expected<void, platform::PlatformError> {
  return push_client_.start_service(
      *account_->opened.connection(), session_client_, platform_.threading(),
      platform_.clock(), this_attempt, account_->opened.lease().bearer_token(),
      account_->opened.lease().created().session_id,
      account_->opened.lease().created().account_id, {}, family_, stop_);
}

auto InternetStart::prepare_control_route() -> Expected<void, RouteFailure> {
  auto prepared = prepare_control_route(
      platform_, *secrets_, stop_, next_deadline(), ipv6_enabled(),
      reach_.force_port_guessing, reach_.port_guesses, gateway_.get(), &stun_);
  if (!prepared) return unexpected(prepared.error());
  prepared_control_route_.emplace(std::move(prepared.value()));
  return {};
}

auto InternetStart::wake(
    std::span<const std::byte, psn_device_uid_size> device_uid)
    -> Expected<void, WakeFailure> {
  const std::span<const std::byte, psn_custom_data_size> data1{
      secrets_->data1.data(), secrets_->data1.size()};
  const std::span<const std::byte, psn_custom_data_size> data2{
      secrets_->data2.data(), secrets_->data2.size()};
  auto ready =
      family_ == psn::rest::ConsoleFamily::ps4
          ? wake_ps4(account_->opened, session_client_, push_client_, data1,
                     data2, platform_.clock(), stop_, next_deadline())
          : wake_ps5(account_->opened, session_client_, push_client_,
                     device_uid, data1, data2, platform_.clock(), stop_,
                     next_deadline());
  if (!ready) return unexpected(ready.error());
  account_->ready = std::move(ready.value());
  push_client_.service_console_uid(account_->ready.device_uid);
  routes_.on_start_ready(this_attempt);
  return {};
}

auto InternetStart::control_route_is_next() -> bool {
  return routes_.next_is<RouteOrder::StartControlRoute>(this_attempt);
}

auto InternetStart::control_route()
    -> Expected<play::internet::holepunch::CtrlRudpHandoff, RouteFailure> {
  auto route = establish_control_route(
      platform_, session_client_, push_client_, *account_, *secrets_,
      net::protocol::remote_play::ctrl_route_offer_req_id,
      net::protocol::remote_play::ctrl_route_accept_req_id, stop_,
      next_deadline(), ipv6_enabled(), family_,
      std::move(prepared_control_route_), reach_.nat_probe_sockets,
      routes_watcher_);
  prepared_control_route_.reset();
  if (!route) return unexpected(route.error());
  push_client_.service_phase(this_attempt,
                             psn::remote_play::PsnServicePhase::data);
  control_console_sid_ = route.value().console_sid;
  return std::move(route.value());
}

auto InternetStart::pair(net::transport::rudp::HandshakeChannel& channel,
                         platform::CryptoProvider& crypto,
                         platform::ClockProvider& clock,
                         net::protocol::remote_play::RegistrationTarget target,
                         std::span<const std::byte> account_id,
                         std::string_view local_address)
    -> Expected<play::internet::PsnRemoteRegistrationResult,
                play::internet::PsnRemoteRegistrationFailure> {
  auto paired = play::internet::RudpPairing(channel, platform_.random(), crypto,
                                            clock, &trace_)
                    .pair(stop_, this_attempt, target, account_id,
                          account_->ready.custom_data1,
                          {secrets_->data1.data(), secrets_->data1.size()},
                          {secrets_->data2.data(), secrets_->data2.size()},
                          local_address);
  return paired;
}

auto InternetStart::keep(
    const net::protocol::remote_play::RegisteredConsole& console,
    net::protocol::remote_play::RegistrationTarget asked)
    -> Expected<void, PairingListener::Refusal> {
  if (pairings_ == nullptr) return {};
  auto paired = paired_console(console, asked);
  if (!paired) return {};
  return pairings_->on_paired(std::move(*paired));
}

auto InternetStart::data_route_is_next() -> bool {
  routes_.on_control_established(this_attempt);
  return routes_.next_is<RouteOrder::StartDataRoute>(this_attempt);
}

void InternetStart::plan_data_route() {
  const auto deadline = next_deadline();
  const auto family = family_;
  const auto enable_ipv6 = ipv6_enabled();
  data_route_task_ = std::make_unique<DataRouteTask>(
      this_attempt, stop_,
      [this, deadline, family, enable_ipv6](platform::StopToken stop) {
        return establish_data_route(
            platform_, session_client_, push_client_, *account_, *secrets_,
            net::protocol::remote_play::data_route_offer_req_id,
            net::protocol::remote_play::data_route_accept_req_id,
            std::move(stop), deadline, enable_ipv6, family,
            reach_.force_port_guessing, reach_.port_guesses,
            reach_.nat_probe_sockets, gateway_.get(), &stun_,
            control_console_sid_, &held_routes_);
      });
}

void InternetStart::drop_data_route() noexcept { data_route_task_.reset(); }

auto InternetStart::launch_data_route()
    -> Expected<void, platform::PlatformError> {
  return data_route_task_->start(platform_.threading());
}

auto InternetStart::data_route_is_planned() const noexcept -> bool {
  return data_route_task_ != nullptr;
}

auto InternetStart::take_data_route()
    -> std::optional<DataRouteTask::Completion> {
  auto completion = data_route_task_->take_completion();
  held_routes_.release_to(routes_watcher_);
  if (completion) data_route_task_.reset();
  return completion;
}

auto InternetStart::routes_are_ready(std::uint64_t completed_attempt) -> bool {
  routes_.on_data_established(completed_attempt);
  if (!routes_.next_is<RouteOrder::RoutesReady>(this_attempt)) return false;
  push_client_.service_phase(this_attempt,
                             psn::remote_play::PsnServicePhase::active);
  return true;
}

void InternetStart::route_failed() { (void)routes_.fail(this_attempt); }

auto InternetStart::signalling_failure() const
    -> std::optional<psn::remote_play::PushError> {
  return push_client_.service_failure();
}

auto InternetStart::is_open() const noexcept -> bool {
  return account_.has_value();
}

auto InternetStart::has_secrets() const noexcept -> bool {
  return secrets_.has_value();
}

auto InternetStart::ipv6_enabled() const noexcept -> bool {
  return reach_.ipv6 && family_ == psn::rest::ConsoleFamily::ps5;
}

auto InternetStart::stop_signalling(bool close_push_first)
    -> std::optional<platform::WebSocketError> {
  (void)routes_.cancel(this_attempt);
  if (data_route_task_) data_route_task_->request_stop();
  push_was_open_ = account_ && account_->opened.connection() &&
                   !push_client_.service_failure();
  push_client_.quiesce_service();
  std::optional<platform::WebSocketError> close_failure;
  if (close_push_first && account_ && account_->opened.connection()) {
    log(platform::LogSeverity::info,
        "remote stop: sleep closed PSN push before media teardown; skipping "
        "membership DELETE");
    close_failure = close_push();
  }
  data_route_task_.reset();
  push_client_.stop_service();
  return close_failure;
}

auto InternetStart::leave() -> Left {
  Left left;
  if (account_) {
    if (account_->opened.connection()) {
      if (push_was_open_ && !account_->opened.lease().release_attempted())
        leave_account_session();
      left.push_close_failure = close_push();
    }
    account_.reset();
  }
  access_.reset();
  prepared_control_route_.reset();
  secrets_.reset();
  if (gateway_) {
    auto released =
        diagnostics::observe(&trace_, diagnostics::Operation::gateway_release,
                             [&] { return gateway_->close(); });
    if (!released) left.gateway_released = false;
    gateway_.reset();
  }
  return left;
}

auto InternetStart::next_deadline() const noexcept -> Deadline {
  return Deadline::per_wait(reach_.wait_microseconds);
}

auto InternetStart::close_push() -> std::optional<platform::WebSocketError> {
  auto closed = diagnostics::observe(
      &trace_, diagnostics::Operation::psn_push_close,
      [&] { return account_->opened.connection().close(); });
  if (!closed) return closed.error();
  return std::nullopt;
}

void InternetStart::leave_account_session() {
  auto released = diagnostics::observe(
      &trace_, diagnostics::Operation::psn_release,
      [&] { return account_->opened.lease().release({}); });
  if (!released) {
    const auto& error = released.error();
    log(platform::LogSeverity::error,
        std::format("remote stop: PSN membership DELETE failed kind={} "
                    "http={} transport={}",
                    std::to_underlying(error.kind), error.status,
                    std::to_underlying(error.transport)));
  }
  push_client_.scope_session({});
  const auto now = platform_.clock().now_microseconds();
  const auto deadline =
      timing::Deadline::after_us(now, net::psn_session_deletion_timeout_us);
  bool confirmed = false;
  for (;;) {
    auto notification = push_client_.receive_bounded(
        *account_->opened.connection(), {}, platform_.clock(), deadline);
    if (!notification) {
      const auto& error = notification.error();
      log(platform::LogSeverity::error,
          std::format("remote stop: PSN deletion wait failed kind={} push={} "
                      "backend={}",
                      std::to_underlying(error.kind),
                      std::to_underlying(error.push.kind),
                      std::to_underlying(error.push.backend)));
      break;
    }
    const auto type = notification.value().type();
    if (type == psn::rest::NotificationType::members_deleted ||
        type == psn::rest::NotificationType::remote_play_session_deleted) {
      confirmed = true;
      break;
    }
  }
  log(confirmed ? platform::LogSeverity::info : platform::LogSeverity::warning,
      confirmed ? "remote stop: current PSN membership deletion observed"
                : "remote stop: PSN deletion unconfirmed");
}

void InternetStart::log(platform::LogSeverity severity,
                        std::string_view line) const {
  if (platform_.supplies(platform::PlatformCapability::diagnostics))
    platform_.diagnostics().write({severity, line});
}

}  // namespace libchiaking::play
