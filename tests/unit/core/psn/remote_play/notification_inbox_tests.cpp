#include "src/core/psn/remote_play/notification_inbox.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>

namespace psn_test {
namespace {

using libchiaking::psn::remote_play::NotificationInbox;
using libchiaking::psn::rest::Notification;

class NotificationInboxOrder : public ::testing::Test {
 protected:
  static auto named(char name) -> Notification {
    return Notification::parse(std::string{R"({"dataType":")"} + name +
                               R"("})");
  }

  static auto names(NotificationInbox& inbox) -> std::string {
    std::string order;
    while (auto front = inbox.take_front()) order += front->body()[13];
    return order;
  }

  static auto batch(std::string_view order) -> std::deque<Notification> {
    std::deque<Notification> notifications;
    for (const char name : order) notifications.push_back(named(name));
    return notifications;
  }
};

TEST_F(NotificationInboxOrder, PutsReturnedOffersFirstInOrder) {
  NotificationInbox inbox{8, 1024};
  EXPECT_TRUE(inbox.push_back(named('a')));
  EXPECT_EQ(inbox.push_front(batch("bcd")), 0U);
  EXPECT_EQ(inbox.size(), 4U);
  EXPECT_EQ(names(inbox), "bcda");
}

TEST_F(NotificationInboxOrder, ReturnsOffersOnlyUpToTheCount) {
  NotificationInbox inbox{3, 1024};
  EXPECT_TRUE(inbox.push_back(named('a')));
  EXPECT_EQ(inbox.push_front(batch("bcd")), 1U);
  EXPECT_EQ(names(inbox), "bca");
}

TEST_F(NotificationInboxOrder, ReturnsNothingToAClosedInbox) {
  NotificationInbox closed{0, 1024};
  EXPECT_EQ(closed.push_front(batch("ab")), 2U);
  EXPECT_TRUE(closed.empty());
}

TEST_F(NotificationInboxOrder, ReturnsOffersOnlyWithinTheByteBudget) {
  const auto size = named('a').size();
  NotificationInbox inbox{8, 2 * size};
  EXPECT_TRUE(inbox.push_back(named('a')));
  EXPECT_EQ(inbox.push_front(batch("bc")), 1U);
  EXPECT_FALSE(inbox.push_back(named('d')));
  EXPECT_EQ(names(inbox), "ba");
  EXPECT_TRUE(inbox.push_back(named('e')));
  EXPECT_TRUE(inbox.push_back(named('f')));
  EXPECT_FALSE(inbox.push_back(named('g')));
  EXPECT_EQ(inbox.push_front(batch("h")), 1U);
  EXPECT_EQ(names(inbox), "ef");
}

TEST_F(NotificationInboxOrder, AdmitsNothingIntoAFullBudget) {
  const auto size = named('a').size();
  NotificationInbox inbox{8, size};
  EXPECT_TRUE(inbox.push_back(named('a')));
  EXPECT_FALSE(inbox.push_back(named('b')));
  EXPECT_EQ(inbox.push_front(batch("c")), 1U);
  EXPECT_EQ(inbox.push_shedding_oldest(named('d')), std::size_t{1});
  EXPECT_EQ(names(inbox), "d");
}

TEST_F(NotificationInboxOrder, AdmitsNothingWithoutABudget) {
  NotificationInbox empty_budget{8, 0};
  EXPECT_FALSE(empty_budget.push_back(named('a')));
  EXPECT_TRUE(empty_budget.empty());
}

}  // namespace
}  // namespace psn_test
