#include "src/core/net/nat/holepunch/holepunch_candidate.hpp"

#include "libchiaking/platform/net/types.hpp"
#include "src/core/net/nat/holepunch/limits.hpp"
#include "src/core/net/ports.hpp"

#include <algorithm>

namespace libchiaking::net::nat::holepunch {

CandidateType CandidateCodec::classify_type(std::string_view type) {
  if (type == "LOCAL") return CandidateType::local;
  if (type == "STUN") return CandidateType::stun;
  if (type == "DERIVED") return CandidateType::derived;
  return CandidateType::static_;
}

bool CandidateCodec::addr_valid(std::string_view value) {
  return value.size() <= 45U && value.find('\0') == std::string_view::npos;
}

auto CandidateCodec::decode(const CandidateWire& root)
    -> std::optional<Candidate> {
  if (!root.type || !root.addr || !root.mapped_addr || !root.port ||
      !root.mapped_port)
    return std::nullopt;
  const auto port_value = MessageSchemaCodec::wrapped<std::uint16_t>(root.port);
  const auto mapped_port_value =
      MessageSchemaCodec::wrapped<std::uint16_t>(root.mapped_port);
  if (!addr_valid(*root.addr) || !addr_valid(*root.mapped_addr) ||
      !port_value || !mapped_port_value)
    return std::nullopt;
  return Candidate{classify_type(*root.type), *root.addr, *root.mapped_addr,
                   *port_value, *mapped_port_value};
}

auto CandidateCodec::decode_list(
    const std::optional<std::vector<CandidateWire>>& listed)
    -> std::optional<std::vector<Candidate>> {
  if (!listed || listed->size() > max_count) return std::nullopt;
  std::vector<Candidate> candidates;
  candidates.reserve(listed->size());
  for (const auto& element : *listed) {
    auto decoded = decode(element);
    if (!decoded) return std::nullopt;
    candidates.push_back(std::move(*decoded));
  }
  return candidates;
}

auto CandidateCodec::ascii_valid(std::string_view value) noexcept -> bool {
  return std::ranges::all_of(
      value, [](char ch) { return static_cast<unsigned char>(ch) <= 0x7fU; });
}

auto CandidateCodec::type_name(CandidateType type) noexcept
    -> std::string_view {
  switch (type) {
    case CandidateType::local:
      return "LOCAL";
    case CandidateType::stun:
      return "STUN";
    case CandidateType::derived:
      return "DERIVED";
    case CandidateType::static_:
      return "STATIC";
  }
  return {};
}

auto CandidateCodec::encode(const Candidate& candidate)
    -> Expected<CandidateEntry, CandidateEncodeError> {
  if (!addr_valid(candidate.addr) || !addr_valid(candidate.mapped_addr) ||
      !ascii_valid(candidate.addr) || !ascii_valid(candidate.mapped_addr))
    return unexpected(CandidateEncodeError::invalid_address);
  const auto type_name = CandidateCodec::type_name(candidate.type);
  if (type_name.empty()) return unexpected(CandidateEncodeError::invalid_type);
  return CandidateEntry{type_name, candidate.addr, candidate.mapped_addr,
                        candidate.port, candidate.mapped_port};
}

auto CandidateCodec::encode_list(std::span<const Candidate> candidates)
    -> Expected<std::vector<CandidateEntry>, CandidateListEncodeError> {
  if (candidates.size() > max_count)
    return unexpected(CandidateListEncodeError::too_many);
  std::vector<CandidateEntry> list;
  std::size_t written = 1U;
  for (const auto& candidate : candidates) {
    auto element = encode(candidate);
    if (!element)
      return unexpected(CandidateListEncodeError::invalid_candidate);
    const std::size_t delimiter = list.empty() ? 0U : 1U;
    const auto spelled =
        json::ps::write<json::ps::body_options_with_nulls>(element.value());
    if (!spelled)
      return unexpected(CandidateListEncodeError::invalid_candidate);
    const auto size = spelled.value().size();
    if (size + delimiter + 1U > max_list_bytes - written)
      return unexpected(CandidateListEncodeError::too_large);
    written += delimiter + size;
    list.push_back(std::move(element.value()));
  }
  return list;
}

auto CandidateGathering::resolve_endpoints(
    const std::vector<Candidate>& candidates, platform::DnsProvider& dns,
    const platform::StopToken& stop)
    -> std::optional<std::vector<ResolvedCandidateEndpoint>> {
  std::vector<ResolvedCandidateEndpoint> endpoints;
  endpoints.reserve(candidates.size());
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    if (stop.stop_requested()) break;
    if (candidates[index].port == 0) continue;
    auto resolved = dns.resolve(candidates[index].addr,
                                std::to_string(candidates[index].port), stop);
    if (!resolved) continue;
    const auto usable = std::ranges::find_if(
        resolved.value(), [](const platform::IpAddress& address) {
          return !address.is_unspecified();
        });
    if (usable != resolved.value().end())
      endpoints.push_back({index, {*usable, candidates[index].port}});
  }
  if (endpoints.empty()) return std::nullopt;
  return endpoints;
}

