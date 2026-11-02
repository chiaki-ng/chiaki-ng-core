#include "ctrl_codec.hpp"
#include "src/core/utils/bytes/byte_io.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <string_view>
#include <utility>

#include "libchiaking/detail/users/account_id.hpp"
#include "libchiaking/detail/wire_sizes.hpp"

#include "limits.hpp"
#include "src/core/net/protocol/remote_play/console_reply.hpp"
#include "src/core/net/protocol/remote_play/pairing/cipher.hpp"
#include "src/core/net/transport/http/status.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/base64.hpp"
#include "src/core/utils/text/integer.hpp"

namespace libchiaking::net::protocol::remote_play {
auto CtrlCodec::text_from(std::span<const std::byte> payload,
                          std::size_t offset, std::uint32_t length)
    -> std::optional<std::string> {
  bytes::Reader reader{payload};
  if (!reader.skip(offset)) return std::nullopt;
  const auto taken = reader.take(length);
  if (!taken) return std::nullopt;
  std::string text(taken->size(), '\0');
  std::ranges::transform(*taken, text.begin(), to_char);
  return text;
}

auto CtrlCodec::declared_length(std::span<const std::byte> payload,
                                std::size_t offset) noexcept -> std::uint32_t {
  return bytes::load_be<std::uint32_t>(payload.subspan(offset));
}

auto CtrlCodec::to_char(std::byte value) noexcept -> char {
  return static_cast<char>(value);
}

auto CtrlCodec::malformed(std::uint16_t type,
                          std::span<const std::byte> payload) -> CtrlNotice {
  return CtrlMalformedNotice{type, payload.size()};
}

auto CtrlCodec::valid_ctrl_pad(std::uint8_t pad) noexcept -> bool {
  const auto seat = Pad::create(pad);
  return seat && seat->couch();
}

struct CtrlCodec::TargetFacts {
  std::string_view path;
  std::string_view version;
  bool bitrate;
  bool streaming_type;
  bool pad_processing;
};

auto CtrlCodec::facts(RegistrationTarget target)
    -> Expected<TargetFacts, CtrlRequestError> {
  const auto version = rp_version_string(target);
  if (!version) return unexpected(CtrlRequestError::invalid_target);
  switch (target) {
    case RegistrationTarget::ps4_8:
    case RegistrationTarget::ps4_9:
      return TargetFacts{rp_ctrl_path_ps4_legacy, *version, false, false,
                         false};
    case RegistrationTarget::ps4_10:
      return TargetFacts{rp_ctrl_path_ps4, *version, true, false, false};
    case RegistrationTarget::ps5_1:
      return TargetFacts{rp_ctrl_path_ps5, *version, true, true, true};
    default:
      return unexpected(CtrlRequestError::invalid_target);
  }
}

auto CtrlCodec::encrypted_base64(platform::CryptoProvider& crypto,
                                 RegistrationTarget target,
                                 const AuthMaterial& material,
                                 std::uint64_t counter,
                                 std::span<const std::byte> plain)
    -> Expected<EncryptedField, CtrlRequestError> {
  SecretArray<rp_did_size> encrypted;
  const auto sealed = std::span{encrypted.data(), plain.size()};
  if (!material.transform(crypto, target, counter, CipherDirection::encrypt,
                          plain, sealed))
    return unexpected(CtrlRequestError::crypto_failure);
  EncryptedField field;
  const auto written =
      text::base64::encode_into(sealed, {field.text.data(), field.text.size()},
                                text::base64::Alphabet::standard);
  if (!written) return unexpected(CtrlRequestError::overflow);
  field.size = *written;
  return field;
}

auto CtrlCodec::encode_frame_header(std::uint16_t type,
                                    std::size_t payload_size) noexcept
    -> Expected<std::array<std::byte, ctrl_frame_header_size>, CtrlFrameError> {
  if (payload_size > ctrl_frame_max_outbound_payload)
    return unexpected(CtrlFrameError::too_large);
  std::array<std::byte, ctrl_frame_header_size> header{};
  bytes::store_be<std::uint32_t>(header,
                                 static_cast<std::uint32_t>(payload_size));
  bytes::store_be<std::uint16_t>(
      std::span{header}.subspan(sizeof(std::uint32_t)), type);
  return header;
}

auto CtrlCodec::decode_frame(std::span<const std::byte> bytes) noexcept
    -> Expected<CtrlFrameView, CtrlFrameError> {
  if (bytes.size() < ctrl_frame_header_size)
    return unexpected(CtrlFrameError::incomplete);
  const auto length = bytes::load_be<std::uint32_t>(bytes);
  if (length > ctrl_frame_max_payload)
    return unexpected(CtrlFrameError::too_large);
  if (bytes.size() - ctrl_frame_header_size < length)
    return unexpected(CtrlFrameError::incomplete);
  return CtrlFrameView{
      bytes::load_be<std::uint16_t>(bytes.subspan(sizeof(std::uint32_t))),
      bytes.subspan(ctrl_frame_header_size, length),
      ctrl_frame_header_size + length,
  };
}

auto CtrlCodec::decode_message(std::uint16_t type,
                               std::span<const std::byte> payload)
    -> CtrlMessage {
  switch (static_cast<CtrlMessageType>(type)) {
    case CtrlMessageType::session_id: {
      auto id = SessionId::parse(payload);
      if (!id) return CtrlSessionIdFallback{id.error()};
      return CtrlSessionId{std::move(id.value())};
    }
    case CtrlMessageType::heartbeat_req:
      return CtrlHeartbeatRequest{!payload.empty()};
    case CtrlMessageType::login_pin_req:
      return CtrlLoginPinRequest{!payload.empty()};
    case CtrlMessageType::login:
      if (!payload.empty() && payload[0] == std::byte{0})
        return CtrlLoginSuccess{};
      if (!payload.empty() && payload[0] == std::byte{1})
        return CtrlLoginPinIncorrect{};
      return CtrlIgnoredMessage{type};
    case CtrlMessageType::switch_to_stream_connection:
      return CtrlStreamSwitchAck{!payload.empty()};
    default:
      if (auto notice = decode_notice(type, payload))
        return CtrlNoticeMessage{std::move(*notice)};
      return CtrlIgnoredMessage{type};
  }
}

auto CtrlCodec::decode_notice(std::uint16_t type,
                              std::span<const std::byte> payload)
    -> std::optional<CtrlNotice> {
  switch (static_cast<CtrlMessageType>(type)) {
    case CtrlMessageType::keyboard_open: {
      if (payload.size() < keyboard_open_header_size)
        return malformed(type, payload);
      auto text =
          text_from(payload, keyboard_open_header_size,
                    declared_length(payload, keyboard_open_length_offset));
      if (!text) return malformed(type, payload);
      return CtrlKeyboardOpen{std::move(*text)};
    }
    case CtrlMessageType::keyboard_close_remote:
      return CtrlKeyboardRemoteClose{};
    case CtrlMessageType::keyboard_text_change_res: {
      if (payload.size() < keyboard_text_response_header_size)
        return malformed(type, payload);
      auto text = text_from(
          payload, keyboard_text_response_header_size,
          declared_length(payload, keyboard_text_response_length_offset));
      if (!text) return malformed(type, payload);
      return CtrlKeyboardTextChange{std::move(*text)};
    }
    case CtrlMessageType::displaya:
      return one_byte_notice<CtrlDisplayA>(type, payload);
    case CtrlMessageType::displayb:
      return two_byte_notice<CtrlDisplayB>(type, payload);
    case CtrlMessageType::couch_user_code: {
      if (payload.empty()) return malformed(type, payload);
      CtrlCouchAccountUnresolvable refusal{
          std::to_integer<std::uint8_t>(payload.front()), {}};
      for (const auto value :
           payload.subspan(1) | std::views::transform(to_char))
        if (text::ascii_printable(value)) refusal.printable.push_back(value);
      return refusal;
    }
    case CtrlMessageType::couch_passcode_req:
      return one_byte_notice<CtrlCouchPasscodeRequest>(type, payload);
    case CtrlMessageType::couch_user_join_rep:
      return two_byte_notice<CtrlCouchUserJoinReply>(type, payload);
    case CtrlMessageType::couch_pad_join_rep:
      return two_byte_notice<CtrlCouchPadJoinReply>(type, payload);
    case CtrlMessageType::couch_pad_dropped:
      return one_byte_notice<CtrlCouchPadDropped>(type, payload);
    case CtrlMessageType::couch_leave_rep:
      return two_byte_notice<CtrlCouchLeaveReply>(type, payload);
    default:
      return std::nullopt;
  }
}

auto CtrlCodec::encode_goto_bed() -> CtrlOutboundFrame {
  return {message_type_id(CtrlMessageType::goto_bed), {}};
}

auto CtrlCodec::encode_go_home() -> CtrlOutboundFrame {
  CtrlOutboundFrame frame{message_type_id(CtrlMessageType::go_home),
                          std::vector<std::byte>(0x10)};
  frame.payload[1] = std::byte{0xff};
  return frame;
}

auto CtrlCodec::encode_keyboard_enable() -> std::array<CtrlOutboundFrame, 2> {
  CtrlOutboundFrame signature{message_type_id(CtrlMessageType::keyboard_enable),
                              std::vector<std::byte>(0x10)};
  signature.payload[3] = std::byte{0x01};
  signature.payload[6] = std::byte{0x05};
  signature.payload[7] = std::byte{0xae};
  return {std::move(signature),
          CtrlOutboundFrame{
              message_type_id(CtrlMessageType::keyboard_enable_toggle),
              {std::byte{1}}}};
}

auto CtrlCodec::encode_keyboard_set_text(std::uint32_t counter,
                                         std::string_view text)
    -> Expected<CtrlOutboundFrame, CtrlFeatureCodecError> {
  if (text.size() >
      ctrl_frame_max_outbound_payload - keyboard_text_request_header_size)
    return unexpected(CtrlFeatureCodecError::too_large);
  const auto length = static_cast<std::uint32_t>(text.size());
  CtrlOutboundFrame frame{
      message_type_id(CtrlMessageType::keyboard_text_change_req),
      std::vector<std::byte>(keyboard_text_request_header_size + text.size())};
  bytes::store_be<std::uint32_t>(std::span{frame.payload}.subspan(0), counter);
  bytes::store_be<std::uint32_t>(std::span{frame.payload}.subspan(4), length);
  bytes::store_be<std::uint32_t>(std::span{frame.payload}.subspan(0x20),
                                 length);
  std::ranges::copy(std::as_bytes(std::span{text}),
                    frame.payload.begin() + keyboard_text_request_header_size);
  return frame;
}

auto CtrlCodec::encode_keyboard_accept() -> CtrlOutboundFrame {
  return {message_type_id(CtrlMessageType::keyboard_close_req),
          std::vector<std::byte>(4)};
}

auto CtrlCodec::encode_keyboard_reject() -> CtrlOutboundFrame {
  return {message_type_id(CtrlMessageType::keyboard_close_req),
          {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}}};
}

