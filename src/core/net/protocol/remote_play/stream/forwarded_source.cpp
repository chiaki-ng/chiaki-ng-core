#include "src/core/net/protocol/remote_play/stream/forwarded_source.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include "src/core/net/protocol/remote_play/stream/active_session.hpp"
#include "src/core/net/protocol/remote_play/stream/limits.hpp"
#include "src/core/net/protocol/remote_play/stream/public_media_event.hpp"
#include "src/core/net/protocol/remote_play/stream/stream_info.hpp"
#include "src/core/net/protocol/remote_play/stream/video_reorder_queue.hpp"
#include "src/core/net/protocol/takion/detail/crypto_context.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/platform_crypto_provider.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/utils/memory/constant_time.hpp"

#include <algorithm>
#include <deque>
#include <limits>
#include <utility>

namespace libchiaking::net::protocol::remote_play {
class ForwardedSourceReceiver::Impl final : private ActiveMediaSink {
 public:
  Impl(takion::WireProfile profile,
       std::unique_ptr<takion::CryptoDirection> direction,
       ValidatedStreamInfo info, bool hevc)
      : profile_(profile),
        direction_(std::move(direction)),
        info_(std::move(info)),
        media_(*this, media_options(profile, hevc), diagnostics_) {
    media_.on_stream_info(info_);
  }

  auto stream_info() const -> event::StreamInfoReady {
    return StreamInfoCodec::to_public_stream_info(info_);
  }

  [[nodiscard]] auto push(takion::SourceMedia media, std::uint64_t key_position,
                          std::span<const std::byte> datagram,
                          std::uint64_t now_us)
      -> Expected<void, ForwardedSourceError> {
    if (datagram.empty()) return malformed();
    const takion::PacketType type{
        std::to_integer<std::uint8_t>(datagram.front())};
    const auto family = type.family();
    const auto expected = media == takion::SourceMedia::video
                              ? takion::PacketFamily::video
                              : takion::PacketFamily::audio_and_haptics;
    if (!family || *family != expected ||
        datagram.size() < profile_.packet_header_size(type))
      return malformed();
    const auto offsets =
        takion::CryptoContext::packet_offsets(*family, profile_);
    if (!offsets ||
        offsets.value().mac + takion::takion_mac_size > datagram.size() ||
        offsets.value().key_position_low + sizeof(std::uint32_t) >
            datagram.size())
      return malformed();
    if (bytes::load_be<std::uint32_t>(
            datagram.subspan(offsets.value().key_position_low)) !=
        static_cast<std::uint32_t>(key_position & 0xffffffffU))
      return malformed();
    if (key_position > std::numeric_limits<std::uint64_t>::max() -
                           offsets.value().cipher_key_position_addition -
                           datagram.size())
      return malformed();

    std::vector<std::byte> bytes(datagram.begin(), datagram.end());
    const auto mac =
        bytes.begin() + static_cast<std::ptrdiff_t>(offsets.value().mac);
    takion::Mac received{};
    std::copy_n(mac, takion::takion_mac_size, received.begin());
    std::fill_n(mac, takion::takion_mac_size, std::byte{});
    const auto computed = direction_->authenticate(key_position, bytes);
    if (!computed) return unexpected(ForwardedSourceError::crypto_unavailable);
    if (!memory::constant_time_equal(received, computed.value()))
      return unexpected(ForwardedSourceError::authentication_failed);
    std::copy(received.begin(), received.end(), mac);

    auto packet = takion::PacketCodec::decode_plain_packet(bytes, profile_);
    if (!packet) return malformed();
    const auto sealed =
        takion::PacketCodec::decode_av_packet(packet.value(), profile_);
    if (!sealed) return malformed();
    const auto sealed_data =
        std::span<std::byte>{bytes}.last(sealed.value().data.size());
    const auto decrypted = direction_->apply_keystream(
        key_position + offsets.value().cipher_key_position_addition,
        sealed_data, sealed_data);
    if (!decrypted) return unexpected(ForwardedSourceError::crypto_unavailable);
    auto unit = takion::PacketCodec::decode_av_packet(packet.value(), profile_);
    if (!unit ||
        (media == takion::SourceMedia::audio && unit.value().is_haptics))
      return malformed();

    if (media == takion::SourceMedia::audio)
      return consume(std::move(unit).value(), now_us);
    reorder_.push(std::move(unit).value(), now_us, reordered_);
    return drain(now_us);
  }

  [[nodiscard]] auto poll(std::uint64_t now_us)
      -> Expected<void, ForwardedSourceError> {
    if (!reorder_.waiting()) return {};
    reorder_.flush(now_us, reordered_);
    return drain(now_us);
  }

  auto next_deadline_us() const noexcept -> std::optional<std::uint64_t> {
    return reorder_.next_deadline_us();
  }

  [[nodiscard]] auto take() -> std::optional<event::EncodedMedia> {
    if (auto media = media_.take())
      return PublicMediaEvent::from(std::move(*media));
    if (feedback_.empty()) return std::nullopt;
    auto event = PublicMediaEvent::from(feedback_.front());
    feedback_.pop_front();
    return event;
  }

  [[nodiscard]] auto take_idr_request() noexcept -> bool {
    return std::exchange(idr_requested_, false);
  }

