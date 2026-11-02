#include "libchiaking/detail/play/session_share.hpp"

#include "libchiaking/detail/diagnostics/session_journal.hpp"
#include "libchiaking/platform/factory.hpp"
#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/net/protocol/remote_play/stream/forwarded_source.hpp"
#include "src/core/play/adapters/links/link_evidence.hpp"
#include "src/core/play/adapters/media/media_delivery.hpp"
#include "src/core/play/adapters/media/media_units.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::play {

class ForwardedStream::Core {
 public:
  Core(std::unique_ptr<net::protocol::remote_play::ForwardedSourceReceiver>
           receiver,
       platform::PlatformFactory& platform, media::MediaSink& media);
  Core(const Core&) = delete;
  auto operator=(const Core&) -> Core& = delete;

  [[nodiscard]] auto describe(
      const std::optional<media::GrantedPicture>& picture)
      -> Expected<void, Refusal>;
  [[nodiscard]] auto receive(const SharedDatagram& datagram)
      -> Expected<void, Refusal>;
  [[nodiscard]] auto service(std::chrono::milliseconds wait)
      -> Expected<std::chrono::milliseconds, Refusal>;
  auto wants_keyframe() noexcept -> bool;
  auto video_presented() noexcept -> media::PresentationLoss;
  void video_rejected(std::uint16_t frame, int cause);
  void video_output_failed(int cause);
  auto fault() const -> std::optional<media::MediaFault>;
  auto statistics() const -> media::VideoStatistics;

  static auto setup(SharedStream shared, ReceiveKeys keys)
      -> net::protocol::remote_play::ForwardedSourceSetup;
  static auto refusal(
      net::protocol::remote_play::ForwardedSourceError error) noexcept
      -> Refusal;
  static auto launch(const media::GrantedPicture& picture) noexcept
      -> net::protocol::remote_play::event::EffectiveLaunch;

 private:
  auto deliver() -> Expected<void, Refusal>;

  platform::ClockProvider& clock_;
  diagnostics::SessionJournal journal_;
  MediaDelivery delivery_;
  std::unique_ptr<net::protocol::remote_play::ForwardedSourceReceiver>
      receiver_;
};

ForwardedStream::Core::Core(
    std::unique_ptr<net::protocol::remote_play::ForwardedSourceReceiver>
        receiver,
    platform::PlatformFactory& platform, media::MediaSink& media)
    : clock_(platform.clock()),
      journal_(platform.threading(), clock_, 1),
      delivery_(media, platform.threading(), clock_,
                platform.supplies(platform::PlatformCapability::diagnostics)
                    ? platform.diagnostics()
                    : static_cast<platform::DiagnosticsProvider&>(journal_)),
      receiver_(std::move(receiver)) {}

auto ForwardedStream::Core::setup(SharedStream shared, ReceiveKeys keys)
    -> net::protocol::remote_play::ForwardedSourceSetup {
  SecretArray<net::protocol::takion::source_key_size> key;
  SecretArray<net::protocol::takion::source_key_size> iv;
  std::ranges::copy(keys.key(), key.begin());
  std::ranges::copy(keys.iv(), iv.begin());
  net::protocol::remote_play::ForwardedSourceSetup wired{
      shared.protocol_version,
      net::protocol::takion::SourceReceiveKeys{std::move(key), std::move(iv)},
      {},
      shared.description.audio_header,
      shared.hevc};
  wired.profiles.reserve(shared.description.profiles.size());
  for (auto& profile : shared.description.profiles)
    wired.profiles.push_back(
        {profile.width, profile.height, std::move(profile.header)});
  return wired;
}

auto ForwardedStream::Core::refusal(
    net::protocol::remote_play::ForwardedSourceError error) noexcept
    -> Refusal {
  switch (error) {
    case net::protocol::remote_play::ForwardedSourceError::invalid_setup:
      return Refusal::invalid_stream;
    case net::protocol::remote_play::ForwardedSourceError::unsupported_version:
      return Refusal::unsupported_version;
    case net::protocol::remote_play::ForwardedSourceError::crypto_unavailable:
      return Refusal::crypto_unavailable;
    case net::protocol::remote_play::ForwardedSourceError::malformed_datagram:
      return Refusal::malformed_datagram;
    case net::protocol::remote_play::ForwardedSourceError::
        authentication_failed:
      return Refusal::authentication_failed;
    case net::protocol::remote_play::ForwardedSourceError::media_failed:
      return Refusal::media_failed;
  }
  return Refusal::media_failed;
}

auto ForwardedStream::Core::launch(
    const media::GrantedPicture& picture) noexcept
    -> net::protocol::remote_play::event::EffectiveLaunch {
  auto codec = net::protocol::remote_play::VideoCodec::h264;
  switch (picture.codec) {
    case media::VideoCodec::h264:
      codec = net::protocol::remote_play::VideoCodec::h264;
      break;
    case media::VideoCodec::h265:
      codec = net::protocol::remote_play::VideoCodec::h265;
      break;
    case media::VideoCodec::h265_hdr:
      codec = net::protocol::remote_play::VideoCodec::h265_hdr;
      break;
  }
  return {codec, picture.width, picture.height, picture.max_fps,
          picture.bitrate_kbps};
}