auto CandidateGathering::offered(CandidateType type,
                                 const platform::IpAddress& address,
                                 std::uint16_t port) -> Candidate {
  return Candidate{type, platform::to_string(address),
                   platform::to_string(platform::IpAddress::any_ipv4()), port,
                   0};
}

auto CandidateGathering::select_local(platform::InterfacesProvider& interfaces,
                                      std::uint16_t port)
    -> Expected<Candidate, LocalCandidateError> {
  if (port == 0) return unexpected(LocalCandidateError::zero_port);
  const auto result = interfaces.enumerate();
  if (!result) return unexpected(LocalCandidateError::provider_failure);
  const platform::InterfaceInfo* chosen = nullptr;
  for (const auto& info : result.value()) {
    if (!info.operational || info.loopback || !info.lan_adapter ||
        !info.address.is_ipv4() || info.address.is_unspecified() ||
        info.address.is_loopback())
      continue;
    if (info.medium != platform::InterfaceMedium::ethernet) {
      chosen = &info;
      break;
    }
    if (!chosen || chosen->index != info.index) chosen = &info;
  }
  if (!chosen) return unexpected(LocalCandidateError::no_usable_interface);
  return offered(CandidateType::local, chosen->address, port);
}

auto CandidateGathering::completed(std::vector<Candidate> candidates,
                                   const platform::IpAddress& public_address,
                                   const Candidate& local)
    -> std::vector<Candidate> {
  candidates.push_back(
      offered(CandidateType::static_, public_address, local.port));
  auto unmapped = local;
  unmapped.mapped_addr = platform::to_string(platform::IpAddress::any_ipv4());
  unmapped.mapped_port = 0;
  candidates.push_back(std::move(unmapped));
  return candidates;
}

auto CandidateGathering::mapped_offer(
    const Candidate& local, const platform::SocketEndpoint& public_mapping)
    -> std::vector<Candidate> {
  std::vector<Candidate> candidates;
  candidates.reserve(3U);
  if (public_mapping.port != local.port)
    candidates.push_back(offered(CandidateType::stun, public_mapping.address,
                                 public_mapping.port));
  return completed(std::move(candidates), public_mapping.address, local);
}

auto CandidateGathering::predicted_offer(
    const Candidate& local,
    const net::nat::stun::StunAllocationEstimate& allocation)
    -> Expected<std::vector<Candidate>, OfferCandidatesError> {
  if (allocation.increment < -max_port || allocation.increment > max_port)
    return unexpected(OfferCandidatesError::invalid_allocation);
  const auto& public_mapping = allocation.projected;
  std::vector<Candidate> candidates;
  candidates.reserve(holepunch_stun_candidate_count + 2U);
  auto port_check = static_cast<std::int32_t>(public_mapping.port);
  for (std::size_t index = 0; index < holepunch_stun_candidate_count; ++index) {
    const auto old = port_check;
    port_check += allocation.increment;
    if (port_check < holepunch_port_floor && old > holepunch_port_floor)
      port_check = max_port - (holepunch_port_floor - port_check);
    else if (port_check < 1)
      port_check += max_port;
    else if (port_check > max_port)
      port_check = port_check - max_port + holepunch_port_floor;
    candidates.push_back(offered(CandidateType::stun, public_mapping.address,
                                 static_cast<std::uint16_t>(port_check)));
  }
  return completed(std::move(candidates), public_mapping.address, local);
}

auto CandidateGathering::guessed_offer(
    const Candidate& local, const platform::SocketEndpoint& public_mapping,
    std::size_t guess_count)
    -> Expected<std::vector<Candidate>, OfferCandidatesError> {
  if (guess_count == 0 || guess_count > max_port_guess_count)
    return unexpected(OfferCandidatesError::invalid_guess_count);
  std::vector<Candidate> candidates;
  candidates.reserve(guess_count + 2U);
  const auto base = static_cast<std::int32_t>(public_mapping.port);
  for (std::size_t index = 0; index < guess_count; ++index) {
    const auto i = static_cast<std::int32_t>(index);
    const auto delta = i == 0 ? 0 : (i % 2 != 0 ? (i + 1) / 2 : -(i / 2));
    auto port = base + delta;
    if (port > max_port)
      port = holepunch_ephemeral_start + (port - max_port - 1);
    else if (port < holepunch_port_floor)
      port = max_port - (holepunch_port_floor - port);
    candidates.push_back(offered(CandidateType::stun, public_mapping.address,
                                 static_cast<std::uint16_t>(port)));
  }
  return completed(std::move(candidates), public_mapping.address, local);
}

