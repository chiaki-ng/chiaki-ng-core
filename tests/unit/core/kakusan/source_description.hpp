#pragma once

#include "libchiaking/detail/play/session_share.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/platform_crypto_provider.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/net/protocol/takion/source_tap.hpp"
#include "src/core/play/adapters/session/session_share_core.hpp"
#include "tests/support/setup.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace kakusan_test {

class SourceDescriptions {
 public:
  static auto describe(std::uint32_t version,
                       libchiaking::play::SharedDatagram::Media media,
                       std::uint64_t key_position,
                       std::span<const std::byte> bytes)
      -> libchiaking::play::SharedDatagram {
    namespace takion = libchiaking::net::protocol::takion;
    libchiaking::play::SharedDatagram unparsed;
    unparsed.media = media;
    unparsed.key_position = key_position;
    unparsed.bytes = bytes;
    const auto profile = takion::resolve_takion_wire_profile(version);
    if (!profile) return unparsed;
    const auto packet =
        takion::PacketCodec::decode_plain_packet(bytes, *profile);
    if (!packet) return unparsed;
    const auto unit =
        takion::PacketCodec::decode_av_packet(packet.value(), *profile);
    if (!unit) return unparsed;
    auto described = libchiaking::play::SessionShare::Core::shared(
        takion::PacketCodec::describe_source_datagram(unit.value(), *profile,
                                                      key_position, bytes));
    described.media = media;
    return described;
  }

  static auto relayout(libchiaking::platform::CryptoProvider& crypto,
                       const libchiaking::play::ReceiveKeys& keys,
                       std::uint32_t version,
                       libchiaking::play::SharedDatagram::Media media,
                       std::uint64_t key_position,
                       std::vector<std::byte> version_12)
      -> std::vector<std::byte> {
    namespace takion = libchiaking::net::protocol::takion;
    if (version == 12) return version_12;
    test_support::Setup::require(version == 15 || version == 20,
                                 "the source version is known");
    auto bytes = std::move(version_12);
    if (media == libchiaking::play::SharedDatagram::Media::audio) {
      bytes[7] = std::byte{};
      bytes[8] &= std::byte{0x0f};
    }
    const std::size_t shift = version == 20 ? 8 : 0;
    bytes.insert(bytes.begin() + 1, shift, std::byte{});
    const auto mac = bytes.begin() + static_cast<std::ptrdiff_t>(shift + 10);
    std::fill_n(mac, takion::takion_mac_size, std::byte{});
    libchiaking::SecretArray<takion::source_key_size> key;
    libchiaking::SecretArray<takion::source_key_size> iv;
    std::ranges::copy(keys.key(), key.begin());
    std::ranges::copy(keys.iv(), iv.begin());
    const takion::SourceReceiveKeys wired{std::move(key), std::move(iv)};
    libchiaking::platform::DefaultThreadingProvider threading;
    takion::PlatformTakionCryptoProvider provider{crypto, threading};
    const auto direction =
        test_support::Setup::value(provider.create_receive_only(wired),
                                    "the receive direction is made");
    const auto computed =
        test_support::Setup::value(direction->authenticate(key_position, bytes),
                                    "the source datagram authenticates");
    std::copy(computed.begin(), computed.end(), mac);
    return bytes;
  }
};

}  // namespace kakusan_test
