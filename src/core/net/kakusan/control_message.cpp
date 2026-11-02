#include "src/core/net/kakusan/control_message.hpp"

#include "libchiaking/platform/crypto/provider.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/short_block.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "src/core/utils/json/strict.hpp"
#include "src/core/utils/memory/constant_time.hpp"
#include "src/core/utils/text/base64.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace libchiaking::net::kakusan::control {

static_assert(share_size == platform::p256_point_size);

auto MessageCodec::integer(std::uint64_t value) -> Value {
  return Value{value};
}

template <std::size_t Size>
auto MessageCodec::bytes(const std::array<std::byte, Size>& value) -> Value {
  return Value{text::base64::encode(value, text::base64::Alphabet::url)};
}

auto MessageCodec::secret(const SourceKey& value) -> Value {
  return Value{text::base64::encode<SecretString>(
      std::span<const std::byte>{value.data(), value.size()},
      text::base64::Alphabet::url)};
}

auto MessageCodec::reason_name(Reason reason) noexcept -> std::string_view {
  return reason_names[static_cast<std::size_t>(reason)];
}

void MessageCodec::append_binding(Value::Object& members,
                                  const SeatBinding& binding) {
  members.push_back(Member{"seat", integer(binding.seat)});
  members.push_back(Member{"generation", integer(binding.generation)});
}

template <class Bytes>
auto MessageCodec::read_bytes(ObjectReader& reader, std::string_view key)
    -> std::optional<Bytes> {
  const auto text = reader.string(key);
  if (!text) return std::nullopt;
  auto decoded =
      text::base64::decode_url_exact<std::tuple_size_v<Bytes>>(*text);
  if (!decoded) return std::nullopt;
  return decoded.value();
}

auto MessageCodec::read_key(ObjectReader& reader, std::string_view key)
    -> std::optional<SourceKey> {
  const auto text = reader.string(key);
  if (!text) return std::nullopt;
  SourceKey decoded;
  if (!text::base64::decode_url_into(
          *text, std::span<std::byte>{decoded.data(), decoded.size()}))
    return std::nullopt;
  return decoded;
}

auto MessageCodec::read_fixed(ObjectReader& reader, std::string_view key,
                              std::uint64_t expected) -> bool {
  const auto value = reader.integer(key);
  return value && *value == expected;
}

auto MessageCodec::read_binding(ObjectReader& reader)
    -> std::optional<SeatBinding> {
  const auto seat = reader.integer("seat");
  const auto generation = reader.integer("generation");
  if (!seat || !generation || *seat < 1 || *seat > max_seat_capacity ||
      *generation < 1 || *generation > max_generation)
    return std::nullopt;
  return SeatBinding{static_cast<std::uint8_t>(*seat),
                     static_cast<std::uint32_t>(*generation)};
}

auto MessageCodec::read_reason(ObjectReader& reader) -> std::optional<Reason> {
  const auto text = reader.string("reason");
  if (!text) return std::nullopt;
  const auto found = std::ranges::find(reason_names, *text);
  if (found == reason_names.end()) return std::nullopt;
  return static_cast<Reason>(found - reason_names.begin());
}

auto MessageCodec::read_profile(const Value& value)
    -> std::optional<VideoProfile> {
  const auto* object = value.object();
  if (object == nullptr) return std::nullopt;
  auto made = ObjectReader::create(*object);
  if (!made) return std::nullopt;
  auto& reader = made.value();
  const auto width = reader.integer("width");
  const auto height = reader.integer("height");
  const auto header = reader.string("header");
  if (!width || !height || !header || !reader.finish()) return std::nullopt;
  auto decoded = text::base64::decode_url(*header);
  if (!decoded) return std::nullopt;
  constexpr std::uint64_t widest = std::numeric_limits<std::uint16_t>::max();
  return VideoProfile{static_cast<std::uint16_t>(std::min(*width, widest)),
                      static_cast<std::uint16_t>(std::min(*height, widest)),
                      std::move(decoded).value()};
}

