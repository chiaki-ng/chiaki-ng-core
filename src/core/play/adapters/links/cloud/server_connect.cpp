#include "src/core/play/adapters/links/cloud/server_connect.hpp"

#include "libchiaking/detail/wire_sizes.hpp"
#include "src/core/net/protocol/takion/detail/envelope_codec.hpp"
#include "src/core/net/protocol/takion/platform_crypto_provider.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/net/protocol/takion/versions.hpp"
#include "src/core/net/transport/datagram/udp_transport.hpp"
#include "src/core/utils/text/base64.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace libchiaking::play {

auto ServerConnect::active_envelope(std::uint8_t psn_wrapper_type)
    -> net::protocol::takion::PacketEnvelope {
  if (psn_wrapper_type == 0) return net::protocol::takion::PlainEnvelope{};
  return net::protocol::takion::PsnEnvelope{psn_wrapper_type};
}

auto Calibration::steady_rtt(std::chrono::microseconds mean)
    -> net::protocol::senkusha::RttStatistics {
  net::protocol::senkusha::RttStatistics statistics;
  statistics.raw_mean = mean;
  statistics.adjusted_mean = mean;
  return statistics;
}

auto HandshakeKey::decode(std::string_view encoded)
    -> Expected<HandshakeKey, ConnectError> {
  HandshakeKey key;
  const auto decoded = text::base64::decode_into(
      encoded, std::span<std::byte>{key.bytes_.data(), key.bytes_.size()},
      text::base64::Alphabet::standard);
  if (!decoded)
    return unexpected(
        ConnectError{decoded.error() == text::base64::DecodeError::too_long
                         ? ConnectError::Reason::handshake_key_length
                         : ConnectError::Reason::handshake_key_encoding});
  if (decoded.value() != key.bytes_.size())
    return unexpected(ConnectError{ConnectError::Reason::handshake_key_length});
  return key;
}

auto Calibration::takion_version(psn::StreamingService service,
                                 std::uint32_t takion_version_override) noexcept
    -> std::uint32_t {
  switch (service) {
    case psn::StreamingService::legacy_cloud_stream:
      return net::protocol::takion::select_takion_version(
          net::protocol::takion::takion_version_legacy_cloud_stream,
          takion_version_override,
          net::protocol::takion::SessionScope::legacy_cloud_stream);
    case psn::StreamingService::cloud_stream:
      return net::protocol::takion::select_takion_version(
          net::protocol::takion::takion_version_cloud_stream,
          takion_version_override,
          net::protocol::takion::SessionScope::cloud_stream);
    default:
      return 0;
  }
}

auto Calibration::from_ping(const psn::gk::Ping& row,
                            psn::StreamingService service,
                            std::uint32_t takion_version_override)
    -> Expected<Calibration, ConnectError> {
  if (!row.measured)
    return unexpected(ConnectError{ConnectError::Reason::not_measured});
  const auto reported = row.reported();
  const std::int64_t rtt_ms =
      reported.rtt_ms > 0 ? reported.rtt_ms : psn::gk::Ping::default_rtt_ms;
  const auto mtu_in = static_cast<std::uint32_t>(
      row.mtu_in > 0 ? row.mtu_in : psn::gk::Ping::default_mtu_in);
  const auto mtu_out = static_cast<std::uint32_t>(
      row.mtu_out > 0 ? row.mtu_out : psn::gk::Ping::default_mtu_out);
  const auto version = takion_version(service, takion_version_override);
  if (version == 0)
    return unexpected(
        ConnectError{ConnectError::Reason::unsupported_takion_version});
  const auto profile =
      net::protocol::takion::resolve_takion_wire_profile(version);
  if (!profile)
    return unexpected(
        ConnectError{ConnectError::Reason::unsupported_takion_version});
  const auto mean =
      std::chrono::microseconds{std::chrono::milliseconds{rtt_ms}};
  return Calibration{
      service, net::protocol::senkusha::Result{version, steady_rtt(mean),
                                               mtu_in, mtu_out, *profile}};
}

auto Calibration::from_allocation(psn::StreamingService service,
                                  std::uint32_t takion_version_override,
                                  std::uint32_t mtu_in, std::uint32_t mtu_out,
                                  std::uint64_t rtt_us)
    -> Expected<Calibration, ConnectError> {
  const auto version = takion_version(service, takion_version_override);
  if (version == 0)
    return unexpected(
        ConnectError{ConnectError::Reason::unsupported_takion_version});
  const auto profile =
      net::protocol::takion::resolve_takion_wire_profile(version);
  if (!profile)
    return unexpected(
        ConnectError{ConnectError::Reason::unsupported_takion_version});
  const bool measured = mtu_in > 0 && mtu_out > 0;
  const auto mean = std::chrono::microseconds{static_cast<std::int64_t>(
      measured && rtt_us > 0 ? rtt_us : unmeasured_rtt_us)};
  return Calibration{
      service,
      net::protocol::senkusha::Result{
          version, steady_rtt(mean),
          measured ? mtu_in
                   : static_cast<std::uint32_t>(psn::gk::Ping::default_mtu_in),
          measured ? mtu_out
                   : static_cast<std::uint32_t>(psn::gk::Ping::default_mtu_out),
          *profile}};
}

