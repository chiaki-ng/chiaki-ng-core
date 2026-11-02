#include "src/core/net/nat/ice/agent.hpp"

#include "libchiaking/detail/secret_array.hpp"
#include "src/core/utils/bytes/endian.hpp"
#include "src/core/utils/text/hex.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <utility>

namespace libchiaking::net::nat::ice {

auto Agent::hex_of(std::span<const std::byte> bytes) -> SecretString {
  SecretString text;
  text.reserve(bytes.size() * 2);
  text::append_hex(text, bytes);
  return text;
}

auto Agent::same_family(const platform::SocketEndpoint& left,
                        const platform::SocketEndpoint& right) noexcept
    -> bool {
  return left.address.family() == right.address.family();
}

auto Agent::usable_host(const platform::SocketEndpoint& endpoint) -> bool {
  const std::array candidates{
      Candidate{"h", 1, endpoint, CandidateType::host, {}}};
  return Parameters::check(candidates).has_value();
}

auto Agent::usable_reflexive(const platform::SocketEndpoint& base,
                             const platform::SocketEndpoint& mapped) -> bool {
  const std::array candidates{
      Candidate{"h", 2, base, CandidateType::host, {}},
      Candidate{"s", 1, mapped, CandidateType::server_reflexive, base}};
  return Parameters::check(candidates).has_value();
}

auto Agent::preferred(const Base& left, const Base& right) -> bool {
  if (left.preference != right.preference)
    return left.preference > right.preference;
  const auto left_text = platform::to_string(left.local.address);
  const auto right_text = platform::to_string(right.local.address);
  if (left_text != right_text) return left_text < right_text;
  return left.local.port < right.local.port;
}

auto Agent::key_of(const SecretString& password) noexcept
    -> std::span<const std::byte> {
  return password.bytes();
}

auto Agent::draw(std::span<std::byte> output) noexcept -> Result<void> {
  try {
    if (random_.fill_bytes(output)) return {};
  } catch (...) {
  }
  return unexpected(Error::crypto_unavailable);
}

Agent::Agent(platform::CryptoProvider& crypto,
             platform::SecureRandomProvider& random, Policy policy,
             std::uint64_t now_us)
    : crypto_(crypto),
      random_(random),
      policy_(policy),
      role_(policy == Policy::host ? Role::controlling : Role::controlled),
      gathering_deadline_us_(now_us + gathering_timeout_us),
      next_check_us_(now_us),
      next_check_transmit_us_(now_us) {}

Agent::~Agent() { close(); }

auto Agent::create(platform::CryptoProvider& crypto,
                   platform::SecureRandomProvider& random, AgentConfig config,
                   std::uint64_t now_us) -> Result<std::unique_ptr<Agent>> {
  std::unique_ptr<Agent> agent{new Agent(crypto, random, config.role, now_us)};
  const auto initialised = agent->initialise(std::move(config));
  if (!initialised) return unexpected(initialised.error());
  if (agent->bases_.empty()) {
    agent->stop(Error::no_path);
    return agent;
  }
  agent->start_gathering(now_us);
  if (agent->transactions_.empty()) agent->finish_gathering(now_us);
  return agent;
}

auto Agent::initialise(AgentConfig config) -> Result<void> {
  for (std::size_t index = 0; index < config.bases.size(); ++index) {
    for (std::size_t other = 0; other < index; ++other) {
      if (config.bases[other].socket == config.bases[index].socket)
        return unexpected(Error::internal);
    }
  }

  SecretArray<ufrag_bytes + password_bytes + tie_breaker_bytes> random;
  const auto drawn = draw(random);
  if (!drawn) return unexpected(drawn.error());
  const auto credentials =
      std::span<const std::byte>{random}.first(ufrag_bytes + password_bytes);
  if (std::all_of(credentials.begin(), credentials.end(),
                  [](std::byte byte) { return byte == std::byte{}; }))
    return unexpected(Error::crypto_unavailable);
  if (config.credentials) {
    local_ufrag_ = config.credentials->ufrag().clone();
    local_password_ = config.credentials->password().clone();
  } else {
    local_ufrag_ = hex_of(credentials.first(ufrag_bytes));
    local_password_ = hex_of(credentials.subspan(ufrag_bytes, password_bytes));
  }
  tie_breaker_ = bytes::load_be<std::uint64_t>(
      std::span<const std::byte>{random}.last(tie_breaker_bytes));

  std::vector<Base> accepted;
  for (const auto& base : config.bases) {
    if (accepted.size() == max_bases) break;
    const auto duplicate = std::any_of(
        accepted.begin(), accepted.end(),
        [&](const Base& other) { return other.local == base.local; });
    if (!duplicate && usable_host(base.local)) accepted.push_back(base);
  }
  std::vector<std::size_t> order(accepted.size());
  for (std::size_t index = 0; index < order.size(); ++index)
    order[index] = index;
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t left, std::size_t right) {
                     return preferred(accepted[left], accepted[right]);
                   });
  bases_.resize(accepted.size());
  for (std::size_t rank = 0; rank < order.size(); ++rank) {
    auto& state = bases_[order[rank]];
    state.base = accepted[order[rank]];
    state.local_preference =
        static_cast<std::uint16_t>(max_local_preference - rank);
  }
  std::vector<platform::IpAddress> addresses;
  for (std::size_t index = 0; index < bases_.size(); ++index) {
    auto& state = bases_[index];
    const auto known =
        std::find(addresses.begin(), addresses.end(), state.base.local.address);
    state.address_index = static_cast<std::size_t>(known - addresses.begin());
    if (known == addresses.end()) addresses.push_back(state.base.local.address);
    LocalCandidate host;
    host.type = CandidateType::host;
    host.endpoint = state.base.local;
    host.base = index;
    host.priority = host_priority(index);
    host.foundation = "h" + std::to_string(state.address_index);
    locals_.push_back(std::move(host));
  }

  guessing_ = config.guessing;
  for (const auto& server : config.stun_servers) {
    if (servers_.size() == max_stun_servers) break;
    if (server.port != 0 && !server.address.is_unspecified() &&
        std::find(servers_.begin(), servers_.end(), server) == servers_.end())
      servers_.push_back(server);
  }
  return {};
}

