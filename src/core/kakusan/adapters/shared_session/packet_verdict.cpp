#include "src/core/kakusan/adapters/shared_session/packet_verdict.hpp"

#include <limits>

namespace libchiaking::kakusan {

auto PacketVerdict::video(std::size_t profile_count,
                          const play::SharedDatagram& datagram) noexcept
    -> Admission {
  if (datagram.unit_total == 0 || datagram.unit_index >= datagram.unit_total ||
      datagram.unit_redundant >= datagram.unit_total ||
      datagram.stream_index >= profile_count)
    return Admission::reject;
  return Admission::forward;
}

auto PacketVerdict::audio(const play::SharedDatagram& datagram) noexcept
    -> Admission {
  if (datagram.auxiliary) return Admission::exclude;
  if (datagram.codec != opus_codec || datagram.unit_total == 0 ||
      datagram.unit_size == 0 || datagram.unit_redundant >= datagram.unit_total)
    return Admission::reject;
  return static_cast<std::size_t>(datagram.unit_size) * datagram.unit_total ==
                 datagram.sealed_size
             ? Admission::forward
             : Admission::reject;
}

auto PacketVerdict::classify(std::size_t profile_count,
                             const play::SharedDatagram& datagram) noexcept
    -> PacketVerdict {
  const bool pictured = datagram.media == play::SharedDatagram::Media::video;
  const PacketVerdict rejected{Admission::reject,
                               pictured ? net::kakusan::MediaKind::video
                                        : net::kakusan::MediaKind::audio};
  const auto bytes = datagram.bytes;
  if (bytes.empty() || bytes.size() > net::kakusan::max_source_datagram_size ||
      datagram.sealed_size == 0 || datagram.sealed_size >= bytes.size())
    return rejected;
  if ((std::to_integer<std::uint8_t>(bytes.front()) & family_mask) !=
      (pictured ? family_video : family_audio))
    return rejected;
  if (datagram.key_position >
      std::numeric_limits<std::uint64_t>::max() - cipher_offset - bytes.size())
    return rejected;
  return PacketVerdict{
      pictured ? video(profile_count, datagram) : audio(datagram),
      rejected.kind};
}

}  // namespace libchiaking::kakusan
