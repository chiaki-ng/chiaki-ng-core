#include "src/core/play/adapters/links/cloud/cloud_messaging.hpp"

#include "src/core/utils/bytes/endian.hpp"
#include "src/core/utils/json/json.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <glaze/util/key_transformers.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <utility>

namespace libchiaking::play::cloud_messaging_wire {

struct Address {
  std::uint32_t addr{};
  std::uint32_t version{};
  std::uint32_t id{};
  std::uint32_t result{};
};

struct Header {
  std::uint32_t msg_type{};
  std::uint32_t action{};
  std::uint32_t sender{};
  std::uint32_t pid{};
  std::optional<Address> data_header{};
  std::optional<Address> result_header{};
};

struct OutgoingAddress {
  std::uint32_t addr{};
  std::uint32_t id{};
  std::uint32_t version{};
};

struct OutgoingHeader {
  std::uint32_t action{};
  OutgoingAddress data_header{};
  std::uint32_t msg_type{};
  std::uint32_t pid{};
  std::uint32_t sender{};
};

struct Text {
  std::optional<std::string> body{};
};

struct NoticeView {
  std::optional<Text> message{};
  std::optional<Text> sub_message{};
};

struct TrophyParameters {
  std::optional<std::string> trophy_grade{};
};

struct Telemetry {
  std::optional<TrophyParameters> parameters{};
};

struct RawNotice {
  std::optional<std::string> use_case_id{};
  std::optional<NoticeView> view_data{};
  std::optional<Telemetry> telemetry_data{};
};

struct NoticePayload {
  std::optional<std::string> use_case_id{};
  std::optional<NoticeView> view_data{};
  std::optional<RawNotice> raw_data{};
};

struct Notification {
  std::optional<NoticePayload> payload{};
};

struct BannerView {
  std::optional<std::string> text{};
};

struct BannerData {
  std::optional<std::string> event_type{};
  std::optional<BannerView> view_data{};
};

struct Banner {
  std::optional<std::string> type{};
  std::optional<BannerData> data{};
};

struct CaptureEvent {
  std::optional<std::string> type{};
  std::optional<std::string> local_ugc_id{};
};

struct CaptureChunk {
  std::optional<std::string> created_date{};
  std::optional<std::string> local_ugc_id{};
  std::optional<std::uint64_t> duration{};
};

struct CaptureUpload {
  std::optional<std::string> content_ext{};
  std::optional<std::uint64_t> content_size{};
  std::optional<std::uint32_t> content_type{};
  std::optional<std::string> created_date{};
  std::optional<std::string> local_ugc_id{};
  std::optional<std::string> upload_ugc_id{};
};

}  // namespace libchiaking::play::cloud_messaging_wire

template <class Wire>
  requires(
      std::same_as<Wire, libchiaking::play::cloud_messaging_wire::Header> ||
      std::same_as<Wire,
                   libchiaking::play::cloud_messaging_wire::OutgoingHeader> ||
      std::same_as<Wire, libchiaking::play::cloud_messaging_wire::NoticeView> ||
      std::same_as<Wire,
                   libchiaking::play::cloud_messaging_wire::TrophyParameters> ||
      std::same_as<Wire, libchiaking::play::cloud_messaging_wire::RawNotice> ||
      std::same_as<Wire,
                   libchiaking::play::cloud_messaging_wire::NoticePayload> ||
      std::same_as<Wire, libchiaking::play::cloud_messaging_wire::BannerData> ||
      std::same_as<Wire,
                   libchiaking::play::cloud_messaging_wire::CaptureEvent> ||
      std::same_as<Wire,
                   libchiaking::play::cloud_messaging_wire::CaptureChunk> ||
      std::same_as<Wire,
                   libchiaking::play::cloud_messaging_wire::CaptureUpload>)
struct glz::meta<Wire> {
  static constexpr auto rename_key(const auto key) -> std::string {
    return glz::to_camel_case(key);
  }
};

