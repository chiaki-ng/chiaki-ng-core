#pragma once

#include "libchiaking/platform/dtls/provider.hpp"
#include "src/core/net/kakusan/datagram_security.hpp"
#include "src/core/net/kakusan/udp_transport_factory.hpp"

namespace test_support::kakusan {

template <class Provider>
class SecureDatagrams {
 public:
  static auto provider() -> Provider& {
    static Provider shared;
    return shared;
  }

  static auto factory() -> libchiaking::net::kakusan::DatagramSecurityFactory {
    return [](libchiaking::net::kakusan::SecurityRole role) {
      return libchiaking::net::kakusan::DatagramSecurity::open(provider(),
                                                               role);
    };
  }
};

}  // namespace test_support::kakusan