template <class Kind>
auto MessageCodec::write(Value::Object& members, const Kind& message)
    -> Result<void> {
  if constexpr (requires { Kind::wire_field; }) {
    const auto& [value] = message;
    members.push_back(Member{std::string{Kind::wire_field}, bytes(value)});
  }
  if constexpr (requires { message.binding; }) {
    if (!valid_binding(message.binding)) return fail(Error::protocol);
    append_binding(members, message.binding);
  }
  if constexpr (requires { message.reason; })
    members.push_back(Member{"reason", Value{reason_name(message.reason)}});
  return {};
}

template <>
auto MessageCodec::write(Value::Object& members, const Identity& message)
    -> Result<void> {
  members.push_back(Member{"identity_key", bytes(message.key)});
  members.push_back(Member{"sig", bytes(message.signature)});
  if (message.hosting) {
    members.push_back(Member{"session_id", bytes(message.hosting->session_id)});
    members.push_back(
        Member{"local_seats", integer(message.hosting->local_seats)});
    members.push_back(
        Member{"remote_seats", integer(message.hosting->remote_seats)});
    members.push_back(
        Member{"reserved_seats", integer(message.hosting->reserved_seats)});
  }
  return {};
}

template <>
auto MessageCodec::write(Value::Object& members, const AdmissionPending&)
    -> Result<void> {
  members.push_back(Member{"timeout_ms", integer(admission_timeout_ms)});
  return {};
}

template <>
auto MessageCodec::write(Value::Object& members, const SourceSetup& message)
    -> Result<void> {
  if (!message.well_formed()) return fail(Error::protocol);
  Value::Array profiles;
  profiles.reserve(message.video_profiles.size());
  for (const auto& profile : message.video_profiles) {
    Value::Object item;
    item.push_back(Member{"width", integer(profile.width)});
    item.push_back(Member{"height", integer(profile.height)});
    item.push_back(
        Member{"header", Value{text::base64::encode(
                             profile.header, text::base64::Alphabet::url)}});
    profiles.push_back(Value{std::move(item)});
  }
  members.push_back(Member{"epoch", integer(source_epoch)});
  members.push_back(
      Member{"source_session_id", bytes(message.source_session_id)});
  members.push_back(
      Member{"source_protocol", integer(message.source_protocol)});
  members.push_back(Member{"key_policy", Value{key_policy}});
  members.push_back(Member{"receive_key", secret(message.receive_key)});
  members.push_back(Member{"receive_iv", secret(message.receive_iv)});
  members.push_back(Member{
      "source_codec",
      Value{message.source_codec == SourceCodec::h265 ? source_codec_h265
                                                      : source_codec_h264}});
  members.push_back(Member{"video_fps", integer(message.video_fps)});
  members.push_back(Member{"video_profiles", Value{std::move(profiles)}});
  members.push_back(Member{"audio_header", bytes(message.audio_header)});
  return {};
}

template <>
auto MessageCodec::write(Value::Object& members, const SourceReady& message)
    -> Result<void> {
  members.push_back(Member{"epoch", integer(source_epoch)});
  members.push_back(Member{"setup_hash", bytes(message.setup_hash)});
  return {};
}

template <>
auto MessageCodec::write(Value::Object& members, const SourceIdr&)
    -> Result<void> {
  members.push_back(Member{"epoch", integer(source_epoch)});
  return {};
}

template <>
auto MessageCodec::write(Value::Object& members, const SeatGranted& message)
    -> Result<void> {
  if (!valid_grant(message)) return fail(Error::protocol);
  append_binding(members, message.binding);
  members.push_back(Member{"capacity", integer(message.capacity)});
  members.push_back(Member{"local_seats", integer(message.local_seats)});
  members.push_back(Member{"input_hz", integer(input_rate_hz)});
  members.push_back(
      Member{"allowed_buttons", integer(message.allowed_buttons)});
  return {};
}

template <>
auto MessageCodec::write(Value::Object& members, const Failure& message)
    -> Result<void> {
  if (std::ranges::find(wire_errors, message.code) == wire_errors.end())
    return fail(Error::protocol);
  members.push_back(Member{"code", Value{wire_code(message.code)}});
  return {};
}