auto CandidateGathering::assemble_offer(
    const Candidate& local,
    const net::nat::stun::StunAllocationEstimate& allocation,
    bool force_port_guessing, std::size_t guess_count)
    -> Expected<std::vector<Candidate>, OfferCandidatesError> {
  if (local.type != CandidateType::local || local.port == 0 ||
      local.addr.empty() || !CandidateCodec::addr_valid(local.addr))
    return unexpected(OfferCandidatesError::invalid_local_candidate);
  const auto& public_mapping = allocation.projected;
  if (!public_mapping.address.is_ipv4() ||
      public_mapping.address.is_unspecified() || public_mapping.port == 0)
    return unexpected(OfferCandidatesError::invalid_public_mapping);
  if (allocation.increment != 0 && !allocation.random_allocation)
    return predicted_offer(local, allocation);
  if (allocation.random_allocation ||
      (force_port_guessing && public_mapping.port != local.port))
    return guessed_offer(local, public_mapping, guess_count);
  return mapped_offer(local, public_mapping);
}

auto CandidateGathering::unmapped(
    const Candidate& local, net::nat::stun::StunAllocationDiscoveryError why,
    const diagnostics::Trace* trace)
    -> Expected<GatheredCandidates, GatherError> {
  const auto address = platform::parse_ip_address(local.addr);
  if (!address) return unexpected(GatherError::local);
  const net::nat::stun::StunAllocationEstimate allocation{
      {address.value(), local.port}, 0, false};
  auto candidates = assemble_offer(local, allocation);
  if (!candidates) return unexpected(GatherError::candidate);
  if (trace)
    trace->decision(diagnostics::Trace::Decision::unmapped_offer,
                    static_cast<unsigned>(why));
  return GatheredCandidates{local, allocation, std::move(candidates.value())};
}

auto CandidateGathering::gather(const GatherRequest& request,
                                const platform::StopToken& stop,
                                net::nat::stun::StunDiscoveryState& discovery,
                                const diagnostics::Trace* trace)
    -> Expected<GatheredCandidates, GatherError> {
  if (stop.stop_requested()) return unexpected(GatherError::cancelled);
  const auto bound = request.socket.local_endpoint();
  if (!bound || bound.value().port == 0) return unexpected(GatherError::socket);
  const auto port = bound.value().port;
  auto local = diagnostics::observe(
      trace, diagnostics::Operation::resolve,
      [&] { return select_local(request.interfaces, port); });
  if (request.gateway) {
    local =
        offered(CandidateType::local, request.gateway->local_address(), port);
    auto mapped = diagnostics::observe(
        trace, diagnostics::Operation::gateway_map,
        [&] { return request.gateway->map_udp(port, stop); });
    if (stop.stop_requested()) return unexpected(GatherError::cancelled);
    if (mapped) {
      net::nat::stun::StunAllocationEstimate allocation{mapped.value(), 0,
                                                        false};
      auto candidates = assemble_offer(local.value(), allocation);
      if (!candidates) return unexpected(GatherError::candidate);
      return GatheredCandidates{local.value(), allocation,
                                std::move(candidates.value())};
    }
  }
  if (!local) return unexpected(GatherError::local);

  auto allocation = net::nat::stun::AllocationDiscovery::discover(
      request.socket, request.poller, request.clock, request.random,
      request.http, request.dns, stop, request.per_server_timeout, trace,
      &discovery);
  if (!allocation) {
    using DiscoveryError = net::nat::stun::StunAllocationDiscoveryError;
    if (allocation.error() == DiscoveryError::cancelled)
      return unexpected(GatherError::cancelled);
    if (allocation.error() == DiscoveryError::random_failure)
      return unexpected(GatherError::stun);
    return unmapped(local.value(), allocation.error(), trace);
  }
  if (request.force_port_guessing && allocation.value().increment == 0 &&
      allocation.value().projected.port != local.value().port) {
    allocation.value().random_allocation = true;
    allocation.value().increment = 1;
    discovery.allocation = allocation.value();
  }
  auto candidates =
      assemble_offer(local.value(), allocation.value(),
                     request.force_port_guessing, request.guess_count);
  if (!candidates) return unexpected(GatherError::candidate);
  return GatheredCandidates{local.value(), allocation.value(),
                            std::move(candidates.value())};
}

}  // namespace libchiaking::net::nat::holepunch