void Agent::start_gathering(std::uint64_t now_us) {
  for (std::size_t base = 0; base < bases_.size(); ++base) {
    for (std::size_t server = 0; server < servers_.size(); ++server) {
      if (lifecycle_.failed()) return;
      if (same_family(bases_[base].base.local, servers_[server]))
        start_server_query(TransactionKind::gather, base, server, 0, now_us);
    }
  }
}

void Agent::finish_gathering(std::uint64_t now_us) {
  if (lifecycle_.gathered() || lifecycle_.failed() || lifecycle_.closed())
    return;
  lifecycle_.finish_gathering();
  transactions_.erase(std::remove_if(transactions_.begin(), transactions_.end(),
                                     [](const Transaction& transaction) {
                                       return transaction.kind ==
                                              TransactionKind::gather;
                                     }),
                      transactions_.end());

  std::vector<std::size_t> order(locals_.size());
  for (std::size_t index = 0; index < order.size(); ++index)
    order[index] = index;
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t left, std::size_t right) {
                     return locals_[left].priority > locals_[right].priority;
                   });
  if (order.size() > max_candidates) order.resize(max_candidates);

  std::vector<Candidate> gathered;
  for (const auto index : order) {
    auto& local = locals_[index];
    Candidate candidate{
        local.foundation, local.priority, local.endpoint, local.type, {}};
    if (local.type == CandidateType::server_reflexive) {
      candidate.related = bases_[local.base].base.local;
      local.refreshing = true;
      local.refresh_at_us = now_us + mapping_refresh_us;
    }
    gathered.push_back(std::move(candidate));
  }
  auto credentials =
      Credentials::make(local_ufrag_.clone(), local_password_.clone());
  if (!credentials) {
    stop(Error::internal);
    return;
  }
  auto local =
      Parameters::make(std::move(credentials.value()), std::move(gathered),
                       wants_guesses() ? max_port_guesses : 0U);
  if (!local) {
    stop(Error::internal);
    return;
  }
  lifecycle_.emit(event::Gathered{std::move(local.value())});
}

auto Agent::wants_guesses() const noexcept -> bool {
  for (const auto& local : locals_) {
    if (local.type != CandidateType::server_reflexive) continue;
    if (guessing_ == PortGuessing::forced) return true;
    const auto moved =
        std::ranges::any_of(locals_, [&](const LocalCandidate& other) {
          return other.type == CandidateType::server_reflexive &&
                 other.base == local.base &&
                 other.endpoint.address == local.endpoint.address &&
                 other.endpoint.port != local.endpoint.port;
        });
    if (moved) return true;
  }
  return false;
}

auto Agent::mapped_ports(std::span<const Candidate> candidates)
    -> std::vector<MappedPorts> {
  std::vector<MappedPorts> observed;
  for (const auto& candidate : candidates) {
    if (candidate.type != CandidateType::server_reflexive) continue;
    const auto port = static_cast<std::int32_t>(candidate.endpoint.port);
    const auto known =
        std::ranges::find_if(observed, [&](const MappedPorts& ports) {
          return ports.related == candidate.related &&
                 ports.address == candidate.endpoint.address;
        });
    if (known == observed.end()) {
      observed.push_back({candidate.related, candidate.endpoint.address,
                          candidate.priority, port, port});
      continue;
    }
    known->priority = std::min(known->priority, candidate.priority);
    known->low = std::min(known->low, port);
    known->high = std::max(known->high, port);
  }
  return observed;
}

void Agent::guess_remote_mappings(const Parameters& remote) {
  const auto observed = mapped_ports(remote.candidates());
  std::size_t guessed{};
  for (std::int32_t step = 1;
       step <= static_cast<std::int32_t>(max_port_guesses); ++step) {
    for (const auto& ports : observed) {
      for (const auto port : {ports.high + step, ports.low - step}) {
        if (guessed == remote.port_guesses()) return;
        const platform::SocketEndpoint endpoint{ports.address,
                                                wrapped_port(port)};
        const auto known =
            std::ranges::any_of(remotes_, [&](const RemoteCandidate& other) {
              return other.endpoint == endpoint;
            });
        if (known) continue;
        const auto lowered = static_cast<std::uint32_t>(step)
                             << local_preference_shift;
        remotes_.push_back(RemoteCandidate{
            endpoint, ports.priority > lowered ? ports.priority - lowered : 1U,
            false});
        ++guessed;
      }
    }
  }
}

void Agent::stop(Error error) {
  if (lifecycle_.failed() || lifecycle_.closed()) return;
  transactions_.clear();
  triggered_.clear();
  early_.clear();
  lifecycle_.release();
  lifecycle_.fail(error);
}

auto Agent::local_key() const noexcept -> std::span<const std::byte> {
  return key_of(local_password_);
}

auto Agent::remote_key() const noexcept -> std::span<const std::byte> {
  return key_of(remote_password_);
}

auto Agent::find_base(SocketId socket) const noexcept
    -> std::optional<std::size_t> {
  for (std::size_t index = 0; index < bases_.size(); ++index) {
    if (bases_[index].base.socket == socket) return index;
  }
  return std::nullopt;
}

auto Agent::find_pair(std::size_t base,
                      const platform::SocketEndpoint& remote) noexcept
    -> Pair* {
  for (auto& pair : pairs_) {
    if (pair.base == base && pair.remote == remote) return &pair;
  }
  return nullptr;
}

auto Agent::pair_by_id(std::uint32_t id) noexcept -> Pair* {
  for (auto& pair : pairs_) {
    if (pair.id == id) return &pair;
  }
  return nullptr;
}

auto Agent::find_transaction(const stun::TransactionId& id) noexcept
    -> std::vector<Transaction>::iterator {
  return std::find_if(
      transactions_.begin(), transactions_.end(),
      [&](const Transaction& transaction) { return transaction.id == id; });
}