namespace libchiaking::play {

auto CloudMessaging::json_text(std::span<const std::byte> body)
    -> std::string_view {
  std::string_view text{reinterpret_cast<const char*>(body.data()),
                        body.size()};
  while (!text.empty() && text.back() == '\0') text.remove_suffix(1);
  return text;
}

auto CloudCaptures::field_text(std::span<const std::byte> body, std::size_t at,
                               std::size_t size) -> std::string_view {
  if (at >= body.size()) return {};
  const auto field = body.subspan(at, std::min(size, body.size() - at));
  const auto end = std::ranges::find(field, std::byte{0});
  return {reinterpret_cast<const char*>(field.data()),
          static_cast<std::size_t>(end - field.begin())};
}

auto CloudMessaging::notice_kind(std::string_view use_case)
    -> std::optional<CloudNoticeKind> {
  constexpr std::array<std::pair<std::string_view, CloudNoticeKind>, 11> kinds{{
      {"NUC9", CloudNoticeKind::screenshot_saved},
      {"NUC8", CloudNoticeKind::screenshot_refused},
      {"NUC7", CloudNoticeKind::recording_saved},
      {"NUC6", CloudNoticeKind::recording_refused},
      {"NUC4", CloudNoticeKind::recording_paused},
      {"NUC5", CloudNoticeKind::recording_resumed},
      {"NUC435", CloudNoticeKind::recording_limit_reached},
      {"NUC507", CloudNoticeKind::captures_uploading},
      {"NUC610", CloudNoticeKind::captures_uploading},
      {"NUC55", CloudNoticeKind::trophy_earned},
      {"NUC559", CloudNoticeKind::trial_time_left},
  }};
  const auto found = std::ranges::find(
      kinds, use_case, &std::pair<std::string_view, CloudNoticeKind>::first);
  if (found == kinds.end()) return std::nullopt;
  return found->second;
}

auto CloudMessaging::trophy_grade(std::string_view grade) -> TrophyGrade {
  if (grade == "Platinum") return TrophyGrade::platinum;
  if (grade == "Gold") return TrophyGrade::gold;
  if (grade == "Silver") return TrophyGrade::silver;
  return TrophyGrade::bronze;
}

auto CloudMessaging::same_addresses(std::vector<std::uint32_t> left,
                                    std::vector<std::uint32_t> right) -> bool {
  std::ranges::sort(left);
  std::ranges::sort(right);
  return left == right;
}

CloudMessaging::CloudMessaging(std::vector<std::uint32_t> receivers)
    : receivers_(std::move(receivers)) {
  if (receivers_.size() > max_addresses) receivers_.resize(max_addresses);
}

auto CloudMessaging::open() -> Transfer {
  return handshake(list_type, receivers_);
}

auto CloudMessaging::allows(std::uint32_t address) const noexcept -> bool {
  return address == sync_address ||
         std::ranges::find(remote_, address) != remote_.end();
}

auto CloudMessaging::message(std::uint32_t address, std::uint32_t version,
                             std::vector<std::byte> body) -> Transfer {
  if (body.size() > max_body) body.resize(max_body);
  return Transfer{data_header(address, version, next_id_++), std::move(body)};
}

auto CloudMessaging::data_header(std::uint32_t address, std::uint32_t version,
                                 std::uint32_t id) -> std::string {
  const auto written = json::ps::write(cloud_messaging_wire::OutgoingHeader{
      .action = 3,
      .data_header = {.addr = address, .id = id, .version = version},
      .msg_type = data_type,
      .pid = 0,
      .sender = 1});
  return written ? std::string{written->view()} : std::string{};
}

auto CloudMessaging::notice(std::span<const std::byte> body)
    -> std::optional<CloudNotice> {
  const auto notification =
      json::read<cloud_messaging_wire::Notification>(json_text(body));
  if (!notification || !notification->payload) return std::nullopt;
  const auto& payload = *notification->payload;
  auto use_case = payload.use_case_id;
  if (!use_case && payload.raw_data) use_case = payload.raw_data->use_case_id;
  const auto kind = use_case ? notice_kind(*use_case) : std::nullopt;
  if (!kind) return std::nullopt;
  CloudNotice notice{*kind, TrophyGrade::bronze, {}, {}};
  if (*kind == CloudNoticeKind::trial_time_left) {
    if (payload.view_data && payload.view_data->message &&
        payload.view_data->message->body)
      notice.title = *payload.view_data->message->body;
    return notice;
  }
  if (*kind != CloudNoticeKind::trophy_earned || !payload.raw_data)
    return notice;
  const auto& trophy = *payload.raw_data;
  if (trophy.telemetry_data && trophy.telemetry_data->parameters &&
      trophy.telemetry_data->parameters->trophy_grade)
    notice.grade =
        trophy_grade(*trophy.telemetry_data->parameters->trophy_grade);
  if (trophy.view_data && trophy.view_data->message &&
      trophy.view_data->message->body)
    notice.title = *trophy.view_data->message->body;
  if (trophy.view_data && trophy.view_data->sub_message &&
      trophy.view_data->sub_message->body)
    notice.detail = *trophy.view_data->sub_message->body;
  return notice;
}

auto CloudMessaging::recording_banner(std::span<const std::byte> body)
    -> std::optional<RecordingBanner> {
  const auto banner = json::read<cloud_messaging_wire::Banner>(json_text(body));
  if (!banner || banner->type != recording_banner_type || !banner->data ||
      !banner->data->event_type)
    return std::nullopt;
  if (*banner->data->event_type == "cancel") return RecordingBanner{};
  if (*banner->data->event_type != "begin") return std::nullopt;
  RecordingBanner recording{true, {}};
  if (banner->data->view_data && banner->data->view_data->text)
    recording.elapsed = *banner->data->view_data->text;
  return recording;
}

auto CloudCaptures::follow(const std::string& id) -> Pending& {
  if (const auto found = pending_.find(id); found != pending_.end())
    return found->second;
  if (pending_.size() >= max_pending) {
    const auto oldest = std::ranges::min_element(
        pending_, {}, [](const auto& entry) { return entry.second.order; });
    pending_.erase(oldest);
  }
  auto& pending = pending_[id];
  pending.capture.id = id;
  pending.order = next_order_++;
  return pending;
}

auto CloudCaptures::chunk(std::span<const std::byte> body, std::uint32_t chunks)
    -> std::optional<CloudCapture> {
  if (body.size() < chunk_data_at || chunks == 0 ||
      chunks > max_thumbnail / 256)
    return std::nullopt;
  const auto index = bytes::load_le<std::uint32_t>(body.subspan(header_size));
  const auto length =
      bytes::load_le<std::uint32_t>(body.subspan(chunk_length_at));
  if (index == 0 || index > chunks || length > body.size() - chunk_data_at)
    return std::nullopt;
  const auto described = json::read<cloud_messaging_wire::CaptureChunk>(
      field_text(body, header_size + 4, chunk_length_at - header_size - 4));
  if (!described || !described->local_ugc_id) return std::nullopt;
  auto& pending = follow(*described->local_ugc_id);
  if (pending.chunks != chunks) {
    pending.chunks = chunks;
    pending.parts.clear();
  }
  if (described->created_date)
    pending.capture.created = *described->created_date;
  pending.capture.video = described->duration.value_or(0) > 0;
  const auto data = body.subspan(chunk_data_at, length);
  pending.parts[index] = std::vector<std::byte>(data.begin(), data.end());
  if (pending.parts.size() != chunks) return std::nullopt;
  std::vector<std::byte> thumbnail;
  for (const auto& [_, part] : pending.parts) {
    if (thumbnail.size() + part.size() > max_thumbnail) return std::nullopt;
    thumbnail.insert(thumbnail.end(), part.begin(), part.end());
  }
  pending.parts.clear();
  auto capture = pending.capture;
  capture.thumbnail = std::move(thumbnail);
  return capture;
}

auto CloudCaptures::receive(std::span<const std::byte> body) -> Received {
  Received received;
  if (body.size() < header_size) return received;
  const auto kind = bytes::load_le<std::uint32_t>(body.subspan(12));
  const auto count = bytes::load_le<std::uint32_t>(body.subspan(16));
  if (kind == thumbnail_kind) {
    received.thumbnail = chunk(body, count);
    return received;
  }
  if (kind == upload_kind) {
    const auto upload = json::read<cloud_messaging_wire::CaptureUpload>(
        field_text(body, header_size, count));
    if (!upload || !upload->local_ugc_id || !upload->upload_ugc_id)
      return received;
    auto& pending = follow(*upload->local_ugc_id);
    pending.upload =
        CloudCaptureUpload{*upload->local_ugc_id,
                           *upload->upload_ugc_id,
                           upload->content_ext.value_or(std::string{}),
                           upload->content_size.value_or(0),
                           upload->content_type.value_or(1) != 1,
                           upload->created_date.value_or(std::string{})};
    return received;
  }
  if (kind != event_kind) return received;
  const auto event = json::read<cloud_messaging_wire::CaptureEvent>(
      field_text(body, header_size, count));
  if (!event || event->type != "complete" || !event->local_ugc_id)
    return received;
  const auto found = pending_.find(*event->local_ugc_id);
  if (found == pending_.end()) return received;
  received.uploaded = found->second.upload;
  pending_.erase(found);
  return received;
}

auto CloudMessaging::capture_body(std::uint64_t account_id, CapturePress press)
    -> std::vector<std::byte> {
  std::vector<std::byte> body(capture_size);
  bytes::store_le<std::uint64_t>(body, account_id);
  bytes::store_le<std::uint32_t>(std::span{body}.subspan(8),
                                 static_cast<std::uint32_t>(press));
  return body;
}

auto CloudMessaging::handshake(std::uint32_t type,
                               std::span<const std::uint32_t> addresses)
    -> Transfer {
  std::vector<std::byte> body(sync_size);
  bytes::store_le<std::uint32_t>(body, type);
  const auto count = std::min(addresses.size(), max_addresses);
  for (std::size_t index = 0; index < count; ++index)
    bytes::store_le<std::uint32_t>(std::span{body}.subspan(4 + index * 4),
                                   addresses[index]);
  return message(sync_address, 1, std::move(body));
}

auto CloudMessaging::handshake_of(std::span<const std::byte> data)
    -> std::optional<std::pair<std::uint32_t, std::vector<std::uint32_t>>> {
  if (data.size() < 4) return std::nullopt;
  std::vector<std::uint32_t> addresses;
  for (std::size_t at = 4;
       at + 4 <= data.size() && addresses.size() < max_addresses; at += 4) {
    const auto address = bytes::load_le<std::uint32_t>(data.subspan(at));
    if (address == 0) break;
    addresses.push_back(address);
  }
  return std::pair{bytes::load_le<std::uint32_t>(data), std::move(addresses)};
}

auto CloudMessaging::receive(std::string_view metadata,
                             std::span<const std::byte> data) -> Received {
  Received received;
  const auto header = json::read<cloud_messaging_wire::Header>(metadata);
  if (!header) return received;
  if (header->msg_type == result_type && header->result_header) {
    const auto& answered = *header->result_header;
    received.result = Result{answered.addr, answered.id, answered.result};
    return received;
  }
  if (header->msg_type != data_type || !header->data_header) return received;
  const auto& addressed = *header->data_header;
  if (addressed.addr != sync_address) {
    received.message =
        Message{addressed.addr, addressed.version, addressed.id,
                std::vector<std::byte>(data.begin(), data.end())};
    return received;
  }
  const auto sync = handshake_of(data);
  if (!sync) return received;
  if (sync->first == list_type) {
    remote_ = sync->second;
    received.replies.push_back(handshake(ack_type, remote_));
  } else if (sync->first == ack_type &&
             !same_addresses(sync->second, receivers_)) {
    received.replies.push_back(handshake(list_type, receivers_));
  }
  return received;
}

}  // namespace libchiaking::play
