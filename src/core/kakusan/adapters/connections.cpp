#include "src/core/kakusan/adapters/connections.hpp"

#include "src/core/net/kakusan/datagram_security.hpp"

namespace libchiaking::kakusan {

Connections::Connections(platform::CryptoProvider& crypto,
                         platform::SecureDatagramProvider& secure_datagram,
                         platform::NetworkProvider& network,
                         platform::SecureRandomProvider& random,
                         std::span<const std::string> stun_servers,
                         net::nat::ice::PortGuessing guessing)
    : udp_(std::in_place,
           net::kakusan::UdpTransportDependencies{
               network, crypto, random,
               [&secure_datagram](net::kakusan::SecurityRole role) {
                 return net::kakusan::DatagramSecurity::open(secure_datagram,
                                                             role);
               }},
           net::kakusan::UdpTransportConfig{
               net::kakusan::UdpTransportFactory::resolve_stun_servers(
                   network.dns(), stun_servers),
               false, guessing}),
      used_(&*udp_) {}

Connections::Connections(net::kakusan::SpokeTransportFactory& borrowed) noexcept
    : used_(&borrowed) {}

}  // namespace libchiaking::kakusan