auto Agent::encoding_failure(stun::MessageError error) noexcept -> Error {
  switch (error) {
    case stun::MessageError::malformed:
    case stun::MessageError::unencodable:
      return Error::protocol;
    case stun::MessageError::crypto_unavailable:
      return Error::crypto_unavailable;
    case stun::MessageError::unsealable:
    case stun::MessageError::crypto_failed:
      return Error::internal;
  }
  return Error::internal;
}

auto Agent::fresh_transaction_id() -> Result<stun::TransactionId> {
  stun::TransactionId id{};
  for (std::size_t attempt = 0; attempt < transaction_draw_attempts;
       ++attempt) {
    const auto drawn = draw(id);
    if (!drawn) return unexpected(drawn.error());
    if (find_transaction(id) == transactions_.end()) return id;
  }
  return unexpected(Error::internal);
}

auto Agent::host_priority(std::size_t base) const noexcept -> std::uint32_t {
  return candidate_priority(host_type_preference,
                            bases_[base].local_preference);
}

auto Agent::reflexive_priority(std::size_t base) const noexcept
    -> std::uint32_t {
  return candidate_priority(reflexive_type_preference,
                            bases_[base].local_preference);
}

auto Agent::pair_priority(std::uint32_t local,
                          std::uint32_t remote) const noexcept
    -> std::uint64_t {
  const std::uint64_t controlling = role_ == Role::controlling ? local : remote;
  const std::uint64_t controlled = role_ == Role::controlling ? remote : local;
  return (std::min(controlling, controlled) << pair_priority_minimum_shift) +
         pair_priority_maximum_factor * std::max(controlling, controlled) +
         (controlling > controlled ? 1U : 0U);
}

auto Agent::mapped_priority(
    std::size_t base, const platform::SocketEndpoint& mapped) const noexcept
    -> std::uint32_t {
  for (const auto& local : locals_) {
    if (local.endpoint == mapped) return local.priority;
  }
  return reflexive_priority(base);
}

void Agent::reprioritise() noexcept {
  for (auto& pair : pairs_) {
    pair.priority = pair_priority(pair.local_priority, pair.remote_priority);
    if (pair.mapped)
      pair.valid_priority = pair_priority(
          mapped_priority(pair.base, *pair.mapped), pair.remote_priority);
  }
}

void Agent::switch_role(Role role) noexcept {
  if (role_ == role) return;
  role_ = role;
  reprioritise();
  if (role_ == Role::controlled) nominating_.reset();
}

void Agent::form_pairs() {
  std::vector<Pair> formed;
  for (std::size_t base = 0; base < bases_.size(); ++base) {
    for (const auto& remote : remotes_) {
      if (!same_family(bases_[base].base.local, remote.endpoint) ||
          remote.endpoint == bases_[base].base.local)
        continue;
      const auto existing =
          std::find_if(formed.begin(), formed.end(), [&](const Pair& pair) {
            return pair.base == base && pair.remote == remote.endpoint;
          });
      if (existing != formed.end()) {
        existing->remote_priority =
            std::max(existing->remote_priority, remote.priority);
        continue;
      }
      Pair pair;
      pair.base = base;
      pair.remote = remote.endpoint;
      pair.local_priority = host_priority(base);
      pair.remote_priority = remote.priority;
      formed.push_back(std::move(pair));
    }
  }
  for (auto& pair : formed)
    pair.priority = pair_priority(pair.local_priority, pair.remote_priority);
  std::stable_sort(formed.begin(), formed.end(),
                   [](const Pair& left, const Pair& right) {
                     return left.priority > right.priority;
                   });
  if (formed.size() > max_pairs) formed.resize(max_pairs);
  for (auto& pair : formed) pair.id = next_pair_id_++;
  pairs_ = std::move(formed);
}

auto Agent::add_pair(std::size_t base, const RemoteCandidate& remote) -> Pair* {
  Pair pair;
  pair.id = next_pair_id_++;
  pair.base = base;
  pair.remote = remote.endpoint;
  pair.local_priority = host_priority(base);
  pair.remote_priority = remote.priority;
  pair.priority = pair_priority(pair.local_priority, pair.remote_priority);
  if (pairs_.size() >= max_pairs) {
    auto victim = pairs_.end();
    for (auto candidate = pairs_.begin(); candidate != pairs_.end();
         ++candidate) {
      const auto idle = candidate->state == PairState::waiting ||
                        candidate->state == PairState::failed;
      if (!idle || candidate->queued || candidate->nominate_on_success ||
          pair_busy(candidate->id) || nominating_ == candidate->id)
        continue;
      if (victim == pairs_.end() || candidate->priority < victim->priority)
        victim = candidate;
    }
    if (victim == pairs_.end() || victim->priority >= pair.priority)
      return nullptr;
    pairs_.erase(victim);
  }
  pairs_.push_back(std::move(pair));
  return &pairs_.back();
}

auto Agent::build_request(std::size_t base, bool use_candidate,
                          const stun::TransactionId& id)
    -> Result<std::vector<std::byte>> {
  stun::Message message;
  message.kind = stun::MessageClass::request;
  message.transaction = id;
  message.username = remote_ufrag_.clone();
  *message.username += ':';
  *message.username += local_ufrag_;
  message.priority = reflexive_priority(base);
  if (role_ == Role::controlling)
    message.ice_controlling = tie_breaker_;
  else
    message.ice_controlled = tie_breaker_;
  message.use_candidate = use_candidate;
  auto encoded =
      message.encode(crypto_, remote_key(), stun::Fingerprint::present);
  if (!encoded) return unexpected(encoding_failure(encoded.error()));
  return std::move(encoded.value());
}

void Agent::respond(std::size_t base, const platform::SocketEndpoint& source,
                    const stun::Message& request,
                    std::optional<stun::ErrorResponse> error,
                    std::vector<std::uint16_t> unknown,
                    std::vector<Outgoing>& outgoing) {
  stun::Message response;
  response.transaction = request.transaction;
  if (error) {
    response.kind = stun::MessageClass::error;
    response.error = std::move(error);
    response.unknown_attributes = std::move(unknown);
  } else {
    response.kind = stun::MessageClass::success;
    response.xor_mapped = source;
  }
  auto encoded =
      response.encode(crypto_, local_key(), stun::Fingerprint::present);
  if (!encoded) return;
  outgoing.push_back(
      Outgoing{bases_[base].base.socket, source, std::move(encoded.value())});
}