auto ForwardedStream::Core::describe(
    const std::optional<media::GrantedPicture>& picture)
    -> Expected<void, Refusal> {
  auto info = receiver_->stream_info();
  if (picture) info.launch = launch(*picture);
  if (!delivery_.configure(info)) return unexpected(Refusal::invalid_stream);
  return {};
}

auto ForwardedStream::Core::deliver() -> Expected<void, Refusal> {
  while (auto media = receiver_->take()) {
    if (auto frame = media::VideoIntake::frame(std::move(*media)))
      delivery_.deliver(std::move(*frame));
    else
      delivery_.deliver(std::move(*media));
  }
  if (delivery_.fault()) return unexpected(Refusal::media_failed);
  return {};
}

auto ForwardedStream::Core::receive(const SharedDatagram& datagram)
    -> Expected<void, Refusal> {
  const auto media = datagram.media == SharedDatagram::Media::video
                         ? net::protocol::takion::SourceMedia::video
                         : net::protocol::takion::SourceMedia::audio;
  const auto pushed = receiver_->push(
      media, datagram.key_position, datagram.bytes, clock_.now_microseconds());
  if (!pushed) return unexpected(refusal(pushed.error()));
  return deliver();
}

auto ForwardedStream::Core::service(std::chrono::milliseconds wait)
    -> Expected<std::chrono::milliseconds, Refusal> {
  const auto now = clock_.now_microseconds();
  if (const auto polled = receiver_->poll(now); !polled)
    return unexpected(refusal(polled.error()));
  if (const auto delivered = deliver(); !delivered)
    return unexpected(delivered.error());
  static_cast<void>(delivery_.video().take_loss_report());
  const auto deadline = receiver_->next_deadline_us();
  if (!deadline) return wait;
  if (*deadline <= now) return std::chrono::milliseconds{0};
  const std::chrono::milliseconds remaining{
      static_cast<std::chrono::milliseconds::rep>(
          (*deadline - now + microseconds_per_millisecond - 1) /
          microseconds_per_millisecond)};
  return std::min(wait, remaining);
}

auto ForwardedStream::Core::wants_keyframe() noexcept -> bool {
  const bool lost = receiver_->take_idr_request();
  const bool rejected = delivery_.video().idr_due();
  return lost || rejected;
}

auto ForwardedStream::Core::video_presented() noexcept
    -> media::PresentationLoss {
  const auto loss = delivery_.presented();
  return {loss.frames_lost, loss.recovered};
}

void ForwardedStream::Core::video_rejected(std::uint16_t frame, int cause) {
  delivery_.rejected(frame, cause);
}

void ForwardedStream::Core::video_output_failed(int cause) {
  delivery_.output_failed(cause);
}

auto ForwardedStream::Core::fault() const -> std::optional<media::MediaFault> {
  if (const auto fault = delivery_.fault()) return *fault;
  return std::nullopt;
}

auto ForwardedStream::Core::statistics() const -> media::VideoStatistics {
  const auto wired = delivery_.video().statistics();
  return {wired.units,
          wired.bytes,
          wired.lost,
          wired.recovered,
          wired.decoder_rejections,
          wired.keyframe_requests,
          wired.last_decoder_error};
}

auto ForwardedStream::open(platform::PlatformFactory& platform,
                           SharedStream shared, ReceiveKeys keys,
                           media::MediaSink& media)
    -> Expected<std::unique_ptr<ForwardedStream>, Refusal> {
  if (!platform.supplies(platform::PlatformCapability::crypto))
    return unexpected(Refusal::crypto_unavailable);
  const auto picture = shared.description.picture;
  auto receiver = net::protocol::remote_play::ForwardedSourceReceiver::create(
      platform.crypto(), platform.threading(),
      Core::setup(std::move(shared), std::move(keys)));
  if (!receiver) return unexpected(Core::refusal(receiver.error()));
  auto core =
      std::make_unique<Core>(std::move(receiver).value(), platform, media);
  if (const auto described = core->describe(picture); !described)
    return unexpected(described.error());
  return std::make_unique<ForwardedStream>(std::move(core));
}

ForwardedStream::ForwardedStream(std::unique_ptr<Core> core) noexcept
    : core_(std::move(core)) {}

ForwardedStream::~ForwardedStream() = default;

auto ForwardedStream::receive(const SharedDatagram& datagram)
    -> Expected<void, Refusal> {
  return core_->receive(datagram);
}

auto ForwardedStream::service(std::chrono::milliseconds wait)
    -> Expected<std::chrono::milliseconds, Refusal> {
  return core_->service(wait);
}

auto ForwardedStream::wants_keyframe() noexcept -> bool {
  return core_->wants_keyframe();
}

auto ForwardedStream::video_presented() noexcept -> media::PresentationLoss {
  return core_->video_presented();
}

void ForwardedStream::video_rejected(std::uint16_t frame, int cause) {
  core_->video_rejected(frame, cause);
}

void ForwardedStream::video_output_failed(int cause) {
  core_->video_output_failed(cause);
}

auto ForwardedStream::fault() const -> std::optional<media::MediaFault> {
  return core_->fault();
}

auto ForwardedStream::statistics() const -> media::VideoStatistics {
  return core_->statistics();
}

}  // namespace libchiaking::play