 private:
  [[nodiscard]] static auto malformed() noexcept
      -> Unexpected<ForwardedSourceError> {
    return unexpected(ForwardedSourceError::malformed_datagram);
  }

  static auto media_options(const takion::WireProfile& profile,
                            bool hevc) noexcept -> ActiveMediaOptions {
    ActiveMediaOptions options;
    options.count_only = profile.features().contains(
        takion::Feature::av_unit_count_only_encoding);
    options.hevc = hevc;
    return options;
  }

  [[nodiscard]] auto consume(takion::AvPacketValues values,
                             std::uint64_t now_us)
      -> Expected<void, ForwardedSourceError> {
    if (!media_.consume(std::move(values), now_us))
      return unexpected(ForwardedSourceError::media_failed);
    return {};
  }

  [[nodiscard]] auto drain(std::uint64_t now_us)
      -> Expected<void, ForwardedSourceError> {
    Expected<void, ForwardedSourceError> result;
    for (auto& values : reordered_) {
      if (!result) break;
      result = consume(std::move(values), now_us);
    }
    reordered_.clear();
    return result;
  }

  [[nodiscard]] auto post_corrupt_frame(std::uint16_t, std::uint16_t)
      -> Expected<void, ActiveSessionError> override {
    idr_requested_ = true;
    return {};
  }

  [[nodiscard]] auto post_idr_after_loss()
      -> Expected<bool, ActiveSessionError> override {
    idr_requested_ = true;
    return true;
  }

  void on_video_fec_failure(std::uint16_t frame_index,
                            bool idr_request_sent) override {
    if (feedback_.size() == feedback_history_capacity) feedback_.pop_front();
    feedback_.push_back(
        feedback_event::VideoFecFailure{frame_index, idr_request_sent});
  }

  takion::WireProfile profile_;
  std::unique_ptr<takion::CryptoDirection> direction_;
  ValidatedStreamInfo info_;
  StreamDiagnostics diagnostics_{.available = true};
  ActiveMediaPipeline media_;
  VideoReorderQueue reorder_;
  std::vector<takion::AvPacketValues> reordered_;
  std::deque<feedback_event::VideoFecFailure> feedback_;
  bool idr_requested_{};
};

auto ForwardedSourceReceiver::create(platform::CryptoProvider& crypto,
                                     platform::ThreadingProvider& threading,
                                     ForwardedSourceSetup setup)
    -> Expected<std::unique_ptr<ForwardedSourceReceiver>,
                ForwardedSourceError> {
  const auto profile =
      takion::resolve_takion_wire_profile(setup.takion_version);
  if (!profile || !forwardable_takion_version(setup.takion_version))
    return unexpected(ForwardedSourceError::unsupported_version);
  if (setup.profiles.empty())
    return unexpected(ForwardedSourceError::invalid_setup);

  takion::StreamInfo info;
  info.audio_header.assign(setup.audio_header.begin(),
                           setup.audio_header.end());
  info.resolutions.reserve(setup.profiles.size());
  for (auto& entry : setup.profiles) {
    if (entry.video_header.empty())
      return unexpected(ForwardedSourceError::invalid_setup);
    info.resolutions.push_back(takion::StreamInfoResolution{
        entry.width, entry.height, std::move(entry.video_header)});
  }
  const auto requested = info.resolutions.size();
  auto validated = StreamInfoCodec::validate(std::move(info));
  if (!validated || validated.value().resolutions.size() != requested)
    return unexpected(ForwardedSourceError::invalid_setup);

  takion::PlatformTakionCryptoProvider provider{crypto, threading};
  auto direction = [&] {
    auto keys = std::move(setup.keys);
    return provider.create_receive_only(keys);
  }();
  if (!direction) return unexpected(ForwardedSourceError::crypto_unavailable);
  return std::unique_ptr<ForwardedSourceReceiver>{new ForwardedSourceReceiver{
      std::make_unique<Impl>(*profile, std::move(direction).value(),
                             std::move(validated).value(), setup.hevc)}};
}

ForwardedSourceReceiver::ForwardedSourceReceiver(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

ForwardedSourceReceiver::~ForwardedSourceReceiver() = default;

auto ForwardedSourceReceiver::stream_info() const -> event::StreamInfoReady {
  return impl_->stream_info();
}

auto ForwardedSourceReceiver::push(takion::SourceMedia media,
                                   std::uint64_t key_position,
                                   std::span<const std::byte> datagram,
                                   std::uint64_t now_us)
    -> Expected<void, ForwardedSourceError> {
  return impl_->push(media, key_position, datagram, now_us);
}

auto ForwardedSourceReceiver::poll(std::uint64_t now_us)
    -> Expected<void, ForwardedSourceError> {
  return impl_->poll(now_us);
}

auto ForwardedSourceReceiver::next_deadline_us() const noexcept
    -> std::optional<std::uint64_t> {
  return impl_->next_deadline_us();
}

auto ForwardedSourceReceiver::take() -> std::optional<event::EncodedMedia> {
  return impl_->take();
}

auto ForwardedSourceReceiver::take_idr_request() noexcept -> bool {
  return impl_->take_idr_request();
}

}  // namespace libchiaking::net::protocol::remote_play
