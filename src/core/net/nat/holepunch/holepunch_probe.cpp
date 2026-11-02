#include "src/core/net/nat/holepunch/holepunch_probe.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include "src/core/utils/timing/deadline.hpp"

#include <algorithm>
#include <functional>

namespace libchiaking::net::nat::holepunch {

auto ProbeCodec::classify(
    std::span<const std::byte> packet,
    const std::array<std::byte, holepunch_request_id_size>&
        outstanding_request_id) -> ProbeDatagramKind {
  using Kind = ProbeDatagramKind;
  if (packet.size() != CandidateProbe::kEncodedSize) return Kind::invalid_size;
  const auto type = bytes::load_be<std::uint32_t>(packet);
  if (type == 0x06000000U) return Kind::incoming_request;
  if (type != 0x07000000U) return Kind::invalid_type;
  if (!std::ranges::equal(
          packet.subspan(request_id_offset, outstanding_request_id.size()),
          outstanding_request_id))
    return Kind::stale_response_id;
  return Kind::matching_response;
}

auto ProbeCodec::encode(const CandidateProbe& probe, std::byte type)
    -> std::array<std::byte, CandidateProbe::kEncodedSize> {
  std::array<std::byte, CandidateProbe::kEncodedSize> packet{};
  const auto view = std::span{packet};
  view[0] = type;
  std::ranges::copy(probe.local_hashed_id,
                    view.subspan(local_hash_offset).begin());
  std::ranges::copy(probe.console_hashed_id,
                    view.subspan(console_hash_offset).begin());
  bytes::store_be(view.subspan(local_sid_offset), probe.local_sid);
  bytes::store_be(view.subspan(console_sid_offset), probe.console_sid);
  std::ranges::copy(probe.request_id, view.subspan(request_id_offset).begin());
  return packet;
}

auto ProbeCodec::encode(const CandidateProbe& probe)
    -> std::array<std::byte, CandidateProbe::kEncodedSize> {
  return encode(probe, request_type);
}

auto ProbeCodec::encode_response(const CandidateProbe& probe,
                                 const platform::SocketEndpoint& peer)
    -> std::array<std::byte, CandidateProbe::kEncodedSize> {
  auto packet = encode(probe, response_type);
  const auto echo = std::span{packet}.subspan(peer_echo_offset, 6);
  bytes::store_be(echo, probe.local_sid);
  bytes::store_be(echo.subspan(2), probe.console_sid);
  bytes::store_be(echo.subspan(4), probe.local_sid);
  std::array<std::byte, 6> mask{};
  std::ranges::copy(peer.address.bytes().first(4), mask.begin());
  bytes::store_be(std::span{mask}.subspan(4), peer.port);
  std::ranges::transform(echo, mask, echo.begin(), std::bit_xor<>{});
  return packet;
}

auto ProbeCodec::decode(std::span<const std::byte> packet)
    -> Expected<CandidateProbe, ProbeCodecError> {
  if (packet.size() != CandidateProbe::kEncodedSize)
    return unexpected(ProbeCodecError::bad_size);
  if (bytes::load_be<std::uint32_t>(packet) != 0x06000000U)
    return unexpected(ProbeCodecError::bad_type);

  CandidateProbe probe;
  std::ranges::copy(
      packet.subspan(local_hash_offset, probe.local_hashed_id.size()),
      probe.local_hashed_id.begin());
  std::ranges::copy(
      packet.subspan(console_hash_offset, probe.console_hashed_id.size()),
      probe.console_hashed_id.begin());
  probe.local_sid =
      bytes::load_be<std::uint16_t>(packet.subspan(local_sid_offset));
  probe.console_sid =
      bytes::load_be<std::uint16_t>(packet.subspan(console_sid_offset));
  std::ranges::copy(packet.subspan(request_id_offset, probe.request_id.size()),
                    probe.request_id.begin());
  return probe;
}

auto Probe::stopped(const platform::StopToken& stop_token) noexcept
    -> ProbeWaitError {
  return stop_token.deadline_expired() ? ProbeWaitError::timeout
                                       : ProbeWaitError::cancelled;
}

auto Probe::peer_probe_error(ProbeWaitError error) noexcept -> PeerProbeError {
  switch (error) {
    case ProbeWaitError::cancelled:
      return PeerProbeError::cancelled;
    case ProbeWaitError::timeout:
      return PeerProbeError::wait_timeout;
    case ProbeWaitError::net:
      return PeerProbeError::wait_net;
    case ProbeWaitError::protocol:
      return PeerProbeError::wait_protocol;
  }
  return PeerProbeError::wait_protocol;
}

auto Probe::make_request(const ProbeRequestInput& input,
                         platform::SecureRandomProvider& random)
    -> Expected<ProbeRequest, ProbeRequestError> {
  if (input.local_sid > UINT16_MAX)
    return unexpected(ProbeRequestError::local_sid_out_of_range);
  if (input.console_sid > UINT16_MAX)
    return unexpected(ProbeRequestError::console_sid_out_of_range);

  CandidateProbe profile{};
  profile.local_hashed_id = input.local_hashed_id;
  profile.console_hashed_id = input.console_hashed_id;
  profile.local_sid = static_cast<std::uint16_t>(input.local_sid);
  profile.console_sid = static_cast<std::uint16_t>(input.console_sid);
  if (!random.fill_bytes(profile.request_id))
    return unexpected(ProbeRequestError::random_failed);
  return ProbeRequest{ProbeCodec::encode(profile), profile.request_id, profile};
}

auto Probe::wait_for_response_from_groups(
    std::span<const ProbeSocketGroup> groups, platform::Poller& poller,
    const platform::ClockProvider& clock, std::span<const std::byte> request,
    const std::array<std::byte, holepunch_request_id_size>& expected_request_id,
    const CandidateProbe& response_profile,
    const platform::StopToken& stop_token, std::chrono::nanoseconds timeout,
    bool* prior_answered, ProbeDiscoveryState* prior_discovery,
    bool send_nat_probes) -> Expected<ProbeSocketPeerMatch, ProbeWaitError> {
  using platform::PollEvent;
  using platform::PollRequest;
  if (stop_token.stop_requested()) return unexpected(stopped(stop_token));
  if (groups.empty() || request.size() != CandidateProbe::kEncodedSize)
    return unexpected(ProbeWaitError::protocol);
  std::vector<PollRequest> requests;
  requests.reserve(groups.size());
  std::vector<platform::IpFamily> families;
  families.reserve(groups.size());
  for (std::size_t index = 0; index < groups.size(); ++index) {
    const auto& group = groups[index];
    if (!group.socket || group.peers.empty() ||
        &group.socket->owner() != &poller.owner())
      return unexpected(ProbeWaitError::protocol);
    for (std::size_t earlier = 0; earlier < index; ++earlier) {
      if (groups[earlier].socket == group.socket)
        return unexpected(ProbeWaitError::protocol);
    }
    const auto local = group.socket->local_endpoint();
    if (!local) return unexpected(ProbeWaitError::net);
    families.push_back(local.value().address.family());
    for (const auto& peer : group.peers) {
      if (peer.port == 0 || peer.address.is_unspecified() ||
          peer.address.family() != local.value().address.family())
        return unexpected(ProbeWaitError::protocol);
    }
    requests.push_back(PollRequest{.pollable = group.socket,
                                   .tag = index + 1U,
                                   .interest = PollEvent::readable});
  }
  bool sent_any = false;
  for (const auto& group : groups) {
    if (group.nat_probe && !send_nat_probes) continue;
    for (const auto& peer : group.peers) {
      if (stop_token.stop_requested()) return unexpected(stopped(stop_token));
      const auto sent = group.socket->send_to(request, peer);
      sent_any = sent_any || (sent && sent.value() == request.size());
    }
  }
  if (!send_nat_probes && prior_discovery) {
    for (const auto& peer : prior_discovery->derived) {
      for (std::size_t index = 0; index < groups.size(); ++index) {
        const auto& group = groups[index];
        if (group.nat_probe || families[index] != peer.address.family())
          continue;
        if (std::find(group.peers.begin(), group.peers.end(), peer) ==
            group.peers.end()) {
          if (stop_token.stop_requested())
            return unexpected(stopped(stop_token));
          static_cast<void>(group.socket->send_to(request, peer));
        }
        break;
      }
    }
  }
  if (!sent_any && send_nat_probes) return unexpected(ProbeWaitError::net);
  const auto timeout_us =
      std::chrono::duration_cast<std::chrono::microseconds>(timeout);
  if (timeout_us.count() <= 0) return unexpected(ProbeWaitError::timeout);
  const std::uint64_t duration_us =
      static_cast<std::uint64_t>(timeout_us.count());
  const auto rearm = [&] {
    return timing::Deadline::after_us(clock.now_microseconds(), duration_us);
  };
  std::uint64_t deadline_us = rearm();
  std::array<std::byte, CandidateProbe::kEncodedSize + 1> buffer{};
  ProbeDiscoveryState local_discovery;
  auto& discovery = prior_discovery ? *prior_discovery : local_discovery;
  bool local_answered = false;
  bool& answered_peer_request =
      prior_answered ? *prior_answered : local_answered;
  for (;;) {
    if (stop_token.stop_requested()) return unexpected(stopped(stop_token));
    const std::uint64_t now_us = clock.now_microseconds();
    if (now_us >= deadline_us) return unexpected(ProbeWaitError::timeout);
    const auto remaining = std::chrono::microseconds(deadline_us - now_us);
    const auto ready = poller.wait(
        requests,
        std::chrono::duration_cast<std::chrono::nanoseconds>(remaining),
        stop_token);
    if (!ready) return unexpected(ProbeWaitError::net);
    if (ready.value().stop_requested) return unexpected(stopped(stop_token));
    bool woke = false;
    for (const auto& event : ready.value().ready) {
      if (event.tag == 0 || event.tag > groups.size() ||
          (event.ready & PollEvent::readable) == PollEvent::none)
        continue;
      const auto index = static_cast<std::size_t>(event.tag - 1U);
      const auto& group = groups[index];
      if (group.nat_probe &&
          !group.socket->set_ipv4_ttl(holepunch_nat_probe_ttl))
        return unexpected(ProbeWaitError::net);
      const auto received = group.socket->receive_from(buffer);
      if (!received) continue;
      woke = true;
      if (received.value().size > buffer.size())
        return unexpected(ProbeWaitError::protocol);
      const auto known = std::find(group.peers.begin(), group.peers.end(),
                                   received.value().source);
      const auto& peer = received.value().source;
      const bool derived = known == group.peers.end();
      if (derived) {
        if (peer.port == 0 || peer.address.is_unspecified()) continue;
        auto existing =
            std::find(discovery.derived.begin(), discovery.derived.end(), peer);
        if (existing == discovery.derived.end()) {
          if (discovery.derived.size() == 3U) continue;
          discovery.derived.push_back(peer);
        }
      }
      const auto kind = ProbeCodec::classify(
          std::span<const std::byte>(buffer.data(), received.value().size),
          expected_request_id);
      if (kind == ProbeDatagramKind::matching_response)
        return ProbeSocketPeerMatch{index, peer, answered_peer_request,
                                    derived};
      if (kind == ProbeDatagramKind::stale_response_id) continue;
      if (kind == ProbeDatagramKind::incoming_request) {
        const auto decoded = ProbeCodec::decode(
            std::span<const std::byte>(buffer.data(), received.value().size));
        if (!decoded) return unexpected(ProbeWaitError::protocol);
        auto response_probe = response_profile;
        response_probe.request_id = decoded.value().request_id;
        const auto response = ProbeCodec::encode_response(response_probe, peer);
        const auto sent = group.socket->send_to(response, peer);
        if ((!sent || sent.value() != response.size()) && !derived)
          return unexpected(ProbeWaitError::net);
        if (derived || group.nat_probe || group.reciprocal) {
          const auto reciprocal = group.socket->send_to(request, peer);
          if (!reciprocal || reciprocal.value() != request.size())
            return unexpected(ProbeWaitError::net);
        }
        answered_peer_request = true;
        continue;
      }
      if (!derived) return unexpected(ProbeWaitError::protocol);
    }
    if (woke) deadline_us = rearm();
  }
}

auto Probe::answer_candidate_requests(platform::UdpSocket& socket,
                                      platform::Poller& poller,
                                      const platform::SocketEndpoint& peer,
                                      const CandidateProbe& response_profile,
                                      const platform::StopToken& stop_token,
                                      bool already_answered)
    -> Expected<void, ProbeWaitError> {
  const platform::PollRequest request{
      .pollable = &socket, .tag = 1, .interest = platform::PollEvent::readable};
  std::array<std::byte, CandidateProbe::kEncodedSize + 1> buffer{};
  for (;;) {
    if (stop_token.stop_requested()) return unexpected(stopped(stop_token));
    auto ready = poller.wait(std::span{&request, 1}, holepunch_response_wait,
                             stop_token);
    if (!ready) return unexpected(ProbeWaitError::net);
    if (ready.value().stop_requested) return unexpected(stopped(stop_token));
    if (ready.value().ready.empty()) {
      if (ready.value().woke) continue;
      if (already_answered) return {};
      return unexpected(ProbeWaitError::timeout);
    }
    for (const auto& event : ready.value().ready) {
      if (event.tag != 1 || (event.ready & platform::PollEvent::readable) ==
                                platform::PollEvent::none)
        continue;
      auto received = socket.receive_from(buffer);
      if (!received) continue;
      if (received.value().source != peer) continue;
      const auto size = received.value().size;
      if (size > buffer.size()) return unexpected(ProbeWaitError::protocol);
      const auto packet = std::span<const std::byte>{buffer.data(), size};
      const auto kind =
          ProbeCodec::classify(packet, response_profile.request_id);
      if (kind == ProbeDatagramKind::matching_response ||
          kind == ProbeDatagramKind::stale_response_id)
        continue;
      if (kind != ProbeDatagramKind::incoming_request)
        return unexpected(ProbeWaitError::protocol);
      auto decoded = ProbeCodec::decode(packet);
      if (!decoded) return unexpected(ProbeWaitError::protocol);
      auto response = response_profile;
      response.request_id = decoded.value().request_id;
      auto bytes = ProbeCodec::encode_response(response, peer);
      auto sent = socket.send_to(bytes, peer);
      if (!sent || sent.value() != bytes.size())
        return unexpected(ProbeWaitError::net);
      already_answered = true;
    }
  }
}

auto Probe::wait_for_response_from_groups_with_retries(
    std::span<const ProbeSocketGroup> groups, platform::Poller& poller,
    const platform::ClockProvider& clock, std::span<const std::byte> request,
    const std::array<std::byte, holepunch_request_id_size>& expected_request_id,
    const CandidateProbe& response_profile,
    const platform::StopToken& stop_token, ProbeRetryPolicy policy)
    -> Expected<ProbeSocketPeerMatch, ProbeWaitError> {
  bool answered_peer_request = false;
  ProbeDiscoveryState discovery;
  for (unsigned attempt = 0; attempt <= policy.max_retries; ++attempt) {
    if (stop_token.stop_requested()) return unexpected(stopped(stop_token));
    const auto result = wait_for_response_from_groups(
        groups, poller, clock, request, expected_request_id, response_profile,
        stop_token, policy.attempt_timeout, &answered_peer_request, &discovery,
        attempt == 0);
    if (result || result.error() != ProbeWaitError::timeout) return result;
  }
  return unexpected(ProbeWaitError::timeout);
}

auto BoundSockets::create(platform::SocketProvider& sockets, bool enable_ipv6)
    -> Expected<BoundSockets, BoundSocketsError> {
  auto ipv4 = sockets.create_udp_socket(platform::IpAddress::any_ipv4());
  if (!ipv4 || !ipv4.value()) return unexpected(BoundSocketsError::ipv4_create);
  if (!ipv4.value()->set_shared_port_binding(true))
    return unexpected(BoundSocketsError::ipv4_shared_port);
  if (!ipv4.value()->bind({platform::IpAddress::any_ipv4(), 0}))
    return unexpected(BoundSocketsError::ipv4_bind);
  const auto local = ipv4.value()->local_endpoint();
  if (!local || !local.value().address.is_ipv4() || local.value().port == 0)
    return unexpected(BoundSocketsError::ipv4_endpoint);

  BoundSockets result{};
  result.local_port = local.value().port;
  result.ipv4 = std::move(ipv4.value());
  if (enable_ipv6) {
    auto ipv6 = sockets.create_udp_socket(platform::IpAddress::any_ipv6());
    if (ipv6 && ipv6.value() && ipv6.value()->set_shared_port_binding(true) &&
        ipv6.value()->bind(
            {platform::IpAddress::any_ipv6(), result.local_port})) {
      const auto bound = ipv6.value()->local_endpoint();
      if (bound && bound.value().address.is_ipv6() &&
          bound.value().port == result.local_port)
        result.ipv6 = std::move(ipv6.value());
    }
  }
  return result;
}

void BoundSockets::add_nat_sockets(BoundSockets& bound,
                                   platform::SocketProvider& provider,
                                   std::size_t count,
                                   const platform::StopToken& stop) {
  bound.random_allocation = true;
  for (std::size_t i = 0; i < count && !stop.stop_requested(); ++i) {
    auto socket = provider.create_udp_socket(platform::IpAddress::any_ipv4());
    if (!socket || !socket.value()) break;
    if (!socket.value()->set_shared_port_binding(true) ||
        !socket.value()->set_ipv4_ttl(2) ||
        !socket.value()->bind({platform::IpAddress::any_ipv4(), 0}))
      continue;
    bound.probes.push_back(std::move(socket.value()));
  }
}

bool ProbeGroupPlan::empty() const noexcept {
  return ipv4_peers_.empty() && ipv6_peers_.empty();
}

ProbeGroupPlan::ProbeGroupPlan(
    const BoundSockets& sockets,
    std::span<const ResolvedCandidateEndpoint> resolved,
    std::span<const Candidate> candidates)
    : ipv4_socket_(sockets.ipv4.get()),
      ipv6_socket_(sockets.ipv6.get()),
      reciprocal_(sockets.random_allocation) {
  for (const auto& item : resolved) {
    if (item.endpoint.address.is_ipv4() && sockets.ipv4) {
      ipv4_peers_.push_back(item.endpoint);
      ipv4_sources_.push_back(item.candidate_index);
      if (item.candidate_index < candidates.size() &&
          (candidates[item.candidate_index].type == CandidateType::stun ||
           candidates[item.candidate_index].type == CandidateType::static_)) {
        nat_peers_.push_back(item.endpoint);
        nat_sources_.push_back(item.candidate_index);
      }
    } else if (item.endpoint.address.is_ipv6() && sockets.ipv6) {
      ipv6_peers_.push_back(item.endpoint);
      ipv6_sources_.push_back(item.candidate_index);
    }
  }
  for (const auto& socket : sockets.probes)
    if (socket && !nat_peers_.empty()) nat_sockets_.push_back(socket.get());
}

auto ProbeGroupPlan::groups() const -> std::vector<ProbeSocketGroup> {
  std::vector<ProbeSocketGroup> result;
  result.reserve(2U);
  if (!ipv4_peers_.empty())
    result.push_back({ipv4_socket_, ipv4_peers_, false, reciprocal_});
  if (!ipv6_peers_.empty())
    result.push_back({ipv6_socket_, ipv6_peers_, false, reciprocal_});
  for (auto* socket : nat_sockets_)
    result.push_back({socket, nat_peers_, true, true});
  return result;
}

auto ProbeGroupPlan::source_index(const ProbeSocketPeerMatch& match) const
    -> std::optional<std::size_t> {
  if (!ipv4_peers_.empty() && match.group_index == 0U)
    return find_source(ipv4_peers_, ipv4_sources_, match.peer);
  const std::size_t ipv6_group = ipv4_peers_.empty() ? 0U : 1U;
  if (!ipv6_peers_.empty() && match.group_index == ipv6_group)
    return find_source(ipv6_peers_, ipv6_sources_, match.peer);
  const auto base_count = static_cast<std::size_t>(!ipv4_peers_.empty()) +
                          static_cast<std::size_t>(!ipv6_peers_.empty());
  if (match.group_index >= base_count &&
      match.group_index - base_count < nat_sockets_.size())
    return find_source(nat_peers_, nat_sources_, match.peer);
  return std::nullopt;
}

auto ProbeGroupPlan::find_source(
    const std::vector<platform::SocketEndpoint>& peers,
    const std::vector<std::size_t>& indices,
    const platform::SocketEndpoint& peer) -> std::optional<std::size_t> {
  for (std::size_t index = 0; index < peers.size(); ++index) {
    if (peers[index] == peer) return indices[index];
  }
  return std::nullopt;
}

auto ProbeGroupPlan::make(const BoundSockets& sockets,
                          std::span<const ResolvedCandidateEndpoint> resolved,
                          std::span<const Candidate> candidates)
    -> std::optional<ProbeGroupPlan> {
  ProbeGroupPlan plan{sockets, resolved, candidates};
  if (plan.empty()) return std::nullopt;
  return plan;
}

auto Probe::select_socket(BoundSockets sockets, const ProbeGroupPlan& plan,
                          const ProbeSocketPeerMatch& match)
    -> Expected<SelectedCandidate, SelectSocketError> {
  const auto groups = plan.groups();
  if (match.group_index >= groups.size())
    return unexpected(SelectSocketError::group_out_of_range);
  const auto candidate_index = plan.source_index(match);
  if (!candidate_index && !match.derived)
    return unexpected(SelectSocketError::unknown_peer);
  auto* winner = groups[match.group_index].socket;
  std::unique_ptr<platform::UdpSocket> owned;
  if (winner == sockets.ipv4.get())
    owned = std::move(sockets.ipv4);
  else if (winner == sockets.ipv6.get())
    owned = std::move(sockets.ipv6);
  else {
    for (auto& socket : sockets.probes)
      if (socket.get() == winner) {
        owned = std::move(socket);
        break;
      }
  }
  if (!owned) return unexpected(SelectSocketError::socket_mismatch);
  return SelectedCandidate{std::move(owned), match.peer,
                           candidate_index.value_or(SIZE_MAX), match.derived};
}

auto Probe::probe_peer(BoundSockets sockets, platform::DnsProvider& dns,
                       platform::Poller& poller,
                       const platform::ClockProvider& clock,
                       platform::SecureRandomProvider& random,
                       const platform::StopToken& stop_token,
                       const PeerProbeInput& input)
    -> Expected<SelectedCandidate, PeerProbeError> {
  if (stop_token.stop_requested()) return unexpected(PeerProbeError::cancelled);
  auto resolved =
      CandidateGathering::resolve_endpoints(input.candidates, dns, stop_token);
  if (!resolved) return unexpected(PeerProbeError::no_candidates);
  if (stop_token.stop_requested()) return unexpected(PeerProbeError::cancelled);
  auto plan = ProbeGroupPlan::make(sockets, resolved.value(), input.candidates);
  if (!plan) return unexpected(PeerProbeError::no_candidates);
  auto request = make_request(input.probe, random);
  if (!request) {
    if (request.error() == ProbeRequestError::random_failed)
      return unexpected(PeerProbeError::random_failed);
    return unexpected(PeerProbeError::invalid_sid);
  }
  if (stop_token.stop_requested()) return unexpected(PeerProbeError::cancelled);
  auto groups = plan.value().groups();
  auto match = wait_for_response_from_groups_with_retries(
      groups, poller, clock, request.value().packet, request.value().request_id,
      request.value().response_profile, stop_token, input.retry);
  if (!match) return unexpected(peer_probe_error(match.error()));
  auto answered = answer_candidate_requests(
      *groups[match.value().group_index].socket, poller, match.value().peer,
      request.value().response_profile, stop_token,
      match.value().answered_peer_request);
  if (!answered) return unexpected(peer_probe_error(answered.error()));
  if (stop_token.stop_requested()) return unexpected(PeerProbeError::cancelled);
  auto selected =
      select_socket(std::move(sockets), plan.value(), match.value());
  if (!selected) return unexpected(PeerProbeError::selection_failed);
  return std::move(selected.value());
}

auto Probe::make_peer_probe_input(const ConnRequest& ours,
                                  const ConnRequest& peer)
    -> Expected<PeerProbeInput, PeerProbeInputError> {
  constexpr auto valid_sid = [](std::uint32_t sid) noexcept {
    return sid <= UINT16_MAX;
  };
  if (!valid_sid(ours.sid))
    return unexpected(PeerProbeInputError::invalid_local_offer);
  if (!valid_sid(peer.sid) || peer.candidates.empty())
    return unexpected(PeerProbeInputError::invalid_peer_offer);

  PeerProbeInput out{};
  out.candidates = peer.candidates;
  out.probe.local_sid = ours.sid;
  out.probe.console_sid = peer.sid;
  std::copy(ours.local_hashed_id.begin(), ours.local_hashed_id.end(),
            out.probe.local_hashed_id.begin());
  std::copy(peer.local_hashed_id.begin(), peer.local_hashed_id.end(),
            out.probe.console_hashed_id.begin());
  return out;
}

}  // namespace libchiaking::net::nat::holepunch