void Agent::transmit(Transaction& transaction, std::uint64_t now_us,
                     std::vector<Outgoing>& outgoing) {
  outgoing.push_back(Outgoing{bases_[transaction.base].base.socket,
                              transaction.remote, transaction.bytes});
  ++transaction.sent;
  if (transaction.sent < transaction.limit)
    transaction.deadline_us =
        now_us + (transaction.rto_us << (transaction.sent - 1));
  else
    transaction.deadline_us = now_us + transaction.final_wait_us;
}

void Agent::start_server_query(TransactionKind kind, std::size_t base,
                               std::size_t server, std::size_t candidate,
                               std::uint64_t now_us) {
  stun::Message message;
  message.kind = stun::MessageClass::request;
  if (lifecycle_.failed()) return;
  const auto id = fresh_transaction_id();
  if (!id) {
    stop(id.error());
    return;
  }
  message.transaction = id.value();
  auto encoded = message.encode(crypto_, {}, stun::Fingerprint::present);
  if (!encoded) return;
  Transaction transaction;
  transaction.id = message.transaction;
  transaction.kind = kind;
  transaction.base = base;
  transaction.remote = servers_[server];
  transaction.server = server;
  transaction.candidate = candidate;
  transaction.bytes = std::move(encoded.value());
  transaction.limit = server_transmissions;
  transaction.rto_us = server_rto_us;
  transaction.final_wait_us = server_rto_us * server_final_wait_factor;
  transaction.deadline_us = now_us;
  transactions_.push_back(std::move(transaction));
}

auto Agent::check_budget() const noexcept -> bool {
  return check_requests_sent_ < max_check_requests;
}

auto Agent::start_check(Pair& pair, bool use_candidate, std::uint64_t now_us,
                        std::vector<Outgoing>& outgoing) -> bool {
  if (lifecycle_.failed() || !check_budget() ||
      now_us < next_check_transmit_us_)
    return false;
  const auto active = static_cast<std::uint64_t>(
      std::count_if(pairs_.begin(), pairs_.end(), [](const Pair& candidate) {
        return candidate.state == PairState::waiting ||
               candidate.state == PairState::in_progress;
      }));
  Transaction transaction;
  const auto id = fresh_transaction_id();
  if (!id) {
    stop(id.error());
    return false;
  }
  transaction.id = id.value();
  auto encoded = build_request(pair.base, use_candidate, transaction.id);
  if (!encoded) return false;
  transaction.kind = TransactionKind::check;
  transaction.base = pair.base;
  transaction.remote = pair.remote;
  transaction.pair = pair.id;
  transaction.role_sent = role_;
  transaction.use_candidate = use_candidate;
  transaction.bytes = std::move(encoded.value());
  transaction.limit = check_transmissions;
  transaction.rto_us = std::max(check_rto_min_us, pacing_interval_us * active);
  transaction.final_wait_us = transaction.rto_us * check_final_wait_factor;
  if (pair.state != PairState::succeeded) pair.state = PairState::in_progress;
  ++check_requests_sent_;
  next_check_transmit_us_ = now_us + check_floor_us;
  transmit(transaction, now_us, outgoing);
  transactions_.push_back(std::move(transaction));
  return true;
}

void Agent::cancel_pair_transactions(std::uint32_t pair, std::uint64_t now_us) {
  transactions_.erase(
      std::remove_if(transactions_.begin(), transactions_.end(),
                     [&](const Transaction& transaction) {
                       return transaction.kind == TransactionKind::check &&
                              transaction.pair == pair && transaction.cancelled;
                     }),
      transactions_.end());
  for (auto& transaction : transactions_) {
    if (transaction.kind != TransactionKind::check ||
        transaction.pair != pair || transaction.cancelled)
      continue;
    transaction.cancelled = true;
    transaction.deadline_us =
        std::max(transaction.deadline_us, now_us + transaction.rto_us);
  }
}

auto Agent::pair_busy(std::uint32_t pair) const noexcept -> bool {
  return std::any_of(transactions_.begin(), transactions_.end(),
                     [&](const Transaction& transaction) {
                       return transaction.kind == TransactionKind::check &&
                              transaction.pair == pair;
                     });
}

auto Agent::nomination_in_flight(std::uint32_t pair) const noexcept -> bool {
  return std::any_of(transactions_.begin(), transactions_.end(),
                     [&](const Transaction& transaction) {
                       return transaction.kind == TransactionKind::check &&
                              transaction.pair == pair &&
                              transaction.use_candidate;
                     });
}

void Agent::enqueue_triggered(Pair& pair) {
  if (pair.state != PairState::succeeded) pair.state = PairState::waiting;
  if (pair.queued) return;
  pair.queued = true;
  triggered_.push_back(pair.id);
}

auto Agent::connect(const Parameters& remote, std::uint64_t now_us)
    -> Result<void> {
  if (lifecycle_.closed() || lifecycle_.failed() || !lifecycle_.connect())
    return unexpected(Error::protocol);
  remote_ufrag_ = remote.ufrag().clone();
  remote_password_ = remote.password().clone();
  for (const auto& candidate : remote.candidates())
    remotes_.push_back(
        RemoteCandidate{candidate.endpoint, candidate.priority, false});
  guess_remote_mappings(remote);
  connect_us_ = now_us;
  if (policy_ == Policy::host)
    connectivity_deadline_us_ = now_us + connectivity_timeout_us;
  next_check_us_ = std::max(next_check_us_, now_us);
  if (!early_.empty()) lifecycle_.hold();
  form_pairs();
  return {};
}

