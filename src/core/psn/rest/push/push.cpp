#include "src/core/psn/rest/push/push.hpp"

#include "src/core/utils/json/wire_dialect.hpp"

#include "src/core/psn/online_id.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/base64.hpp"
#include "src/core/utils/text/hex.hpp"

#include <libchiaking/detail/secret_array.hpp>

#include "src/core/net/transport/http/url.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <utility>

namespace libchiaking::psn::rest {

auto PushChannelRequest::valid_fqdn(std::string_view host) -> bool {
  const auto allowed = [](char value) {
    return text::ascii_visible(value) && value != '/' && value != '?' &&
           value != '#' && value != '@' && value != '\\';
  };
  return !host.empty() && host.size() <= fqdn_max_bytes &&
         std::ranges::all_of(host, allowed);
}

struct Notification::Envelope {
  using keys = json::ps::CamelCaseKeys;
  std::optional<std::string> data_type;
};

struct Notification::Addressed {
  struct To {
    using keys = json::ps::CamelCaseKeys;
    std::optional<std::string> online_id;
  };
  std::optional<To> to;
};

struct Notification::MembersData {
  struct Member {
    using keys = json::ps::CamelCaseKeys;
    std::optional<std::string> device_unique_id;
  };
  struct Data {
    std::optional<std::vector<Member>> members;
  };
  struct Body {
    std::optional<Data> data;
  };
  std::optional<Body> body;
};

struct Notification::CustomData {
  struct Data {
    using keys = json::ps::CamelCaseKeys;
    std::optional<std::string> custom_data1;
  };
  struct Body {
    std::optional<Data> data;
  };
  std::optional<Body> body;
};

struct Notification::SessionMessageData {
  struct Message {
    std::optional<SecretString> payload;
  };
  struct Data {
    using keys = json::ps::CamelCaseKeys;
    std::optional<Message> session_message;
  };
  struct Body {
    std::optional<Data> data;
  };
  std::optional<Body> body;
};

struct Notification::LoungeData {
  struct Sender {
    using keys = json::ps::CamelCaseKeys;
    std::optional<std::string> online_id;
  };
  struct Value {
    using keys = json::ps::CamelCaseKeys;
    std::optional<std::string> group_id;
    std::optional<std::string> thread_id;
    std::optional<std::string> message_uid;
  };
  struct Data {
    std::optional<Value> value;
  };
  struct Extended {
    std::optional<Data> data;
  };
  struct Body {
    using keys = json::ps::CamelCaseKeys;
    std::optional<Extended> extended_data;
  };
  std::optional<Sender> from;
  std::optional<Body> body;
};

struct Notification::InvitationData {
  struct Sender {
    using keys = json::ps::CamelCaseKeys;
    std::optional<std::string> online_id;
  };
  struct Properties {
    std::optional<Sender> from;
  };
  struct Data {
    using keys = json::ps::CamelCaseKeys;
    std::optional<std::string> session_id;
    std::optional<Properties> custom_properties;
  };
  struct Body {
    std::optional<Data> data;
  };
  std::optional<Body> body;
};

struct Notification::SessionData {
  struct Data {
    using keys = json::ps::CamelCaseKeys;
    std::optional<glz::raw_json> session_id;
  };
  struct Body {
    std::optional<Data> data;
  };
  std::optional<Body> body;
};

auto Notification::classify(
    const Expected<SecretString, json::ParseError>& canonical)
    -> NotificationType {
  if (!canonical) return NotificationType::unknown;
  const auto envelope =
      json::WireDialect::read_canonical<Envelope>(canonical->view());
  if (!envelope || !envelope->data_type) return NotificationType::unknown;
  constexpr std::array<std::pair<std::string_view, NotificationType>, 8> known{{
      {session_created_type, NotificationType::remote_play_session_created},
      {members_created_type, NotificationType::members_created},
      {custom_data1_updated_type, NotificationType::custom_data1_updated},
      {session_message_created_type, NotificationType::session_message_created},
      {members_deleted_type, NotificationType::members_deleted},
      {session_deleted_type, NotificationType::remote_play_session_deleted},
      {invitation_created_type, NotificationType::invitation_created},
      {lounge_group_type, NotificationType::lounge_group_changed},
  }};
  const auto found =
      std::ranges::find(known, *envelope->data_type,
                        &std::pair<std::string_view, NotificationType>::first);
  return found == known.end() ? NotificationType::unknown : found->second;
}

template <class Message>
auto Notification::typed(NotificationType expected) const
    -> Expected<Message, MessageFault> {
  if (!canonical_) return unexpected(MessageFault::parsed(canonical_.error()));
  if (type_ != expected)
    return unexpected(MessageFault{bad_reply::InvalidSchema{}});
  auto message = json::WireDialect::read_canonical<Message>(canonical_->view());
  if (!message) return unexpected(MessageFault{bad_reply::InvalidSchema{}});
  return std::move(message.value());
}

Notification::Notification(
    SecretString body,
    Expected<SecretString, json::ParseError> canonical) noexcept
    : body_(std::move(body)),
      canonical_(std::move(canonical)),
      type_(classify(canonical_)) {}

auto Notification::parse(std::span<const std::byte> payload) -> Notification {
  SecretString body{payload};
  auto canonical = json::WireDialect::canonical_text(body.view());
  return Notification{std::move(body), std::move(canonical)};
}

auto Notification::data_type() const -> std::string {
  if (!canonical_) return {};
  const auto envelope =
      json::WireDialect::read_canonical<Envelope>(canonical_->view());
  if (!envelope || !envelope->data_type) return {};
  return *envelope->data_type;
}

auto Notification::parse(std::string_view payload) -> Notification {
  return parse(std::as_bytes(std::span{payload.data(), payload.size()}));
}

auto Notification::session_created_online_id() const
    -> Expected<std::string, OnlineIdFault> {
  auto message =
      typed<Addressed>(NotificationType::remote_play_session_created);
  if (!message) return unexpected(OnlineIdFault{message.error()});
  if (!message->to || !message->to->online_id)
    return unexpected(OnlineIdFault{bad_reply::InvalidSchema{}});
  if (!psn::OnlineId::parse(*message->to->online_id))
    return unexpected(OnlineIdFault{bad_reply::InvalidOnlineId{}});
  return std::move(*message->to->online_id);
}

auto Notification::member_device_uid() const
    -> Expected<std::array<std::byte, psn_device_uid_size>, DeviceUidFault> {
  auto message = typed<MembersData>(NotificationType::members_created);
  if (!message) return unexpected(DeviceUidFault{message.error()});
  if (!message->body || !message->body->data || !message->body->data->members ||
      message->body->data->members->empty() ||
      !message->body->data->members->front().device_unique_id)
    return unexpected(DeviceUidFault{bad_reply::InvalidSchema{}});
  std::array<std::byte, psn_device_uid_size> bytes{};
  if (!text::decode_hex(*message->body->data->members->front().device_unique_id,
                        bytes))
    return unexpected(DeviceUidFault{bad_reply::InvalidDeviceUid{}});
  return bytes;
}

auto Notification::custom_data1() const
    -> Expected<std::array<std::byte, psn_custom_data_size>, CustomData1Fault> {
  auto message = typed<CustomData>(NotificationType::custom_data1_updated);
  if (!message) return unexpected(CustomData1Fault{message.error()});
  if (!message->body || !message->body->data ||
      !message->body->data->custom_data1)
    return unexpected(CustomData1Fault{bad_reply::InvalidSchema{}});
  const std::string_view text = *message->body->data->custom_data1;
  if (text.size() != custom_data1_text_length)
    return unexpected(CustomData1Fault{bad_reply::InvalidCustomData1{}});
  SecretArray<24> outer;
  const auto outer_length = text::base64::decode_into(
      text, {outer.data(), outer.size()}, text::base64::Alphabet::standard);
  if (!outer_length)
    return unexpected(CustomData1Fault{bad_reply::InvalidCustomData1{}});
  SecretArray<24> inner;
  const std::string_view encoded{reinterpret_cast<const char*>(outer.data()),
                                 outer_length.value()};
  const auto inner_length = text::base64::decode_into(
      encoded, {inner.data(), inner.size()}, text::base64::Alphabet::standard);
  if (!inner_length || inner_length.value() < psn_custom_data_size ||
      inner_length.value() > custom_data1_maximum_bytes)
    return unexpected(CustomData1Fault{bad_reply::InvalidCustomData1{}});
  std::array<std::byte, psn_custom_data_size> bytes{};
  std::copy_n(inner.begin(), bytes.size(), bytes.begin());
  return bytes;
}

auto Notification::lounge_message() const
    -> Expected<LoungeMessage, MessageFault> {
  const auto message =
      typed<LoungeData>(NotificationType::lounge_group_changed);
  if (!message) return unexpected(MessageFault{message.error()});
  if (!message->from || !message->from->online_id ||
      message->from->online_id->empty() || !message->body ||
      !message->body->extended_data || !message->body->extended_data->data ||
      !message->body->extended_data->data->value)
    return unexpected(MessageFault{bad_reply::InvalidSchema{}});
  const auto& value = *message->body->extended_data->data->value;
  if (!value.message_uid || value.message_uid->empty() || !value.group_id)
    return unexpected(MessageFault{bad_reply::InvalidSchema{}});
  return LoungeMessage{*message->from->online_id, *value.group_id,
                       value.thread_id.value_or(""), *value.message_uid};
}

auto Notification::session_invitation() const
    -> Expected<SessionInvitation, MessageFault> {
  const auto message =
      typed<InvitationData>(NotificationType::invitation_created);
  if (!message) return unexpected(MessageFault{message.error()});
  if (!message->body || !message->body->data)
    return unexpected(MessageFault{bad_reply::InvalidSchema{}});
  const auto& data = *message->body->data;
  if (!data.session_id || data.session_id->empty() || !data.custom_properties ||
      !data.custom_properties->from ||
      !data.custom_properties->from->online_id ||
      data.custom_properties->from->online_id->empty())
    return unexpected(MessageFault{bad_reply::InvalidSchema{}});
  return SessionInvitation{*data.custom_properties->from->online_id,
                           *data.session_id};
}

auto Notification::session_message_payload() const
    -> Expected<SecretString, MessageFault> {
  auto message =
      typed<SessionMessageData>(NotificationType::session_message_created);
  if (!message) return unexpected(MessageFault{message.error()});
  if (!message->body || !message->body->data ||
      !message->body->data->session_message ||
      !message->body->data->session_message->payload)
    return unexpected(MessageFault{bad_reply::InvalidSchema{}});
  return std::move(*message->body->data->session_message->payload);
}

auto Notification::belongs_to_session(std::string_view session_id,
                                      bool require_id) const -> bool {
  if (!canonical_) return false;
  const auto message =
      json::WireDialect::read_canonical<SessionData>(canonical_->view());
  if (!message) return false;
  if (!message->body || !message->body->data ||
      !message->body->data->session_id)
    return !require_id;
  const auto id =
      json::ps::read_reply<std::string>(message->body->data->session_id->str);
  return id && *id == session_id;
}

PushChannelRequest::PushChannelRequest(
    platform::WebSocketRequest request) noexcept
    : request_(std::move(request)) {}

auto PushChannelRequest::create(const auth::BearerToken& token,
                                std::string_view fqdn, PushApp app)
    -> Expected<PushChannelRequest, Fault<bad_request::InvalidFqdn>> {
  if (!valid_fqdn(fqdn))
    return unexpected(
        Fault<bad_request::InvalidFqdn>{bad_request::InvalidFqdn{}});
  platform::WebSocketRequest request;
  request.url = SecretString{std::format("wss://{}{}", fqdn, channel_path)};
  constexpr auto placement = auth::BearerToken::kind::placement;
  SecretString authorization{placement.prefix};
  authorization.append(token.view());
  request.headers.reserve(8);
  request.headers.push_back(
      {std::string{placement.header}, std::move(authorization)});
  const auto header = [&request](std::string_view name,
                                 std::string_view value) {
    request.headers.push_back({std::string{name}, SecretString{value}});
  };
  if (app == PushApp::mobile) {
    header("X-PSN-APP-TYPE", mobile_app_type);
    header("X-PSN-APP-VER", mobile_app_version);
    header("X-PSN-KEEP-ALIVE-STATUS-TYPE", "3");
    header("X-PSN-OS-VER", mobile_os_version);
  } else {
    header("User-Agent", "WebSocket++/0.8.2");
    header("X-PSN-APP-TYPE", "REMOTE_PLAY");
    header("X-PSN-APP-VER", "RemotePlay/1.0");
    header("X-PSN-KEEP-ALIVE-STATUS-TYPE", "3");
    header("X-PSN-OS-VER", "Windows/10.0");
  }
  header("X-PSN-PROTOCOL-VERSION", "2.1");
  header("X-PSN-RECONNECTION", "false");
  request.subprotocol = "np-pushpacket";
  request.connect_timeout = push_connect_timeout;
  request.ping_interval = push_ping_interval;
  request.pong_timeout = push_pong_timeout;
  request.close_mode = platform::WebSocketCloseMode::transport_only;
  request.ping_immediately = true;
  request.maximum_message_bytes = push_max_message_bytes;
  request.tls_configuration.trust_mode = platform::TlsTrustMode::system;
  return PushChannelRequest{std::move(request)};
}

struct Push::ServerAddress {
  std::optional<std::string> fqdn;
};

Push::Push(net::http::Client& client, auth::BearerToken token) noexcept
    : client_(client), token_(std::move(token)) {}

auto Push::server_address() -> Expected<std::string, ServerAddressError> {
  net::http::Query url{server_address_url};
  url.add_verbatim("version", "2.1")
      .add_verbatim("fields", "keepAliveStatus")
      .add_verbatim("keepAliveStatusType", "3");
  net::http::Request request{server_address_endpoint, std::move(url)};
  request.authorize(token_);
  auto response = ServerAddressError::checked(client_.send(std::move(request)),
                                              token_.view());
  if (!response) return unexpected(std::move(response.error()));
  auto reply = json::WireDialect::read<ServerAddress>(response.value().body());
  if (!reply) return unexpected(ServerAddressError::parsed(reply.error()));
  if (!reply->fqdn)
    return unexpected(ServerAddressError{bad_reply::InvalidSchema{}});
  if (!PushChannelRequest::valid_fqdn(*reply->fqdn))
    return unexpected(ServerAddressError{bad_reply::InvalidFqdn{}});
  return std::move(*reply->fqdn);
}

}  // namespace libchiaking::psn::rest