auto CtrlCodec::valid_couch_account_id(std::string_view account_id) -> bool {
  return AccountId::from_base64(account_id).has_value();
}

auto CtrlCodec::encode_couch_user_join(std::uint8_t pad,
                                       std::string_view account_id)
    -> Expected<CtrlOutboundFrame, CtrlFeatureCodecError> {
  if (!valid_ctrl_pad(pad))
    return unexpected(CtrlFeatureCodecError::invalid_pad);
  if (account_id.empty())
    return unexpected(CtrlFeatureCodecError::invalid_account_id);
  const auto decoded = AccountId::from_base64(account_id);
  if (!decoded) return unexpected(CtrlFeatureCodecError::invalid_account_id);
  CtrlOutboundFrame frame{message_type_id(CtrlMessageType::couch_user_join_req),
                          std::vector<std::byte>(16)};
  std::ranges::copy(decoded->bytes(), frame.payload.begin());
  frame.payload[8] = std::byte{pad};
  frame.payload[9] = std::byte{couch_default_controller_type};
  return frame;
}

auto CtrlCodec::encode_couch_pad_join(std::uint8_t pad,
                                      std::uint8_t controller_type)
    -> Expected<CtrlOutboundFrame, CtrlFeatureCodecError> {
  if (!valid_ctrl_pad(pad))
    return unexpected(CtrlFeatureCodecError::invalid_pad);
  return CtrlOutboundFrame{
      message_type_id(CtrlMessageType::couch_pad_join_req),
      {std::byte{pad},
       std::byte{controller_type ? controller_type
                                 : couch_default_controller_type}}};
}