void Agent::receive(SocketId socket, const platform::SocketEndpoint& source,
                    std::span<const std::byte> datagram, std::uint64_t now_us,
                    std::vector<Outgoing>& outgoing) {
  if (lifecycle_.closed() || !stun::Message::recognises(datagram)) return;
  const auto base = find_base(socket);
  if (!base || !same_family(bases_[*base].base.local, source)) return;
  const auto decoded = stun::ReceivedMessage::decode(datagram);
  if (!decoded) return;
  switch (decoded.value().message.kind) {
    case stun::MessageClass::request:
      handle_request(*base, source, datagram, decoded.value(), now_us,
                     outgoing);
      return;
    case stun::MessageClass::success:
    case stun::MessageClass::error:
      handle_response(*base, source, datagram, decoded.value(), now_us);
      return;
    case stun::MessageClass::indication:
      return;
  }
}

void Agent::handle_request(std::size_t base,
                           const platform::SocketEndpoint& source,
                           std::span<const std::byte> datagram,
                           const stun::ReceivedMessage& decoded,
                           std::uint64_t now_us,
                           std::vector<Outgoing>& outgoing) {
  const auto& message = decoded.message;
  if (!decoded.fingerprinted || !decoded.integrity_offset ||
      !message.username || !message.priority || *message.priority == 0 ||
      message.ice_controlling.has_value() == message.ice_controlled.has_value())
    return;
  const auto username = message.username->view();
  const auto prefix_size = local_ufrag_.size() + 1;
  if (username.size() <= prefix_size ||
      username.compare(0, local_ufrag_.size(), local_ufrag_.view()) != 0 ||
      username[local_ufrag_.size()] != ':')
    return;
  if (lifecycle_.connected() && username.compare(prefix_size, std::string::npos,
                                                 remote_ufrag_.view()) != 0)
    return;
  if (!decoded.verify_integrity(crypto_, datagram, local_key())) return;
  if (lifecycle_.connected()) {
    process_request(base, source, decoded, now_us, outgoing);
    return;
  }
  if (lifecycle_.failed()) return;
  early_.erase(std::remove_if(early_.begin(), early_.end(),
                              [&](const EarlyRequest& early) {
                                return early.base == base &&
                                       early.source == source;
                              }),
               early_.end());
  if (early_.size() >= max_early_requests) early_.pop_front();
  early_.push_back(EarlyRequest{base, source, SecretBytes{datagram}});
}

void Agent::process_request(std::size_t base,
                            const platform::SocketEndpoint& source,
                            const stun::ReceivedMessage& decoded,
                            std::uint64_t now_us,
                            std::vector<Outgoing>& outgoing) {
  const auto& message = decoded.message;
  if (!decoded.unknown_required.empty()) {
    respond(base, source, message,
            stun::ErrorResponse{stun::error_code::unknown_attribute,
                                std::string{stun::ErrorResponse::reason_phrase(
                                    stun::error_code::unknown_attribute)}},
            decoded.unknown_required, outgoing);
    return;
  }
  if (lifecycle_.failed() || nominated_) {
    respond(base, source, message, std::nullopt, {}, outgoing);
    return;
  }
  const stun::ErrorResponse conflict{
      stun::error_code::role_conflict,
      std::string{
          stun::ErrorResponse::reason_phrase(stun::error_code::role_conflict)}};
  if (role_ == Role::controlling && message.ice_controlling) {
    if (tie_breaker_ >= *message.ice_controlling) {
      respond(base, source, message, conflict, {}, outgoing);
      return;
    }
    switch_role(Role::controlled);
  } else if (role_ == Role::controlled && message.ice_controlled) {
    if (tie_breaker_ < *message.ice_controlled) {
      respond(base, source, message, conflict, {}, outgoing);
      return;
    }
    switch_role(Role::controlling);
  }
  respond(base, source, message, std::nullopt, {}, outgoing);

  auto remote = std::find_if(remotes_.begin(), remotes_.end(),
                             [&](const RemoteCandidate& candidate) {
                               return candidate.endpoint == source;
                             });
  if (remote == remotes_.end()) {
    const auto learned = std::count_if(
        remotes_.begin(), remotes_.end(),
        [](const RemoteCandidate& candidate) { return candidate.reflexive; });
    if (static_cast<std::size_t>(learned) >= max_remote_reflexive) return;
    remotes_.push_back(RemoteCandidate{source, *message.priority, true});
    remote = remotes_.end() - 1;
  }
  auto* pair = find_pair(base, source);
  if (pair == nullptr) pair = add_pair(base, *remote);
  if (pair == nullptr) return;
  if (pair->state == PairState::in_progress)
    cancel_pair_transactions(pair->id, now_us);
  if (pair->state != PairState::succeeded) enqueue_triggered(*pair);
  if (message.use_candidate && role_ == Role::controlled) {
    if (pair->state == PairState::succeeded && pair->mapped)
      nominate(*pair, now_us);
    else
      pair->nominate_on_success = true;
  }
}

void Agent::handle_response(std::size_t base,
                            const platform::SocketEndpoint& source,
                            std::span<const std::byte> datagram,
                            const stun::ReceivedMessage& decoded,
                            std::uint64_t now_us) {
  const auto found = find_transaction(decoded.message.transaction);
  if (found == transactions_.end()) return;
  const auto symmetric = found->base == base && found->remote == source;
  const auto& message = decoded.message;

  if (found->kind == TransactionKind::gather ||
      found->kind == TransactionKind::refresh) {
    if (!symmetric) return;
    std::optional<platform::SocketEndpoint> mapped;
    if (message.kind == stun::MessageClass::success) {
      mapped = message.xor_mapped ? message.xor_mapped : message.mapped;
      if (!mapped || !same_family(*mapped, bases_[base].base.local)) return;
    }
    auto transaction = std::move(*found);
    transactions_.erase(found);
    handle_server_response(std::move(transaction), mapped, now_us);
    return;
  }

  if (!decoded.fingerprinted ||
      !decoded.verify_integrity(crypto_, datagram, remote_key()))
    return;
  if (message.kind == stun::MessageClass::success && !message.xor_mapped)
    return;
  if (message.kind == stun::MessageClass::error && !message.error) return;
  auto transaction = std::move(*found);
  transactions_.erase(found);
  if (transaction.kind == TransactionKind::consent) {
    if (symmetric && message.kind == stun::MessageClass::success)
      last_consent_us_ = std::max(last_consent_us_, now_us);
    return;
  }
  handle_check_response(std::move(transaction), symmetric, decoded, now_us);
}