template <class Kind>
auto MessageCodec::read(ObjectReader& reader) -> Result<Message> {
  Kind message;
  if constexpr (requires { Kind::wire_field; }) {
    auto& [value] = message;
    auto decoded = read_bytes<std::remove_cvref_t<decltype(value)>>(
        reader, Kind::wire_field);
    if (!decoded) return fail(Error::protocol);
    value = *decoded;
  }
  if constexpr (requires { message.binding; }) {
    const auto binding = read_binding(reader);
    if (!binding) return fail(Error::protocol);
    message.binding = *binding;
  }
  if constexpr (requires { message.reason; }) {
    const auto reason = read_reason(reader);
    if (!reason) return fail(Error::protocol);
    message.reason = *reason;
  }
  if (!reader.finish()) return fail(Error::protocol);
  return Message{std::move(message)};
}

template <>
auto MessageCodec::read<Identity>(ObjectReader& reader) -> Result<Message> {
  auto key = read_bytes<IdentityKey>(reader, "identity_key");
  auto signature = read_bytes<Signature>(reader, "sig");
  if (!key || !signature) return fail(Error::protocol);
  Identity identity{*key, *signature, std::nullopt};
  if (reader.has("session_id")) {
    const auto session = read_bytes<SessionId>(reader, "session_id");
    const auto local = reader.integer("local_seats");
    const auto remote = reader.integer("remote_seats");
    const auto reserved = reader.integer("reserved_seats");
    if (!session || !local || !remote || !reserved ||
        *local > max_seat_capacity || *remote > max_seat_capacity ||
        *reserved > max_seat_capacity)
      return fail(Error::protocol);
    identity.hosting =
        Identity::Hosting{*session, static_cast<std::uint8_t>(*local),
                          static_cast<std::uint8_t>(*remote),
                          static_cast<std::uint8_t>(*reserved)};
  }
  if (!reader.finish()) return fail(Error::protocol);
  return Message{identity};
}

template <>
auto MessageCodec::read<AdmissionPending>(ObjectReader& reader)
    -> Result<Message> {
  if (!read_fixed(reader, "timeout_ms", admission_timeout_ms) ||
      !reader.finish())
    return fail(Error::protocol);
  return Message{AdmissionPending{}};
}

template <>
auto MessageCodec::read<SourceSetup>(ObjectReader& reader) -> Result<Message> {
  const bool epoch = read_fixed(reader, "epoch", source_epoch);
  auto session = read_bytes<SourceSessionId>(reader, "source_session_id");
  auto receive_key = read_key(reader, "receive_key");
  auto receive_iv = read_key(reader, "receive_iv");
  auto audio = read_bytes<AudioHeader>(reader, "audio_header");
  const auto protocol = reader.integer("source_protocol");
  const auto policy = reader.string("key_policy");
  const auto codec = reader.string("source_codec");
  const auto fps = reader.integer("video_fps");
  const auto profiles = reader.array("video_profiles");
  if (!epoch || !session || !receive_key || !receive_iv || !audio ||
      !protocol || !policy || !codec || !fps || !profiles || !reader.finish() ||
      *policy != key_policy || !SourceSetup::supported_protocol(*protocol))
    return fail(Error::protocol);
  if (*codec != source_codec_h264 && *codec != source_codec_h265)
    return fail(Error::codec);
  SourceSetup setup;
  setup.source_session_id = *session;
  setup.source_protocol = static_cast<std::uint8_t>(*protocol);
  setup.receive_key = std::move(*receive_key);
  setup.receive_iv = std::move(*receive_iv);
  setup.video_fps = static_cast<std::uint8_t>(
      std::min<std::uint64_t>(*fps, std::numeric_limits<std::uint8_t>::max()));
  setup.audio_header = *audio;
  setup.source_codec =
      *codec == source_codec_h265 ? SourceCodec::h265 : SourceCodec::h264;
  if (profiles->size() > max_video_profiles) return fail(Error::codec);
  for (const auto& item : *profiles) {
    auto profile = read_profile(item);
    if (!profile) return fail(Error::codec);
    setup.video_profiles.push_back(std::move(*profile));
  }
  if (!setup.well_formed()) return fail(Error::codec);
  return Message{std::move(setup)};
}

template <>
auto MessageCodec::read<SourceReady>(ObjectReader& reader) -> Result<Message> {
  const bool epoch = read_fixed(reader, "epoch", source_epoch);
  const auto hash = read_bytes<Digest256>(reader, "setup_hash");
  if (!epoch || !hash || !reader.finish()) return fail(Error::protocol);
  return Message{SourceReady{*hash}};
}

