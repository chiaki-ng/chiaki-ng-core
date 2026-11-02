#pragma once

#include "src/core/psn/rest/push/push.hpp"
#include "src/core/utils/text/hex.hpp"
#include "tests/support/json/tree.hpp"
#include "tests/support/setup.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace test_support::psn {

class Notifications {
 public:
  using Type = libchiaking::psn::rest::NotificationType;
  using Notification = libchiaking::psn::rest::Notification;
  using ConsoleFamily = libchiaking::psn::rest::ConsoleFamily;

  static auto typed(Type type, std::string_view body) -> Notification {
    if (type == Type::unknown ||
        body.find("\"dataType\"") != std::string_view::npos) {
      auto notification = Notification::parse(body);
      Setup::require(notification.type() == type,
                     "the notification has the asked type");
      return notification;
    }
    auto parsed = Setup::value(JsonTree::parse(body), "the body parses");
    Setup::require(parsed.is_object(), "the body is an object");
    parsed["dataType"] = std::string{data_type_of(type)};
    const auto text = JsonTree::dump(parsed);
    auto notification = Notification::parse(std::string_view{text});
    Setup::require(notification.type() == type,
                   "the notification has the asked type");
    return notification;
  }

  static auto envelope(std::string_view payload, std::int64_t account_id,
                       std::span<const std::byte, 32> device_uid,
                       ConsoleFamily family) -> std::string {
    std::string device;
    libchiaking::text::append_hex(device, device_uid);
    auto recipient = JsonTree::object();
    recipient["accountId"] = std::to_string(account_id);
    recipient["deviceUniqueId"] = device;
    recipient["platform"] = family == ConsoleFamily::ps4 ? "PS4" : "PS5";
    auto recipients = JsonTree::array();
    JsonTree::elements(recipients).push_back(std::move(recipient));
    auto root = JsonTree::object();
    root["channel"] = "remote_play:1";
    root["payload"] = "ver=1.0, type=text, body=" + std::string{payload};
    root["to"] = std::move(recipients);
    return JsonTree::dump(root);
  }

 private:
  static auto data_type_of(Type type) -> std::string_view {
    switch (type) {
      case Type::remote_play_session_created:
        return "psn:sessionManager:sys:remotePlaySession:created";
      case Type::members_created:
        return "psn:sessionManager:sys:rps:members:created";
      case Type::custom_data1_updated:
        return "psn:sessionManager:sys:rps:customData1:updated";
      case Type::session_message_created:
        return "psn:sessionManager:sys:rps:sessionMessage:created";
      case Type::members_deleted:
        return "psn:sessionManager:sys:rps:members:deleted";
      case Type::remote_play_session_deleted:
        return "psn:sessionManager:sys:remotePlaySession:deleted";
      case Type::invitation_created:
        return "psn:sessionManager:sys:v2:ps:invitations:created";
      case Type::lounge_group_changed:
        return "np:service:gamingLoungeGroups";
      case Type::unknown:
        break;
    }
    return {};
  }
};

}  // namespace test_support::psn