auto CtrlCodec::encode_couch_leave(std::uint8_t pad)
    -> Expected<CtrlOutboundFrame, CtrlFeatureCodecError> {
  if (!valid_ctrl_pad(pad))
    return unexpected(CtrlFeatureCodecError::invalid_pad);
  return CtrlOutboundFrame{message_type_id(CtrlMessageType::couch_leave_req),
                           {std::byte{pad}}};
}

auto CtrlCodec::encode_couch_passcode(std::uint8_t pad,
                                      std::string_view passcode)
    -> Expected<CtrlOutboundFrame, CtrlFeatureCodecError> {
  if (!valid_ctrl_pad(pad))
    return unexpected(CtrlFeatureCodecError::invalid_pad);
  if (passcode.size() != couch_passcode_digits ||
      !std::ranges::all_of(passcode, text::ascii_digit))
    return unexpected(CtrlFeatureCodecError::invalid_passcode);
  CtrlOutboundFrame frame{message_type_id(CtrlMessageType::couch_passcode_rep),
                          std::vector<std::byte>(8)};
  frame.payload[0] = std::byte{pad};
  std::ranges::copy(std::as_bytes(std::span{passcode}),
                    frame.payload.begin() + 1);
  return frame;
}

auto CtrlCodec::streaming_type(VideoCodec codec) noexcept
    -> std::optional<std::uint32_t> {
  switch (codec) {
    case VideoCodec::h264:
      return rp_streaming_type_h264;
    case VideoCodec::h265:
      return rp_streaming_type_h265;
    case VideoCodec::h265_hdr:
      return rp_streaming_type_h265_hdr;
  }
  return std::nullopt;
}

