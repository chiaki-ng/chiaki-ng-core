#include "src/core/psn/rest/push/push.hpp"
#include "src/core/utils/json/json.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace psn_test {
namespace {

using libchiaking::psn::rest::Cause;
using libchiaking::psn::rest::Notification;
using libchiaking::psn::rest::NotificationType;
namespace bad_reply = libchiaking::psn::rest::bad_reply;
namespace bad_request = libchiaking::psn::rest::bad_request;

class PushNotification : public ::testing::Test {
 protected:
  static auto type(std::string_view text) -> NotificationType {
    return Notification::parse(text).type();
  }
};

TEST_F(PushNotification, IsMoveOnly) {
  static_assert(!std::is_copy_constructible_v<Notification>);
  static_assert(!std::is_copy_assignable_v<Notification>);
  static_assert(std::is_move_constructible_v<Notification>);
  static_assert(std::is_move_assignable_v<Notification>);
  SUCCEED();
}

TEST_F(PushNotification, TypesTheSessionManagerEvents) {
  EXPECT_EQ(
      type(
          R"({"dataType":"psn:sessionManager:sys:remotePlaySession:created"})"),
      NotificationType::remote_play_session_created);
  EXPECT_EQ(
      type(R"({"dataType":"psn:sessionManager:sys:rps:members:created"})"),
      NotificationType::members_created);
  EXPECT_EQ(
      type(R"({"dataType":"psn:sessionManager:sys:rps:customData1:updated"})"),
      NotificationType::custom_data1_updated);
  EXPECT_EQ(
      type(
          R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created"})"),
      NotificationType::session_message_created);
  EXPECT_EQ(
      type(R"({"dataType":"psn:sessionManager:sys:rps:members:deleted"})"),
      NotificationType::members_deleted);
  EXPECT_EQ(
      type(
          R"({"dataType":"psn:sessionManager:sys:remotePlaySession:deleted"})"),
      NotificationType::remote_play_session_deleted);
  EXPECT_EQ(
      type(
          R"({"dataType":"psn:sessionManager:sys:v2:ps:invitations:created"})"),
      NotificationType::invitation_created);
}

TEST_F(PushNotification, TypesALoungeGroupChange) {
  EXPECT_EQ(
      Notification::parse(R"({"dataType":"np:service:gamingLoungeGroups"})")
          .data_type(),
      "np:service:gamingLoungeGroups");
  EXPECT_EQ(type(R"({"dataType":"np:service:gamingLoungeGroups"})"),
            NotificationType::lounge_group_changed);
}

TEST_F(PushNotification, LeavesOtherDataTypesUnknown) {
  EXPECT_EQ(type(R"({"dataType":"np:service:notificationlist:newitem"})"),
            NotificationType::unknown);
  EXPECT_EQ(type(R"({"dataType":"psn:sessionManager:sys:rps:unheard:of"})"),
            NotificationType::unknown);
  EXPECT_EQ(type(R"({"dataType":123})"), NotificationType::unknown);
  EXPECT_EQ(type(R"({"other":"x"})"), NotificationType::unknown);
}

TEST_F(PushNotification, LeavesUnreadableTextUnknown) {
  EXPECT_TRUE(Notification::parse("{").data_type().empty());
  EXPECT_EQ(type("{"), NotificationType::unknown);
  EXPECT_EQ(type(R"({"dataType":)"), NotificationType::unknown);
  EXPECT_EQ(type(std::string(libchiaking::json::max_document_bytes + 1, ' ')),
            NotificationType::unknown);
  EXPECT_EQ(type(std::string(16 * 1024 + 1, ' ')), NotificationType::unknown);
}

TEST_F(PushNotification, TypesByTheLastDuplicateDataType) {
  EXPECT_EQ(
      type(
          R"({"dataType":"a","dataType":"psn:sessionManager:sys:rps:members:created"})"),
      NotificationType::members_created);
}

TEST_F(PushNotification, TypesWireDialectText) {
  EXPECT_EQ(type("{\"dataType\":\"psn:sessionManager:sys:rps:members:"
                 "created\",\"note\":\"\\uD800\xFF\t\",\"n\":007}\n<html>"),
            NotificationType::members_created);
}

TEST_F(PushNotification, ReadsALoungeMessage) {
  const auto message =
      Notification::parse(
          R"({"dataType":"np:service:gamingLoungeGroups","from":{"accountId":222,"onlineId":"Friend"},)"
          R"("body":{"extendedData":{"data":{"value":{"pushEventId":"101","groupId":"g-1",)"
          R"("threadId":"t-1","messageUid":"m-1"}}}}})")
          .lounge_message();
  ASSERT_TRUE(message);
  EXPECT_EQ(message->sender, "Friend");
  EXPECT_EQ(message->group_id, "g-1");
  EXPECT_EQ(message->thread_id, "t-1");
  EXPECT_EQ(message->message_uid, "m-1");
}

TEST_F(PushNotification, RejectsALoungeReadReceiptAsAMessage) {
  const auto read =
      Notification::parse(
          R"({"dataType":"np:service:gamingLoungeGroups","body":{"extendedData":)"
          R"({"data":{"value":{"pushEventId":"1001","groupId":"g-1","accountId":"222"}}}}})")
          .lounge_message();
  ASSERT_FALSE(read);
  EXPECT_TRUE(read.error().is<bad_reply::InvalidSchema>());
}

TEST_F(PushNotification, HasNoLoungeMessageForAnInvitation) {
  EXPECT_FALSE(
      Notification::parse(
          R"({"dataType":"psn:sessionManager:sys:v2:ps:invitations:created"})")
          .lounge_message());
}

TEST_F(PushNotification, ReadsASessionInvitation) {
  const auto invited =
      Notification::parse(
          R"({"dataType":"psn:sessionManager:sys:v2:ps:invitations:created",)"
          R"("body":{"data":{"customProperties":{"from":{"accountId":"222",)"
          R"("onlineId":"Friend","platform":"PS5"}},"sessionId":"s-1"}}})")
          .session_invitation();
  ASSERT_TRUE(invited);
  EXPECT_EQ(invited->sender, "Friend");
  EXPECT_EQ(invited->session_id, "s-1");
}

TEST_F(PushNotification, HasNoInvitationWithoutABodyOrOfAnotherType) {
  EXPECT_FALSE(
      Notification::parse(
          R"({"dataType":"psn:sessionManager:sys:v2:ps:invitations:created"})")
          .session_invitation());
  EXPECT_FALSE(
      Notification::parse(
          R"({"dataType":"np:service:gamingLoungeGroups","body":{"data":{)"
          R"("customProperties":{"from":{"onlineId":"Friend"}},"sessionId":"s-1"}}})")
          .session_invitation());
}

TEST_F(PushNotification, KeepsItsBody) {
  const auto created = Notification::parse(
      R"({"dataType":"psn:sessionManager:sys:remotePlaySession:created"})");
  EXPECT_EQ(
      created.body(),
      R"({"dataType":"psn:sessionManager:sys:remotePlaySession:created"})");
  EXPECT_EQ(Notification::parse(R"({"dataType":)").body(), R"({"dataType":)");
}

TEST_F(PushNotification, TypesALargeByteBody) {
  std::string big =
      R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"pad":")";
  big.append(48 * 1024, 'x');
  big += R"("}}  )";
  const auto large =
      Notification::parse(std::as_bytes(std::span{big.data(), big.size()}));
  EXPECT_EQ(large.type(), NotificationType::session_message_created);
  EXPECT_EQ(large.size(), big.size());
}

TEST_F(PushNotification, OwnsACopyOfItsText) {
  std::string original =
      R"({"dataType":"psn:sessionManager:sys:rps:members:created"})";
  const auto owned = Notification::parse(original);
  original.assign(original.size(), 'x');
  EXPECT_EQ(owned.body(),
            R"({"dataType":"psn:sessionManager:sys:rps:members:created"})");
}

}  // namespace
}  // namespace psn_test