template <>
auto MessageCodec::read<SourceIdr>(ObjectReader& reader) -> Result<Message> {
  if (!read_fixed(reader, "epoch", source_epoch) || !reader.finish())
    return fail(Error::protocol);
  return Message{SourceIdr{}};
}

template <>
auto MessageCodec::read<SeatGranted>(ObjectReader& reader) -> Result<Message> {
  const auto binding = read_binding(reader);
  const auto capacity = reader.integer("capacity");
  const auto local = reader.integer("local_seats");
  const auto buttons = reader.integer("allowed_buttons");
  if (!binding || !capacity || !local || !buttons ||
      !read_fixed(reader, "input_hz", input_rate_hz) || !reader.finish() ||
      *capacity > max_seat_capacity || *local > max_seat_capacity ||
      *buttons > button_mask)
    return fail(Error::protocol);
  const SeatGranted grant{*binding, static_cast<std::uint8_t>(*capacity),
                          static_cast<std::uint8_t>(*local),
                          static_cast<std::uint32_t>(*buttons)};
  if (!valid_grant(grant)) return fail(Error::protocol);
  return Message{grant};
}

template <>
auto MessageCodec::read<Failure>(ObjectReader& reader) -> Result<Message> {
  const auto code = reader.string("code");
  if (!code || !reader.finish()) return fail(Error::protocol);
  const auto known = std::ranges::find(wire_errors, *code, wire_code);
  if (known == wire_errors.end()) return fail(Error::protocol);
  return Message{Failure{*known}};
}

template <std::size_t... Index>
constexpr auto MessageCodec::entries_of(std::index_sequence<Index...>) noexcept
    -> std::array<Entry, sizeof...(Index)> {
  return {Entry{std::variant_alternative_t<Index, Message>::wire_name,
                &read<std::variant_alternative_t<Index, Message>>}...};
}

const std::array<MessageCodec::Entry, std::variant_size_v<Message>>
    MessageCodec::entries = MessageCodec::entries_of(
        std::make_index_sequence<std::variant_size_v<Message>>{});

template <class Text>
auto MessageCodec::bounded(Text text) -> Result<Text> {
  if (text.size() > max_control_message_size) return fail(Error::protocol);
  return text;
}

auto MessageCodec::object(const Message& message) -> Result<Value> {
  Value::Object members;
  members.push_back(Member{"type", Value{type_name(message)}});
  return std::visit(
             [&members](const auto& kind) { return write(members, kind); },
             message)
      .transform([&members] { return Value{std::move(members)}; });
}

auto MessageCodec::type_name(const Message& message) noexcept
    -> std::string_view {
  return entries[message.index()].name;
}

auto MessageCodec::encode_secret(const Message& message)
    -> Result<SecretString> {
  return object(message).and_then(
      [](const Value& value) { return bounded(value.canonical_secret()); });
}

auto MessageCodec::encode(const Message& message) -> Result<std::string> {
  return object(message).and_then(
      [](const Value& value) -> Result<std::string> {
        auto text = value.canonical();
        if (!text) return fail(Error::internal);
        return bounded(std::move(*text));
      });
}

auto MessageCodec::decode(std::string_view text) -> Result<Message> {
  if (text.empty() || text.size() > max_control_message_size)
    return fail(Error::protocol);
  const auto parsed = json::strict::Value::parse(text);
  if (!parsed) return fail(Error::protocol);
  return decode(*parsed);
}

auto MessageCodec::decode(const json::strict::Value& document)
    -> Result<Message> {
  const auto* members = document.object();
  if (members == nullptr) return fail(Error::protocol);
  auto made = ObjectReader::create(*members);
  if (!made) return fail(Error::protocol);
  auto& reader = made.value();
  const auto type = reader.string("type");
  if (!type) return fail(Error::protocol);
  const auto entry = std::ranges::find(entries, *type, &Entry::name);
  if (entry == entries.end()) return fail(Error::protocol);
  return entry->read(reader);
}

auto MessageCodec::setup_hash(platform::CryptoProvider& crypto,
                              const SourceSetup& setup) -> Result<Digest256> {
  auto encoded = encode_secret(Message{setup.clone()});
  if (!encoded) return fail(encoded.error());
  return Hash::sha256(crypto, encoded.value().bytes());
}

}  // namespace libchiaking::net::kakusan::control