auto CtrlCodec::encode_startup_request(
    platform::CryptoProvider& crypto, RegistrationTarget target,
    const AuthMaterial& material, std::string_view host, std::uint16_t port,
    std::span<const std::byte> registration_key, std::span<const std::byte> did,
    VideoCodec codec) -> Expected<CtrlStartupRequest, CtrlRequestError> {
  const auto target_facts = facts(target);
  if (!target_facts) return unexpected(target_facts.error());
  if (!CtrlStartupRequest::Head::accepts_host(host))
    return unexpected(CtrlRequestError::invalid_host);
  if (port == 0) return unexpected(CtrlRequestError::invalid_port);
  if (registration_key.size() != rp_registration_key_size)
    return unexpected(CtrlRequestError::invalid_key);
  if (did.size() != rp_did_size)
    return unexpected(CtrlRequestError::invalid_did);
  const auto streaming = streaming_type(codec);
  if (!streaming) return unexpected(CtrlRequestError::invalid_codec);

  const auto field = [&](std::uint64_t counter,
                         std::span<const std::byte> plain) {
    return encrypted_base64(crypto, target, material, counter, plain);
  };
  const auto auth = field(ctrl_crypt_counter::auth, registration_key);
  if (!auth) return unexpected(auth.error());
  const auto device = field(ctrl_crypt_counter::did, did);
  if (!device) return unexpected(device.error());
  constexpr auto os = rp_os_type_bytes();
  const auto os_type = field(ctrl_crypt_counter::os_type, os);
  if (!os_type) return unexpected(os_type.error());
  std::optional<EncryptedField> bitrate;
  std::optional<EncryptedField> streaming_header;
  if (target_facts.value().bitrate) {
    constexpr std::array<std::byte, sizeof(std::uint32_t)> zero{};
    auto encoded = field(ctrl_crypt_counter::start_bitrate, zero);
    if (!encoded) return unexpected(encoded.error());
    bitrate.emplace(std::move(encoded.value()));
  }
  if (target_facts.value().streaming_type) {
    std::array<std::byte, sizeof(std::uint32_t)> type{};
    bytes::store_le<std::uint32_t>(type, *streaming);
    auto encoded = field(ctrl_crypt_counter::streaming_type, type);
    if (!encoded) return unexpected(encoded.error());
    streaming_header.emplace(std::move(encoded.value()));
  }

  CtrlStartupRequest request;
  auto& head = request.head_;
  if (!head.request_line("GET", target_facts.value().path) ||
      !head.host_header(host, port) ||
      !head.header("User-Agent", "remoteplay Windows") ||
      !head.header("Connection", "keep-alive") ||
      !head.header("Content-Length", "0") ||
      !head.header("RP-Auth", auth->view()) ||
      !head.header("RP-Version", target_facts.value().version) ||
      !head.header("RP-Did", device->view()) ||
      !head.header("RP-ControllerType", rp_controller_type) ||
      !head.header("RP-ClientType", rp_client_type) ||
      !head.header("RP-OSType", os_type->view()) ||
      !head.header("RP-ConPath", "1") ||
      (bitrate && !head.header("RP-StartBitrate", bitrate->view())) ||
      (streaming_header &&
       !head.header("RP-StreamingType", streaming_header->view())) ||
      (target_facts.value().pad_processing &&
       !head.header("RP-PadProcNo", rp_pad_processing)) ||
      !head.end())
    return unexpected(CtrlRequestError::overflow);
  request.next_counter_ = target_facts.value().streaming_type
                              ? ctrl_crypt_counter::streaming_type + 1
                          : target_facts.value().bitrate
                              ? ctrl_crypt_counter::start_bitrate + 1
                              : ctrl_crypt_counter::os_type + 1;
  return request;
}