void Agent::handle_server_response(
    Transaction transaction, std::optional<platform::SocketEndpoint> mapped,
    std::uint64_t now_us) {
  if (transaction.kind == TransactionKind::gather) {
    if (mapped) learn_reflexive(transaction.base, transaction.server, *mapped);
    const auto pending =
        std::any_of(transactions_.begin(), transactions_.end(),
                    [](const Transaction& other) {
                      return other.kind == TransactionKind::gather;
                    });
    if (!lifecycle_.gathered() && !pending) finish_gathering(now_us);
    return;
  }
  if (!mapped || nominated_ || transaction.candidate >= locals_.size()) return;
  if (locals_[transaction.candidate].endpoint != *mapped) stop(Error::no_path);
}

void Agent::handle_check_response(Transaction transaction, bool symmetric,
                                  const stun::ReceivedMessage& decoded,
                                  std::uint64_t now_us) {
  auto* pair = pair_by_id(transaction.pair);
  if (pair == nullptr || nominated_) return;
  const auto& message = decoded.message;
  const auto nominating = transaction.use_candidate && nominating_ == pair->id;
  if (!symmetric || (message.kind == stun::MessageClass::error &&
                     message.error->code != stun::error_code::role_conflict)) {
    if (nominating) nominating_.reset();
    if (nominating || (!pair_busy(pair->id) &&
                       pair->state != PairState::succeeded && !pair->queued)) {
      pair->state = PairState::failed;
      pair->mapped.reset();
    }
    return;
  }
  if (message.kind == stun::MessageClass::error) {
    if (transaction.role_sent == role_)
      switch_role(role_ == Role::controlling ? Role::controlled
                                             : Role::controlling);
    if (nominating) nominating_.reset();
    if (pair->state != PairState::succeeded) enqueue_triggered(*pair);
    return;
  }
  pair->state = PairState::succeeded;
  if (pair->queued) {
    pair->queued = false;
    triggered_.erase(
        std::remove(triggered_.begin(), triggered_.end(), pair->id),
        triggered_.end());
  }
  pair->mapped = message.xor_mapped;
  pair->valid_priority = pair_priority(
      mapped_priority(pair->base, *pair->mapped), pair->remote_priority);
  if (!first_valid_us_) first_valid_us_ = now_us;
  if (nominating && role_ == Role::controlling) {
    nominate(*pair, now_us);
    return;
  }
  if (role_ == Role::controlled && pair->nominate_on_success)
    nominate(*pair, now_us);
}

void Agent::learn_reflexive(std::size_t base, std::size_t server,
                            const platform::SocketEndpoint& mapped) {
  if (lifecycle_.gathered()) return;
  const auto& local = bases_[base].base.local;
  if (!same_family(local, mapped) || mapped == local) return;
  const auto known = std::any_of(locals_.begin(), locals_.end(),
                                 [&](const LocalCandidate& candidate) {
                                   return candidate.endpoint == mapped;
                                 });
  if (known || !usable_reflexive(local, mapped)) return;
  LocalCandidate candidate;
  candidate.type = CandidateType::server_reflexive;
  candidate.endpoint = mapped;
  candidate.base = base;
  candidate.server = server;
  candidate.priority = candidate_priority(server_reflexive_type_preference,
                                          bases_[base].local_preference);
  candidate.foundation = "s" + std::to_string(bases_[base].address_index) +
                         "x" + std::to_string(server);
  locals_.push_back(std::move(candidate));
}

void Agent::nominate(Pair& pair, std::uint64_t now_us) {
  if (lifecycle_.failed() || nominated_ || !pair.mapped) return;
  const auto interval = consent_interval_us();
  if (!interval) {
    stop(interval.error());
    return;
  }
  nominated_ = Path{bases_[pair.base].base.socket, *pair.mapped, pair.remote};
  const auto base = pair.base;
  nominating_.reset();
  rearm_at_us_.reset();
  connectivity_deadline_us_.reset();
  transactions_.erase(std::remove_if(transactions_.begin(), transactions_.end(),
                                     [](const Transaction& transaction) {
                                       return transaction.kind ==
                                              TransactionKind::check;
                                     }),
                      transactions_.end());
  triggered_.clear();
  for (auto& candidate : pairs_) candidate.queued = false;
  early_.clear();
  lifecycle_.release();
  last_consent_us_ = now_us;
  next_consent_us_ = now_us + interval.value();
  for (auto& local : locals_) {
    if (local.type == CandidateType::server_reflexive && local.base != base)
      local.refreshing = false;
  }
  lifecycle_.emit(event::Nominated{*nominated_});
}

void Agent::poll(std::uint64_t now_us, std::vector<Outgoing>& outgoing) {
  if (lifecycle_.closed() || lifecycle_.failed()) return;
  if (lifecycle_.connected() && !nominated_ && connectivity_deadline_us_ &&
      now_us >= *connectivity_deadline_us_) {
    stop(Error::no_path);
    return;
  }
  run_refreshes(now_us);
  if (lifecycle_.failed()) return;
  run_transactions(now_us, outgoing);
  if (lifecycle_.failed()) return;
  if (!lifecycle_.gathered()) {
    const auto pending =
        std::any_of(transactions_.begin(), transactions_.end(),
                    [](const Transaction& transaction) {
                      return transaction.kind == TransactionKind::gather;
                    });
    if (!pending || now_us >= gathering_deadline_us_) finish_gathering(now_us);
    if (lifecycle_.failed()) return;
  }
  if (lifecycle_.connected() && !nominated_) {
    run_early_requests(now_us, outgoing);
    if (lifecycle_.failed()) return;
    run_rearm(now_us);
    run_nomination(now_us);
    run_checks(now_us, outgoing);
    if (lifecycle_.failed()) return;
    run_rearm(now_us);
  }
  if (nominated_) run_consent(now_us, outgoing);
}

