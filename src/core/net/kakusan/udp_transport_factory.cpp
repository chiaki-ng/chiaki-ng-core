#include "src/core/net/kakusan/udp_transport_factory.hpp"

#include "src/core/net/host_port.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "src/core/net/nat/ice/agent.hpp"
#include "src/core/net/nat/ice/candidate_policy.hpp"
#include "src/core/net/nat/stun/message.hpp"
#include "src/core/net/ports.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <utility>

namespace libchiaking::net::kakusan {

UdpTransportFactory::UdpTransportFactory(UdpTransportDependencies dependencies,
                                         UdpTransportConfig config)
    : dependencies_(std::move(dependencies)), config_(std::move(config)) {
  auto poller = dependencies_.network.pollers().create_poller();
  registry_ = std::make_shared<SocketRegistry>(
      poller ? std::move(poller).value() : std::unique_ptr<platform::Poller>{});
}

UdpTransportFactory::~UdpTransportFactory() = default;

auto UdpTransportFactory::create(TransportRole role)
    -> Result<std::unique_ptr<SpokeTransport>> {
  if (!dependencies_.security) return fail(Error::internal);
  std::unique_ptr<SpokeTransport> transport =
      std::make_unique<UdpSpokeTransport>(dependencies_, config_, registry_,
                                          role);
  return transport;
}

auto UdpTransportFactory::wait(std::uint64_t timeout_us) -> WaitResult {
  return registry_->wait(timeout_us);
}

void UdpTransportFactory::wake() noexcept { registry_->wake(); }

auto UdpTransportFactory::resolve_stun_servers(
    platform::DnsProvider& dns, std::span<const std::string> servers)
    -> std::vector<platform::SocketEndpoint> {
  std::vector<platform::SocketEndpoint> resolved;
  for (const auto& entry : servers) {
    if (resolved.size() == nat::ice::max_stun_servers) break;
    const auto target = StunTarget::parse(entry);
    if (!target) continue;
    std::vector<platform::IpAddress> addresses;
    if (const auto literal = platform::parse_ip_address(target->host)) {
      if (target->bracketed && !literal.value().is_ipv6()) continue;
      addresses.push_back(literal.value());
    } else if (target->literal) {
      continue;
    } else {
      auto looked_up = dns.resolve(target->host, std::to_string(target->port));
      if (!looked_up) continue;
      addresses = std::move(looked_up).value();
    }
    for (const auto& found : addresses) {
      if (resolved.size() == nat::ice::max_stun_servers) break;
      const auto address = found.unmapped_ipv4();
      if (address.is_unspecified()) continue;
      const platform::SocketEndpoint endpoint{address, target->port};
      if (std::find(resolved.begin(), resolved.end(), endpoint) ==
          resolved.end())
        resolved.push_back(endpoint);
    }
  }
  return resolved;
}

UdpTransportFactory::SocketRegistry::SocketRegistry(
    std::unique_ptr<platform::Poller> poller) noexcept
    : poller_(std::move(poller)) {}

void UdpTransportFactory::SocketRegistry::attach(
    const platform::UdpSocket& socket) {
  sockets_.push_back(&socket);
}

void UdpTransportFactory::SocketRegistry::detach(
    const platform::UdpSocket& socket) noexcept {
  std::erase(sockets_, &socket);
}

auto UdpTransportFactory::SocketRegistry::wait(std::uint64_t timeout_us)
    -> WaitResult {
  if (!poller_) return WaitResult::unavailable;
  requests_.clear();
  for (std::size_t index = 0; index < sockets_.size(); ++index)
    requests_.push_back(platform::PollRequest{sockets_[index], index,
                                              platform::PollEvent::readable});
  const std::chrono::microseconds timeout{
      static_cast<std::chrono::microseconds::rep>(
          std::min(timeout_us, max_wait_us))};
  const auto result = poller_->wait(requests_, timeout, platform::StopToken{});
  requests_.clear();
  if (!result) return WaitResult::unavailable;
  if (!result.value().ready.empty()) return WaitResult::readable;
  if (result.value().woke) return WaitResult::woken;
  return WaitResult::timed_out;
}

void UdpTransportFactory::SocketRegistry::wake() noexcept {
  if (poller_) static_cast<void>(poller_->wake());
}

auto UdpTransportFactory::StunTarget::parse(std::string_view text)
    -> std::optional<StunTarget> {
  if (text.size() > max_stun_host_size + 2 + 1 + max_port_digits)
    return std::nullopt;
  const auto written = HostPort::split(text);
  if (!written) return std::nullopt;
  if (written->bare_ipv6())
    return StunTarget{text, default_stun_port, false, true};
  auto port = default_stun_port;
  if (written->port_text()) {
    const auto chosen = written->port();
    if (!chosen || *chosen == 0) return std::nullopt;
    port = *chosen;
  }
  if (written->bracketed())
    return StunTarget{written->host(), port, true, true};
  if (written->host().empty() || written->host().size() > max_stun_host_size)
    return std::nullopt;
  return StunTarget{written->host(), port, false, false};
}

UdpTransportFactory::UdpSpokeTransport::UdpSpokeTransport(
    const UdpTransportDependencies& dependencies,
    const UdpTransportConfig& config, std::shared_ptr<SocketRegistry> registry,
    TransportRole role)
    : network_(dependencies.network),
      crypto_(dependencies.crypto),
      random_(dependencies.random),
      security_factory_(dependencies.security),
      stun_servers_(config.stun_servers),
      enable_ipv6_(config.enable_ipv6),
      guessing_(config.guessing),
      registry_(std::move(registry)),
      role_(role) {}

UdpTransportFactory::UdpSpokeTransport::~UdpSpokeTransport() { close(); }

auto UdpTransportFactory::UdpSpokeTransport::gather(
    std::uint64_t now_us, std::optional<IceCredentials> credentials)
    -> Result<void> {
  if (closed_ || started_) return fail(Error::internal);
  started_ = true;
  observe(now_us);
  auto bases = bind_sockets();
  if (bases.empty()) {
    stop(Error::no_path);
    return {};
  }
  auto security =
      security_factory_
          ? security_factory_(role_ == TransportRole::host
                                  ? SecurityRole::client
                                  : SecurityRole::server)
          : Result<std::unique_ptr<DatagramSecurity>>{fail(Error::internal)};
  if (!security || !security.value())
    return abandon(security ? Error::internal : security.error());
  security_ = std::move(security).value();
  nat::ice::AgentConfig config{role_, std::move(bases), stun_servers_,
                               guessing_, std::move(credentials)};
  auto agent =
      nat::ice::Agent::create(crypto_, random_, std::move(config), now_us_);
  if (!agent) return abandon(path_failure(agent.error()));
  agent_ = std::move(agent).value();
  drain_agent();
  return {};
}

auto UdpTransportFactory::UdpSpokeTransport::connect(
    const TransportDescription& remote, std::uint64_t now_us) -> Result<void> {
  if (closed_) return fail(Error::internal);
  if (failed_) return fail(Error::no_path);
  if (!agent_ || !gathered_ || connected_) return fail(Error::internal);
  observe(now_us);
  const auto connected = agent_->connect(remote.ice, now_us_);
  if (!connected) return fail(path_failure(connected.error()));
  remote_digest_ = remote.certificate_digest;
  connected_ = true;
  drain_agent();
  return {};
}

auto UdpTransportFactory::UdpSpokeTransport::pump(
    std::uint64_t now_us, std::vector<TransportEvent>& events) -> Result<void> {
  if (closed_) return {};
  observe(now_us);
  if (!failed_ && agent_) {
    read_sockets();
    drive_timers();
  }
  while (!pending_.empty()) {
    events.push_back(surfaced(std::move(pending_.front())));
    pending_.pop_front();
  }
  return {};
}

auto UdpTransportFactory::UdpSpokeTransport::next_deadline_us() const noexcept
    -> std::optional<std::uint64_t> {
  if (closed_) return std::nullopt;
  if (!pending_.empty() || backlog_) return now_us_;
  if (failed_ || !agent_) return std::nullopt;
  auto deadline = agent_->next_deadline_us();
  if (securing_ && security_)
    deadline =
        timing::Deadline::earliest(deadline, security_->next_deadline_us());
  return deadline;
}

auto UdpTransportFactory::UdpSpokeTransport::send_record(
    std::span<const std::byte> record) -> Result<void> {
  if (!usable()) return fail(Error::no_path);
  if (record.empty() || record.size() > security_->max_record_size())
    return fail(Error::internal);
  const auto sent = security_->send(record);
  if (!sent) {
    stop(sent.error());
    return fail(sent.error());
  }
  flush_security();
  return {};
}

auto UdpTransportFactory::UdpSpokeTransport::send_datagram(
    std::span<const std::byte> datagram) -> Result<void> {
  if (!usable()) return fail(Error::no_path);
  if (!std::ranges::starts_with(datagram, Hash::bytes_of(media_prefix)) ||
      datagram.size() > max_datagram_size)
    return fail(Error::internal);
  send_on_path(datagram);
  return {};
}

auto UdpTransportFactory::UdpSpokeTransport::export_secret(
    std::string_view label, std::span<const std::byte> context,
    std::span<std::byte> output) -> Result<void> {
  if (closed_ || failed_ || !established_ || !security_)
    return fail(Error::no_path);
  return security_->export_secret(label, context, output);
}

void UdpTransportFactory::UdpSpokeTransport::close() noexcept {
  if (closed_) return;
  if (!failed_ && established_ && security_ && agent_ && agent_->consent()) {
    security_->close();
    flush_security();
  }
  closed_ = true;
  release();
  pending_.clear();
}

auto UdpTransportFactory::UdpSpokeTransport::path_failure(
    nat::ice::Error error) noexcept -> Error {
  switch (error) {
    case nat::ice::Error::protocol:
      return Error::protocol;
    case nat::ice::Error::no_path:
      return Error::no_path;
    case nat::ice::Error::crypto_unavailable:
      return Error::crypto_unavailable;
    case nat::ice::Error::internal:
      return Error::internal;
  }
  return Error::internal;
}

auto UdpTransportFactory::UdpSpokeTransport::surfaced(Pending pending)
    -> TransportEvent {
  if (auto* gathered = std::get_if<transport_event::Gathered>(&pending))
    return std::move(*gathered);
  if (std::holds_alternative<transport_event::Nominated>(pending))
    return transport_event::Nominated{};
  if (std::holds_alternative<transport_event::Established>(pending))
    return transport_event::Established{};
  if (auto* record = std::get_if<transport_event::Record>(&pending))
    return std::move(*record);
  if (auto* datagram = std::get_if<transport_event::Datagram>(&pending))
    return std::move(*datagram);
  return std::get<transport_event::Failed>(pending);
}

auto UdpTransportFactory::UdpSpokeTransport::permitted_local(
    const platform::IpAddress& address, bool ipv6) noexcept -> bool {
  return (address.is_ipv4() || ipv6) &&
         nat::ice::CandidatePolicy::permits(address);
}

auto UdpTransportFactory::UdpSpokeTransport::medium_rank(
    const platform::InterfaceInfo& info) noexcept -> unsigned {
  if (!info.lan_adapter) return 3;
  switch (info.medium) {
    case platform::InterfaceMedium::ethernet:
      return 0;
    case platform::InterfaceMedium::wireless:
      return 1;
    case platform::InterfaceMedium::unclassified:
      return 2;
  }
  return 2;
}

auto UdpTransportFactory::UdpSpokeTransport::select_addresses(
    platform::InterfacesProvider& interfaces, bool ipv6)
    -> std::vector<platform::IpAddress> {
  const auto listed = interfaces.enumerate();
  if (!listed) return {};
  struct Choice {
    platform::IpAddress address;
    unsigned rank{};
  };
  std::vector<Choice> choices;
  for (const auto& info : listed.value()) {
    if (!info.operational || info.loopback ||
        !permitted_local(info.address, ipv6))
      continue;
    const auto rank =
        medium_rank(info) * 2U + (info.address.is_ipv4() ? 0U : 1U);
    const auto known = std::find_if(
        choices.begin(), choices.end(),
        [&](const Choice& choice) { return choice.address == info.address; });
    if (known == choices.end())
      choices.push_back(Choice{info.address, rank});
    else
      known->rank = std::min(known->rank, rank);
  }
  std::stable_sort(choices.begin(), choices.end(),
                   [](const Choice& left, const Choice& right) {
                     return left.rank < right.rank;
                   });
  if (choices.size() > max_local_addresses) choices.resize(max_local_addresses);
  std::vector<platform::IpAddress> addresses;
  addresses.reserve(choices.size());
  for (const auto& choice : choices) addresses.push_back(choice.address);
  return addresses;
}

auto UdpTransportFactory::UdpSpokeTransport::normalise(
    platform::SocketEndpoint endpoint) noexcept -> platform::SocketEndpoint {
  endpoint.address = endpoint.address.unmapped_ipv4();
  return endpoint;
}

void UdpTransportFactory::UdpSpokeTransport::observe(
    std::uint64_t now_us) noexcept {
  now_us_ = std::max(now_us_, now_us);
}

auto UdpTransportFactory::UdpSpokeTransport::usable() const noexcept -> bool {
  return !closed_ && !failed_ && established_ && security_ && agent_ &&
         agent_->consent();
}

auto UdpTransportFactory::UdpSpokeTransport::bind_sockets()
    -> std::vector<nat::ice::Base> {
  std::vector<nat::ice::Base> bases;
  const auto capabilities = network_.capabilities();
  const auto addresses = select_addresses(network_.interfaces(), enable_ipv6_);
  for (std::size_t rank = 0; rank < addresses.size(); ++rank) {
    const auto& address = addresses[rank];
    auto created = network_.sockets().create_udp_socket(address);
    if (!created || !created.value()) continue;
    auto socket = std::move(created).value();
    if (!socket->bind(platform::SocketEndpoint{address, 0})) {
      static_cast<void>(socket->close());
      continue;
    }
    const auto local = socket->local_endpoint();
    if (!local || local.value().port == 0) {
      static_cast<void>(socket->close());
      continue;
    }
    const bool dont_fragment = address.is_ipv4()
                                   ? capabilities.socket_dont_fragment_ipv4
                                   : capabilities.socket_dont_fragment_ipv6;
    if (dont_fragment)
      static_cast<void>(socket->set_boolean_option(
          platform::SocketOption::dont_fragment, true));
    static_cast<void>(
        socket->set_receive_buffer_size(socket_receive_buffer_bytes));
    const auto id = static_cast<nat::ice::SocketId>(sockets_.size() + 1);
    bases.push_back(nat::ice::Base{
        id, platform::SocketEndpoint{address, local.value().port},
        static_cast<std::uint16_t>(max_local_addresses - rank)});
    registry_->attach(*socket);
    sockets_.push_back(Bound{id, std::move(socket)});
  }
  return bases;
}

void UdpTransportFactory::UdpSpokeTransport::read_sockets() {
  backlog_ = false;
  std::size_t budget = max_datagrams_per_pump;
  const auto count = sockets_.size();
  for (std::size_t offset = 0; offset < count && !failed_ && !backlog_;
       ++offset) {
    const auto index = (next_socket_ + offset) % count;
    while (!failed_) {
      if (budget == 0) {
        backlog_ = true;
        break;
      }
      const auto received = sockets_[index].socket->receive_from(buffer_);
      if (!received) {
        const auto error = received.error();
        if (error != platform::NetError::connection_reset &&
            error != platform::NetError::connection_refused)
          break;
        --budget;
        continue;
      }
      --budget;
      const auto size = received.value().size;
      if (size == 0 || size > max_datagram_size) continue;
      on_datagram(sockets_[index].id, normalise(received.value().source),
                  std::span<const std::byte>{buffer_}.first(size));
    }
  }
  if (!failed_ && count != 0) next_socket_ = (next_socket_ + 1) % count;
}

void UdpTransportFactory::UdpSpokeTransport::on_datagram(
    nat::ice::SocketId socket, const platform::SocketEndpoint& source,
    std::span<const std::byte> bytes) {
  if (nat::stun::Message::recognises(bytes)) {
    outgoing_.clear();
    agent_->receive(socket, source, bytes, now_us_, outgoing_);
    transmit();
    drain_agent();
    return;
  }
  const auto path = agent_->nominated();
  if (!path || !agent_->consent() || path->socket != socket ||
      path->remote != source)
    return;
  if (std::ranges::starts_with(bytes, Hash::bytes_of(media_prefix))) {
    if (established_)
      pending_.push_back(transport_event::Datagram{
          std::vector<std::byte>{bytes.begin(), bytes.end()}});
    return;
  }
  if (securing_) after_security(security_->receive(bytes, now_us_));
}

void UdpTransportFactory::UdpSpokeTransport::drive_timers() {
  if (failed_ || !agent_) return;
  if (const auto deadline = agent_->next_deadline_us();
      deadline && now_us_ >= *deadline) {
    outgoing_.clear();
    agent_->poll(now_us_, outgoing_);
    transmit();
    drain_agent();
  }
  if (failed_ || !securing_) return;
  if (const auto deadline = security_->next_deadline_us();
      deadline && now_us_ >= *deadline)
    after_security(security_->poll(now_us_));
}

void UdpTransportFactory::UdpSpokeTransport::drain_agent() {
  while (!failed_ && agent_) {
    auto event = agent_->take_event();
    if (!event) return;
    if (auto* gathered = std::get_if<nat::ice::event::Gathered>(&*event)) {
      gathered_ = true;
      pending_.push_back(transport_event::Gathered{TransportDescription{
          std::move(gathered->local), security_->certificate_digest()}});
    } else if (std::holds_alternative<nat::ice::event::Nominated>(*event)) {
      on_nominated();
    } else if (const auto* failed =
                   std::get_if<nat::ice::event::Failed>(&*event)) {
      stop(path_failure(failed->error));
    }
  }
}

void UdpTransportFactory::UdpSpokeTransport::on_nominated() {
  if (securing_) return;
  if (!connected_) return stop(Error::internal);
  pending_.push_back(transport_event::Nominated{});
  securing_ = true;
  after_security(security_->start(remote_digest_, now_us_));
}

void UdpTransportFactory::UdpSpokeTransport::after_security(
    const Result<void>& result) {
  if (!result) return stop(result.error());
  flush_security();
  if (!established_ && security_->established()) {
    established_ = true;
    pending_.push_back(transport_event::Established{});
  }
  while (auto record = security_->take_record())
    pending_.push_back(transport_event::Record{std::move(*record)});
  if (security_->state() == SecurityState::closed) stop(Error::no_path);
}

void UdpTransportFactory::UdpSpokeTransport::flush_security() {
  while (auto datagram = security_->take_datagram()) send_on_path(*datagram);
}

void UdpTransportFactory::UdpSpokeTransport::send_on_path(
    std::span<const std::byte> bytes) {
  if (!agent_ || bytes.empty() || bytes.size() > max_datagram_size) return;
  const auto path = agent_->nominated();
  if (!path || !agent_->consent()) return;
  if (auto* socket = find_socket(path->socket))
    static_cast<void>(socket->send_to(bytes, path->remote));
}

void UdpTransportFactory::UdpSpokeTransport::transmit() {
  for (auto& packet : outgoing_) {
    if (auto* socket = find_socket(packet.socket))
      static_cast<void>(socket->send_to(packet.bytes, packet.remote));
  }
  outgoing_.clear();
}

auto UdpTransportFactory::UdpSpokeTransport::find_socket(
    nat::ice::SocketId id) noexcept -> platform::UdpSocket* {
  for (auto& bound : sockets_) {
    if (bound.id == id) return bound.socket.get();
  }
  return nullptr;
}

void UdpTransportFactory::UdpSpokeTransport::stop(Error error) {
  if (failed_ || closed_) return;
  if (security_ && agent_) flush_security();
  failed_ = true;
  pending_.push_back(transport_event::Failed{error});
  release();
}

auto UdpTransportFactory::UdpSpokeTransport::abandon(Error error)
    -> Result<void> {
  failed_ = true;
  release();
  return fail(error);
}

void UdpTransportFactory::UdpSpokeTransport::release() noexcept {
  if (security_) {
    security_->close();
    while (security_->take_datagram()) {
    }
    security_.reset();
  }
  if (agent_) {
    agent_->close();
    agent_.reset();
  }
  for (auto& bound : sockets_) {
    registry_->detach(*bound.socket);
    static_cast<void>(bound.socket->close());
  }
  sockets_.clear();
  outgoing_.clear();
  remote_digest_ = {};
  buffer_ = {};
  securing_ = false;
  established_ = false;
  backlog_ = false;
}

}  // namespace libchiaking::net::kakusan