auto CtrlCodec::parse_response(std::span<const std::byte> received) noexcept
    -> Expected<CtrlResponse, CtrlResponseError> {
  const std::string_view input{reinterpret_cast<const char*>(received.data()),
                               received.size()};
  const auto reply = ConsoleReply::response(
      input, rp_max_header_bytes,
      ConsoleReply::Faults<CtrlResponseError>{CtrlResponseError::too_large,
                                              CtrlResponseError::invalid,
                                              CtrlResponseError::incomplete});
  if (!reply) return unexpected(reply.error());
  const auto header_size = reply->length();
  const auto code = reply->status();
  const auto headers = reply->headers();

  if (net::HttpStatus{code} != net::HttpStatus::ok)
    return CtrlResponse{unexpected(CtrlRefusal{code})};

  CtrlGrant result;
  result.header_size_ = header_size;

  for (const auto& [key, value] : headers | std::views::reverse) {
    if (key == "RP-Server-Type") {
      SecretArray<rp_server_type_size> decoded;
      const auto decoded_size =
          text::base64::decode_into(value, {decoded.data(), decoded.size()},
                                    text::base64::Alphabet::standard);
      if (!decoded_size) return unexpected(CtrlResponseError::invalid);
      if (decoded_size.value() == decoded.size())
        result.encrypted_server_type_.emplace(std::move(decoded));
      else
        result.encrypted_server_type_.reset();
    } else if (key == "RP-Prohibit") {
      result.prohibited_ =
          text::parse_integer<std::uint32_t>(text::trim(value)) == 1U;
    }
  }
  return CtrlResponse{std::move(result)};
}

auto CtrlCodec::finalize_response(const CtrlGrant& grant,
                                  platform::CryptoProvider& crypto,
                                  RegistrationTarget target,
                                  const AuthMaterial& material) noexcept
    -> CtrlResponseFinal {
  CtrlResponseFinal result;
  result.prohibited = grant.prohibited();
  if (!grant.encrypted_server_type()) return result;

  SecretArray<rp_server_type_size> plain;
  const auto decrypted =
      material.transform(crypto, target, 0, CipherDirection::decrypt,
                         grant.encrypted_server_type()->span(), plain.span());
  result.next_remote_counter = 1;
  if (decrypted)
    result.server_type = std::to_integer<std::uint8_t>(plain[0]);
  else
    result.server_type = unexpected(ServerTypeError::undecryptable);
  return result;
}

auto CtrlCodec::post_id_frames(bool dualsense, bool keyboard)
    -> std::vector<CtrlOutboundFrame> {
  std::vector<CtrlOutboundFrame> frames;
  if (dualsense) {
    frames.push_back(
        {message_type_id(CtrlMessageType::enable_dualsense_features),
         {std::byte{0}, std::byte{0x40}, std::byte{0}}});
    frames.push_back(
        {message_type_id(CtrlMessageType::dualsense_connect),
         {std::byte{0xa0}, std::byte{0xab}, std::byte{0x51}, std::byte{0xbd},
          std::byte{0xd1}, std::byte{0x7e}, std::byte{0}, std::byte{0},
          std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
          std::byte{0xff}, std::byte{0}, std::byte{0}, std::byte{0}}});
  }
  if (keyboard) {
    auto enable = encode_keyboard_enable();
    frames.push_back(std::move(enable[0]));
    frames.push_back(std::move(enable[1]));
  }
  const CtrlOutboundFrame mic_toggle{
      message_type_id(CtrlMessageType::mic_toggle),
      {std::byte{0}, std::byte{1}, std::byte{1},
       std::byte{ctrl_microphone_toggle_trailer}}};
  frames.push_back(mic_toggle);
  frames.push_back(mic_toggle);
  frames.push_back({message_type_id(CtrlMessageType::display_devices),
                    std::vector<std::byte>(4)});
  return frames;
}

}  // namespace libchiaking::net::protocol::remote_play