auto AllocatedEndpoint::resolve(platform::DnsProvider& dns,
                                std::string_view host, std::uint16_t port)
    -> Expected<AllocatedEndpoint, ConnectError> {
  if (host.empty())
    return unexpected(ConnectError{ConnectError::Reason::invalid_host});
  const std::uint16_t effective_port = port != 0 ? port : default_port;
  const bool want_ipv6 = host.find(':') != std::string_view::npos;
  const auto answers = dns.resolve(host, std::to_string(effective_port));
  if (!answers)
    return unexpected(ConnectError{ConnectError::Reason::dns_failure});
  const auto match =
      std::ranges::find_if(answers.value(), [want_ipv6](const auto& address) {
        return want_ipv6 ? address.is_ipv6() : address.is_ipv4();
      });
  if (match == answers.value().end())
    return unexpected(ConnectError{ConnectError::Reason::no_address});
  return AllocatedEndpoint{platform::SocketEndpoint{*match, effective_port}};
}

Handshake::Handshake(
    HandshakeKey key, net::protocol::remote_play::PreparedEcdh ecdh,
    std::shared_ptr<const SecretString> session_id,
    std::shared_ptr<const SecretString> launch_specification) noexcept
    : key_(std::move(key)),
      ecdh_(std::move(ecdh)),
      session_id_(std::move(session_id)),
      launch_specification_(std::move(launch_specification)) {}

auto Handshake::prepare(platform::CryptoProvider& crypto,
                        const psn::gk::Allocation& allocation,
                        const net::protocol::senkusha::Result& calibration,
                        std::uint32_t expected_version)
    -> Expected<Handshake, ConnectError> {
  if (allocation.launch_specification.empty())
    return unexpected(
        ConnectError{ConnectError::Reason::invalid_launch_specification});

  auto key = HandshakeKey::decode(allocation.handshake_key.view());
  if (!key) return unexpected(key.error());

  const auto profile = net::protocol::takion::resolve_takion_wire_profile(
      calibration.takion_version);
  const bool expected =
      expected_version == 0
          ? calibration.takion_version ==
                    net::protocol::takion::takion_version_legacy_cloud_stream ||
                calibration.takion_version ==
                    net::protocol::takion::takion_version_cloud_stream
          : calibration.takion_version == expected_version;
  if (!profile || !expected ||
      calibration.wire_profile.version() != profile->version() ||
      calibration.wire_profile.ecdh_curve() != profile->ecdh_curve())
    return unexpected(ConnectError{ConnectError::Reason::invalid_profile});

  const auto curve = profile->ecdh_curve();
  if (platform::ecdh_shared_secret_size(curve) == 0)
    return unexpected(ConnectError{ConnectError::Reason::unsupported_curve});
  auto ecdh = net::protocol::remote_play::EcdhWireCodec::prepare(
      crypto, curve, key.value().bytes());
  if (!ecdh)
    return unexpected(ConnectError{ConnectError::Reason::ecdh_failure});

  return Handshake{
      std::move(key.value()), std::move(ecdh.value()),
      std::make_shared<const SecretString>(
          allocation.session_id.view().substr(0, session_id_size_max - 1)),
      std::make_shared<const SecretString>(
          allocation.launch_specification.view())};
}

auto ServerConnect::takion_config(std::uint8_t psn_wrapper_type)
    -> net::protocol::takion::ClientConfig {
  net::protocol::takion::ClientConfig config;
  config.envelope = active_envelope(psn_wrapper_type);
  config.receive_envelope = net::protocol::takion::PlainEnvelope{};
  return config;
}