void Agent::run_transactions(std::uint64_t now_us,
                             std::vector<Outgoing>& outgoing) {
  for (std::size_t index = 0; index < transactions_.size();) {
    auto& transaction = transactions_[index];
    if (transaction.deadline_us > now_us) {
      ++index;
      continue;
    }
    const auto retransmit =
        transaction.sent == 0 ||
        (!transaction.cancelled && transaction.sent < transaction.limit);
    if (retransmit && transaction.kind == TransactionKind::check) {
      if (!check_budget()) {
        transaction.cancelled = true;
        transaction.deadline_us = now_us + transaction.rto_us;
        ++index;
        continue;
      }
      if (now_us < next_check_transmit_us_) {
        ++index;
        continue;
      }
      ++check_requests_sent_;
      next_check_transmit_us_ = now_us + check_floor_us;
    }
    if (retransmit) {
      transmit(transaction, now_us, outgoing);
      ++index;
      continue;
    }
    auto expired = std::move(transaction);
    transactions_.erase(transactions_.begin() +
                        static_cast<std::ptrdiff_t>(index));
    expire(expired);
    if (lifecycle_.failed()) return;
  }
}

void Agent::expire(const Transaction& transaction) {
  if (transaction.kind != TransactionKind::check) return;
  auto* pair = pair_by_id(transaction.pair);
  if (pair == nullptr) return;
  if (transaction.use_candidate && nominating_ == pair->id) {
    ++pair->nomination_tries;
    if (pair->nomination_tries < nomination_attempts) return;
    nominating_.reset();
    pair->state = PairState::failed;
    pair->mapped.reset();
    return;
  }
  if (pair_busy(pair->id) || pair->state == PairState::succeeded ||
      pair->queued)
    return;
  pair->state = PairState::failed;
}

void Agent::run_refreshes(std::uint64_t now_us) {
  if (!lifecycle_.gathered()) return;
  for (std::size_t index = 0; index < locals_.size(); ++index) {
    auto& local = locals_[index];
    if (!local.refreshing || local.refresh_at_us > now_us) continue;
    local.refresh_at_us = now_us + mapping_refresh_us;
    transactions_.erase(
        std::remove_if(transactions_.begin(), transactions_.end(),
                       [&](const Transaction& transaction) {
                         return transaction.kind == TransactionKind::refresh &&
                                transaction.candidate == index;
                       }),
        transactions_.end());
    start_server_query(TransactionKind::refresh, local.base, local.server,
                       index, now_us);
    if (lifecycle_.failed()) return;
  }
}

void Agent::run_early_requests(std::uint64_t now_us,
                               std::vector<Outgoing>& outgoing) {
  if (!lifecycle_.holding()) return;
  lifecycle_.release();
  auto queued = std::move(early_);
  early_.clear();
  const auto prefix_size = local_ufrag_.size() + 1;
  for (const auto& request : queued) {
    const auto decoded = stun::ReceivedMessage::decode(request.bytes.span());
    if (!decoded || !decoded.value().message.username ||
        decoded.value().message.username->view().compare(
            prefix_size, std::string::npos, remote_ufrag_.view()) != 0)
      continue;
    process_request(request.base, request.source, decoded.value(), now_us,
                    outgoing);
    if (nominated_ || lifecycle_.failed()) return;
  }
}

auto Agent::best_valid_index() const noexcept -> std::optional<std::size_t> {
  std::optional<std::size_t> best;
  for (std::size_t index = 0; index < pairs_.size(); ++index) {
    const auto& pair = pairs_[index];
    if (pair.state != PairState::succeeded || !pair.mapped) continue;
    if (!best || pair.valid_priority > pairs_[*best].valid_priority)
      best = index;
  }
  return best;
}

auto Agent::higher_pending(std::uint64_t priority) const noexcept -> bool {
  return std::any_of(pairs_.begin(), pairs_.end(), [&](const Pair& pair) {
    return pair.priority > priority && (pair.state == PairState::waiting ||
                                        pair.state == PairState::in_progress);
  });
}

auto Agent::settle_deadline_us() const noexcept
    -> std::optional<std::uint64_t> {
  if (role_ != Role::controlling || nominating_ || !first_valid_us_)
    return std::nullopt;
  const auto best = best_valid_index();
  if (!best) return std::nullopt;
  if (higher_pending(pairs_[*best].valid_priority))
    return *first_valid_us_ + nomination_settle_us;
  return *first_valid_us_;
}

void Agent::run_nomination(std::uint64_t now_us) {
  const auto due = settle_deadline_us();
  if (!due || now_us < *due) return;
  auto& best = pairs_[*best_valid_index()];
  best.nomination_tries = 0;
  nominating_ = best.id;
}

auto Agent::check_work_pending() const noexcept -> bool {
  if (nominating_ && role_ == Role::controlling &&
      !nomination_in_flight(*nominating_))
    return true;
  return std::any_of(pairs_.begin(), pairs_.end(), [](const Pair& pair) {
    return pair.state == PairState::waiting;
  });
}

void Agent::run_checks(std::uint64_t now_us, std::vector<Outgoing>& outgoing) {
  if (now_us < next_check_us_ || now_us < next_check_transmit_us_ ||
      !check_budget())
    return;
  Pair* chosen = nullptr;
  bool nomination = false;
  bool triggered = false;
  if (nominating_ && role_ == Role::controlling &&
      !nomination_in_flight(*nominating_)) {
    chosen = pair_by_id(*nominating_);
    nomination = chosen != nullptr;
  }
  while (chosen == nullptr && !triggered_.empty()) {
    auto* pair = pair_by_id(triggered_.front());
    if (pair == nullptr || !pair->queued) {
      triggered_.pop_front();
      continue;
    }
    chosen = pair;
    triggered = true;
  }
  if (chosen == nullptr) {
    for (auto& pair : pairs_) {
      if (pair.state == PairState::waiting &&
          (chosen == nullptr || pair.priority > chosen->priority))
        chosen = &pair;
    }
  }
  if (chosen == nullptr || !start_check(*chosen, nomination, now_us, outgoing))
    return;
  if (triggered) {
    triggered_.pop_front();
    chosen->queued = false;
  }
  next_check_us_ = now_us + pacing_interval_us;
}

