#include "src/core/kakusan/kakusan_core.hpp"

#include "libchiaking/detail/secret_array.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <utility>

namespace libchiaking::kakusan {

Surroundings::Surroundings(
    platform::CryptoProvider& crypto, platform::SecureRandomProvider& random,
    ClockSource clock, platform::ThreadingProvider& threading,
    platform::ClockProvider* wall, platform::DiagnosticsProvider* log,
    DeviceIdentity identity, std::unique_ptr<RememberedDevices> remembered,
    std::optional<Network> network,
    net::kakusan::SpokeTransportFactory* transports)
    : crypto_(&crypto),
      random_(&random),
      clock_(std::move(clock)),
      threading_(&threading),
      log_(log),
      identity_(std::move(identity)),
      remembered_(std::move(remembered)),
      network_mutex_(threading),
      network_(std::move(network)),
      transports_(transports),
      stream_clock_(clock_),
      stream_wall_clock_(clock_),
      stream_platform_(platform::PlatformProviders{
          .threading = &threading,
          .clock = &stream_clock_,
          .wall_clock = wall != nullptr ? wall : &stream_wall_clock_,
          .random = &random,
          .network = network_ ? &network_->network : nullptr,
          .crypto = &crypto,
          .diagnostics = log}) {}

auto Surroundings::of(platform::PlatformFactory& platform, DeviceStore& store,
                      std::span<const std::string> stun_servers)
    -> Expected<std::shared_ptr<Surroundings>, Kakusan::Unavailable> {
  if (!platform.supplies(platform::PlatformCapability::secure_datagram))
    return unexpected(Kakusan::Unavailable::no_secure_transport);
  if (!platform.supplies(platform::PlatformCapability::crypto) ||
      !has_identity_crypto(platform.crypto()))
    return unexpected(Kakusan::Unavailable::no_identity_crypto);
  if (!usable(stun_servers))
    return unexpected(Kakusan::Unavailable::invalid_stun_servers);
  auto* steady = &platform.clock();
  auto* wall = &platform.wall_clock();
  ClockSource clock = [steady, wall] {
    using namespace std::chrono;
    return Clock{
        steady->now_microseconds(),
        static_cast<std::uint64_t>(
            duration_cast<seconds>(microseconds{wall->now_microseconds()})
                .count())};
  };
  return assemble(platform.crypto(), platform.random(), std::move(clock),
                  platform.threading(), wall,
                  platform.supplies(platform::PlatformCapability::diagnostics)
                      ? &platform.diagnostics()
                      : nullptr,
                  store,
                  Network{platform.secure_datagram(),
                          platform.network(),
                          {stun_servers.begin(), stun_servers.end()}},
                  nullptr);
}

auto Surroundings::of(Injected injected)
    -> Expected<std::shared_ptr<Surroundings>, Kakusan::Unavailable> {
  const bool networked =
      injected.network != nullptr && injected.secure_datagram != nullptr;
  if (injected.transports == nullptr && !networked)
    return unexpected(Kakusan::Unavailable::no_secure_transport);
  if (!injected.clock || !has_identity_crypto(injected.crypto))
    return unexpected(Kakusan::Unavailable::no_identity_crypto);
  std::optional<Network> network;
  if (injected.transports == nullptr)
    network.emplace(Network{*injected.secure_datagram, *injected.network, {}});
  return assemble(injected.crypto, injected.random, std::move(injected.clock),
                  injected.threading, nullptr, injected.log, injected.store,
                  std::move(network), injected.transports);
}

auto Surroundings::assemble(platform::CryptoProvider& crypto,
                            platform::SecureRandomProvider& random,
                            ClockSource clock,
                            platform::ThreadingProvider& threading,
                            platform::ClockProvider* wall,
                            platform::DiagnosticsProvider* log,
                            DeviceStore& store, std::optional<Network> network,
                            net::kakusan::SpokeTransportFactory* transports)
    -> Expected<std::shared_ptr<Surroundings>, Kakusan::Unavailable> {
  auto identity = own_identity(crypto, random, store);
  if (!identity) return unexpected(identity.error());
  auto remembered = RememberedDevices::load(crypto, threading, store);
  if (!remembered)
    return unexpected(Kakusan::Unavailable::device_store_unavailable);
  return std::shared_ptr<Surroundings>{new Surroundings{
      crypto, random, std::move(clock), threading, wall, log,
      std::move(identity).value(), std::move(remembered).value(),
      std::move(network), transports}};
}

auto Surroundings::own_identity(platform::CryptoProvider& crypto,
                                platform::SecureRandomProvider& random,
                                DeviceStore& store)
    -> Expected<DeviceIdentity, Kakusan::Unavailable> {
  const auto saved = store.load_identity();
  if (!saved) return unexpected(Kakusan::Unavailable::device_store_unavailable);
  if (saved.value()) {
    auto restored = DeviceIdentity::restore(crypto, *saved.value());
    if (!restored) return unexpected(Kakusan::Unavailable::no_identity_crypto);
    return std::move(restored).value();
  }
  auto created = DeviceIdentity::create(crypto, random);
  if (!created) return unexpected(Kakusan::Unavailable::no_identity_crypto);
  if (!store.save_identity(created.value().saved()))
    return unexpected(Kakusan::Unavailable::device_store_unavailable);
  return std::move(created).value();
}

auto Surroundings::usable(std::span<const std::string> stun_servers) noexcept
    -> bool {
  return stun_servers.size() <= Kakusan::most_stun_servers &&
         std::ranges::all_of(stun_servers, [](const std::string& server) {
           return !server.empty() &&
                  server.size() <= Kakusan::longest_stun_server;
         });
}

auto Surroundings::use_stun_servers(std::span<const std::string> stun_servers)
    -> Expected<void, Kakusan::Unavailable> {
  if (!usable(stun_servers))
    return unexpected(Kakusan::Unavailable::invalid_stun_servers);
  std::vector<std::string> servers{stun_servers.begin(), stun_servers.end()};
  const platform::Lock lock{network_mutex_};
  if (network_) network_->stun_servers = std::move(servers);
  return {};
}

void Surroundings::use_port_guessing(Kakusan::PortGuessing guessing) {
  const platform::Lock lock{network_mutex_};
  if (network_)
    network_->guessing = guessing == Kakusan::PortGuessing::forced
                             ? net::nat::ice::PortGuessing::forced
                             : net::nat::ice::PortGuessing::automatic;
}

auto Surroundings::connections() const -> std::unique_ptr<Connections> {
  if (transports_ != nullptr)
    return std::make_unique<Connections>(*transports_);
  const platform::Lock lock{network_mutex_};
  return std::make_unique<Connections>(
      *crypto_, network_->secure_datagram, network_->network, *random_,
      network_->stun_servers, network_->guessing);
}

void Surroundings::log(platform::LogSeverity severity,
                       std::string_view message) const noexcept {
  if (log_ != nullptr) log_->write({severity, message});
}

auto Surroundings::has_identity_crypto(platform::CryptoProvider& crypto)
    -> bool {
  constexpr platform::ScryptParameters smallest{2, 1, 1};
  SecretArray<platform::ed25519_seed_size> seed;
  seed.fill(std::byte{1});
  SecretArray<platform::p256_scalar_size> one;
  one[one.size() - 1] = std::byte{1};
  const std::array<std::byte, 1> message{std::byte{1}};
  std::array<std::byte, platform::sha512_digest_size> wide{};
  std::array<std::byte, platform::hmac_sha1_digest_size> narrow{};
  std::array<std::byte, platform::sha256_digest_size> derived{};
  std::array<std::byte, platform::ed25519_public_key_size> key{};
  std::array<std::byte, platform::ed25519_signature_size> signature{};
  std::array<std::byte, platform::p256_point_size> generator{};
  std::array<std::byte, platform::p256_point_size> same{};
  std::array<std::byte, platform::p256_point_size> doubled{};
  if (!crypto.sha512(message, wide) ||
      !crypto.hmac_sha512(message, message, wide) ||
      !crypto.hmac_sha1(message, message, narrow) ||
      !crypto.scrypt(message, message, smallest, derived) ||
      !crypto.ed25519_public_key(
          std::span<const std::byte>{seed.data(), seed.size()}, key) ||
      !crypto.ed25519_sign(std::span<const std::byte>{seed.data(), seed.size()},
                           message, signature))
    return false;
  const auto verified = crypto.ed25519_verify(key, message, signature);
  if (!verified || !verified.value()) return false;
  const std::span<const std::byte> scalar{one.data(), one.size()};
  return crypto.p256_base_multiply(scalar, generator) &&
         crypto.p256_multiply(scalar, generator, same) && same == generator &&
         crypto.p256_add(generator, generator, doubled);
}

}  // namespace libchiaking::kakusan