auto ServerConnect::control_profile(psn::StreamingService service,
                                    const Options& options) noexcept
    -> net::protocol::remote_play::ActiveControlProfile {
  net::protocol::remote_play::ActiveControlProfile profile;
  profile.controller_type =
      options.dualsense
          ? net::protocol::takion::ControllerConnectionType::dualsense
          : net::protocol::takion::ControllerConnectionType::dualshock4;
  profile.audio_assembly =
      service == psn::StreamingService::cloud_stream
          ? net::protocol::remote_play::AudioAssemblyPolicy::cloud_generation
          : net::protocol::remote_play::AudioAssemblyPolicy::console_units;
  profile.datagram_budget = net::protocol::remote_play::ActiveDatagramBudget{
      net::protocol::senkusha::mtu_network_overhead, reserved_envelope_bytes};
  profile.hevc = service == psn::StreamingService::cloud_stream;
  profile.enable_idr_on_fec_failure = options.enable_idr_on_fec_failure;
  profile.pad_count = options.pad_count;
  profile.peer_silence_timeout_us = peer_silence_timeout_us;
  net::protocol::remote_play::ActiveControlProfile::apply_session_tuning(
      profile, options.tuning);
  if (options.tuning.negotiate_takion_version)
    profile.takion_offer = net::protocol::takion::takion_negotiation_offer(
        service == psn::StreamingService::cloud_stream
            ? net::protocol::takion::SessionScope::cloud_stream
            : net::protocol::takion::SessionScope::legacy_cloud_stream);
  return profile;
}

auto ServerConnect::connect(const psn::gk::Allocation& allocation,
                            Calibration calibration, Options options)
    -> Expected<Connected, ConnectError> {
  const auto service = calibration.service();
  auto established = route(allocation, std::move(calibration));
  if (!established) return unexpected(established.error());
  return start(std::move(established.value()), allocation, service,
               std::move(options));
}

auto ServerConnect::route(const psn::gk::Allocation& allocation,
                          Calibration calibration)
    -> Expected<net::protocol::remote_play::EstablishedRoute, ConnectError> {
  const auto remote = AllocatedEndpoint::resolve(
      providers_.network.dns(), allocation.server_ip, allocation.server_port);
  if (!remote) return unexpected(remote.error());
  const auto& peer = remote.value().socket();

  net::transport::datagram::UdpTransportConfig config;
  config.local = {peer.address.is_ipv6() ? platform::IpAddress::any_ipv6()
                                         : platform::IpAddress::any_ipv4(),
                  0};
  config.peer = peer;
  config.dont_fragment = false;
  config.connect_peer = true;
  auto transport = net::transport::datagram::ProviderUdpTransport::create(
      providers_.network, config);
  if (!transport)
    return unexpected(ConnectError{ConnectError::Reason::transport_failure});
  const auto local = transport.value()->local_endpoint();
  if (!local) {
    static_cast<void>(transport.value()->close());
    return unexpected(ConnectError{ConnectError::Reason::transport_failure});
  }
  std::unique_ptr<net::transport::datagram::DatagramTransport> route_transport{
      std::move(transport.value())};
  return net::protocol::remote_play::EstablishedRoute{
      {std::move(route_transport), local.value(), peer,
       net::protocol::remote_play::RouteKind::cloud_allocated,
       net::protocol::remote_play::DontFragmentPolicy::disabled},
      calibration.measurement()};
}

auto ServerConnect::start(net::protocol::remote_play::EstablishedRoute route,
                          const psn::gk::Allocation& allocation,
                          psn::StreamingService service, Options options)
    -> Expected<Connected, ConnectError> {
  if (!route.transport)
    return unexpected(ConnectError{ConnectError::Reason::invalid_route});
  const auto abandon = [&route](ConnectError::Reason reason) {
    static_cast<void>(route.transport.close());
    return unexpected(ConnectError{reason});
  };
  const auto expected_version = Calibration::takion_version(
      service, options.tuning.takion_version_override);
  if ((service != psn::StreamingService::legacy_cloud_stream &&
       service != psn::StreamingService::cloud_stream) ||
      route.calibration.takion_version != expected_version)
    return abandon(ConnectError::Reason::invalid_route);
  if (route.kind != net::protocol::remote_play::RouteKind::cloud_allocated)
    return abandon(ConnectError::Reason::invalid_route);
  auto handshake = Handshake::prepare(providers_.crypto, allocation,
                                      route.calibration, expected_version);
  if (!handshake) return abandon(handshake.error().reason());

  const auto* pollable = route.transport->pollable();
  auto& prepared = handshake.value();
  auto session = net::protocol::remote_play::ActiveSession::start(
      std::move(route), providers_.clock, providers_.threading,
      providers_.random, providers_.crypto, providers_.takion_crypto,
      std::move(prepared.key_).release(), std::move(prepared.ecdh_),
      std::move(prepared.session_id_),
      std::move(prepared.launch_specification_),
      takion_config(allocation.psn_wrapper_type),
      control_profile(service, options));
  if (!session)
    return unexpected(ConnectError{ConnectError::Reason::start_failed});
  return Connected{std::move(session.value()), pollable};
}

}  // namespace libchiaking::play