auto Agent::round_finished() const noexcept -> bool {
  if (!triggered_.empty() || pair_checks_active()) return false;
  auto failed = false;
  for (const auto& pair : pairs_) {
    if (pair.state == PairState::waiting ||
        pair.state == PairState::in_progress)
      return false;
    failed = failed || pair.state == PairState::failed;
  }
  return failed;
}

auto Agent::pair_checks_active() const noexcept -> bool {
  return std::any_of(transactions_.begin(), transactions_.end(),
                     [](const Transaction& transaction) {
                       return transaction.kind == TransactionKind::check;
                     });
}

void Agent::run_rearm(std::uint64_t now_us) {
  if (policy_ != Policy::guest || !check_budget()) return;
  if (rearm_at_us_) {
    if (now_us < *rearm_at_us_) return;
    rearm_at_us_.reset();
    for (auto& pair : pairs_) {
      if (pair.state == PairState::failed) pair.state = PairState::waiting;
    }
    return;
  }
  if (round_finished()) rearm_at_us_ = now_us + guest_rearm_us;
}

auto Agent::consent_interval_us() -> Result<std::uint64_t> {
  SecretArray<consent_draw_size> entropy;
  const auto drawn = draw(entropy);
  if (!drawn) return unexpected(drawn.error());
  const auto value = bytes::load_be<std::uint32_t>(entropy);
  return consent_interval_min_us + value % (consent_interval_span_us + 1);
}

void Agent::run_consent(std::uint64_t now_us, std::vector<Outgoing>& outgoing) {
  if (lifecycle_.failed() || lifecycle_.consent_expired()) return;
  if (now_us >= last_consent_us_ + consent_timeout_us) {
    lifecycle_.expire_consent();
    stop(Error::no_path);
    return;
  }
  if (now_us < next_consent_us_) return;
  const auto interval = consent_interval_us();
  if (!interval) {
    stop(interval.error());
    return;
  }
  const auto base = find_base(nominated_->socket);
  if (!base) return;
  const auto id = fresh_transaction_id();
  if (!id) {
    stop(id.error());
    return;
  }
  next_consent_us_ = now_us + interval.value();
  const auto outstanding =
      std::count_if(transactions_.begin(), transactions_.end(),
                    [](const Transaction& transaction) {
                      return transaction.kind == TransactionKind::consent;
                    });
  if (static_cast<std::size_t>(outstanding) >= max_consent_outstanding) {
    const auto oldest =
        std::find_if(transactions_.begin(), transactions_.end(),
                     [](const Transaction& transaction) {
                       return transaction.kind == TransactionKind::consent;
                     });
    transactions_.erase(oldest);
  }
  Transaction transaction;
  transaction.id = id.value();
  auto encoded = build_request(*base, false, transaction.id);
  if (!encoded) return;
  transaction.kind = TransactionKind::consent;
  transaction.base = *base;
  transaction.remote = nominated_->remote;
  transaction.role_sent = role_;
  transaction.bytes = std::move(encoded.value());
  transaction.limit = 1;
  transaction.rto_us = consent_response_window_us;
  transaction.final_wait_us = consent_response_window_us;
  transmit(transaction, now_us, outgoing);
  transactions_.push_back(std::move(transaction));
}

auto Agent::take_event() -> std::optional<Event> { return lifecycle_.take(); }

auto Agent::next_deadline_us() const noexcept -> std::optional<std::uint64_t> {
  if (lifecycle_.closed() || lifecycle_.failed()) return std::nullopt;
  timing::Deadline deadline;
  if (!lifecycle_.gathered()) deadline.consider(gathering_deadline_us_);
  for (const auto& transaction : transactions_) {
    const auto retransmit =
        transaction.sent == 0 ||
        (!transaction.cancelled && transaction.sent < transaction.limit);
    if (retransmit && transaction.kind == TransactionKind::check &&
        check_budget())
      deadline.consider(
          std::max(transaction.deadline_us, next_check_transmit_us_));
    else
      deadline.consider(transaction.deadline_us);
  }
  if (lifecycle_.gathered()) {
    for (const auto& local : locals_) {
      if (local.refreshing) deadline.consider(local.refresh_at_us);
    }
  }
  if (lifecycle_.connected() && !nominated_) {
    if (connectivity_deadline_us_)
      deadline.consider(*connectivity_deadline_us_);
    if (lifecycle_.holding()) deadline.consider(connect_us_);
    if (check_budget() && check_work_pending())
      deadline.consider(std::max(next_check_us_, next_check_transmit_us_));
    if (const auto settle = settle_deadline_us()) deadline.consider(*settle);
    if (rearm_at_us_)
      deadline.consider(*rearm_at_us_);
    else if (policy_ == Policy::guest && check_budget() && round_finished())
      deadline.consider(connect_us_);
  }
  if (nominated_ && !lifecycle_.consent_expired()) {
    deadline.consider(next_consent_us_);
    deadline.consider(last_consent_us_ + consent_timeout_us);
  }
  return deadline.at_us();
}

auto Agent::nominated() const noexcept -> std::optional<Path> {
  if (lifecycle_.closed()) return std::nullopt;
  return nominated_;
}

auto Agent::consent() const noexcept -> bool {
  return !lifecycle_.closed() && nominated_.has_value() &&
         !lifecycle_.consent_expired();
}

void Agent::close() noexcept {
  if (lifecycle_.closed()) return;
  lifecycle_.close();
  local_ufrag_ = SecretString{};
  local_password_ = SecretString{};
  remote_ufrag_ = SecretString{};
  remote_password_ = SecretString{};
  tie_breaker_ = 0;
  early_.clear();
  transactions_.clear();
  triggered_.clear();
  pairs_.clear();
  remotes_.clear();
  rearm_at_us_.reset();
  connectivity_deadline_us_.reset();
  nominating_.reset();
}

}  // namespace libchiaking::net::nat::ice
