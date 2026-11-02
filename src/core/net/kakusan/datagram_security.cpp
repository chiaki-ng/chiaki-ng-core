#include "src/core/net/kakusan/datagram_security.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace libchiaking::net::kakusan {

auto DatagramSecurity::open(platform::SecureDatagramProvider& provider,
                            SecurityRole role, DatagramSecurityConfig config)
    -> Result<std::unique_ptr<DatagramSecurity>> {
  using std::chrono::microseconds;
  constexpr auto longest =
      static_cast<std::uint64_t>(microseconds::max().count());
  platform::SecureDatagramProfile profile;
  profile.role = role;
  profile.alpn = std::move(config.alpn);
  profile.mtu = max_datagram_size;
  profile.max_certificate_size = max_certificate_size;
  profile.handshake_deadline =
      instant(std::min(config.handshake_timeout_us, longest));
  profile.retransmit_first = instant(security_retransmit_initial_us);
  profile.retransmit_max = instant(security_retransmit_max_us);
  profile.record_limit = config.record_limit;
  auto session = provider.open(profile);
  if (!session) return fail(protocol_error(session.error()));
  if (!session.value()) return fail(Error::internal);
  return std::unique_ptr<DatagramSecurity>{
      new DatagramSecurity(std::move(session).value())};
}

DatagramSecurity::DatagramSecurity(std::unique_ptr<Session> session) noexcept
    : session_(std::move(session)) {}

auto DatagramSecurity::certificate_digest() const noexcept -> const Digest256& {
  return session_->certificate_digest();
}

auto DatagramSecurity::start(const Digest256& peer_certificate_digest,
                             std::uint64_t now_us) -> Result<void> {
  return translate(session_->start(peer_certificate_digest, instant(now_us)));
}

auto DatagramSecurity::receive(std::span<const std::byte> datagram,
                               std::uint64_t now_us) -> Result<void> {
  return translate(session_->receive(datagram, instant(now_us)));
}

auto DatagramSecurity::poll(std::uint64_t now_us) -> Result<void> {
  return translate(session_->poll(instant(now_us)));
}

auto DatagramSecurity::take_datagram()
    -> std::optional<std::vector<std::byte>> {
  return session_->take_datagram();
}

auto DatagramSecurity::take_record() -> std::optional<SecretBytes> {
  return session_->take_record();
}

auto DatagramSecurity::send(std::span<const std::byte> record) -> Result<void> {
  return translate(session_->send(record));
}

auto DatagramSecurity::state() const noexcept -> SecurityState {
  return session_->state();
}

auto DatagramSecurity::established() const noexcept -> bool {
  return state() == SecurityState::established;
}

auto DatagramSecurity::max_record_size() const noexcept -> std::size_t {
  return session_->max_record_size();
}

auto DatagramSecurity::export_secret(std::string_view label,
                                     std::span<const std::byte> context,
                                     std::span<std::byte> output)
    -> Result<void> {
  const auto secret = session_->export_secret(label, context, output.size());
  if (!secret) return fail(protocol_error(secret.error()));
  if (secret.value().size() != output.size()) return fail(Error::internal);
  std::ranges::copy(secret.value().bytes(), output.begin());
  return {};
}

auto DatagramSecurity::next_deadline_us() const noexcept
    -> std::optional<std::uint64_t> {
  const auto deadline = session_->next_deadline();
  if (!deadline) return std::nullopt;
  return static_cast<std::uint64_t>(
      std::max<Session::Instant::rep>(deadline->count(), 0));
}

void DatagramSecurity::close() noexcept { session_->close(); }

auto DatagramSecurity::protocol_error(
    platform::SecureDatagramError error) noexcept -> Error {
  switch (error) {
    case platform::SecureDatagramError::handshake_timeout:
      return Error::timeout;
    case platform::SecureDatagramError::invalid_state:
    case platform::SecureDatagramError::invalid_argument:
    case platform::SecureDatagramError::peer_not_pinned:
    case platform::SecureDatagramError::handshake_failed:
    case platform::SecureDatagramError::record_limit:
    case platform::SecureDatagramError::peer_violation:
      return Error::protocol;
    case platform::SecureDatagramError::unsupported:
    case platform::SecureDatagramError::invalid_profile:
    case platform::SecureDatagramError::internal:
      return Error::internal;
  }
  return Error::internal;
}

auto DatagramSecurity::instant(std::uint64_t now_us) noexcept
    -> Session::Instant {
  constexpr auto longest =
      static_cast<std::uint64_t>(Session::Instant::max().count());
  return Session::Instant{
      static_cast<Session::Instant::rep>(std::min(now_us, longest))};
}

auto DatagramSecurity::translate(
    const Expected<void, platform::SecureDatagramError>& result)
    -> Result<void> {
  if (!result) return fail(protocol_error(result.error()));
  return {};
}

}  // namespace libchiaking::net::kakusan
