#include "src/core/kakusan/model/guest_progress.hpp"
#include "src/core/kakusan/model/host_progress.hpp"
#include "src/core/kakusan/model/player_lifecycle.hpp"

#include <gtest/gtest.h>

#include <variant>

namespace {

using namespace libchiaking::kakusan;
namespace lifecycle = libchiaking::kakusan::player_lifecycle;
namespace control = libchiaking::net::kakusan::control;
using libchiaking::net::kakusan::SeatBinding;

constexpr SeatBinding seat{3, 7};

TEST(PlayerChannelLifecycle, OpensOnceAndSpendsTheInvitationOnce) {
  lifecycle::Channel<InvitedEffect> channel;
  EXPECT_FALSE(channel.is_open());
  EXPECT_TRUE(channel.open());
  EXPECT_TRUE(channel.is_open());
  EXPECT_FALSE(channel.open());
  channel.spend();
  channel.spend();
  const auto effects = channel.outbox().drain();
  ASSERT_EQ(effects.size(), 1U);
  EXPECT_TRUE(std::holds_alternative<ConsumeInvitation>(effects[0]));
  EXPECT_TRUE(channel.outbox().drain().empty());
}

TEST(PlayerChannelLifecycle, NumbersMessagesAndMarksTheLastOne) {
  lifecycle::Channel<VisitingEffect> channel;
  EXPECT_EQ(channel.outbox().send(control::AdmissionPending{}), 1U);
  EXPECT_EQ(channel.outbox().send_last(control::End{control::Reason::user}),
            2U);
  const auto effects = channel.outbox().drain();
  ASSERT_EQ(effects.size(), 2U);
  EXPECT_FALSE(std::get<SendControl>(effects[0]).terminal);
  EXPECT_TRUE(std::get<SendControl>(effects[1]).terminal);
  EXPECT_EQ(std::get<SendControl>(effects[1]).token, 2U);
}

TEST(HostProgressLifecycle, SeatsTheGuestAndUndoesItInReverse) {
  lifecycle::Channel<InvitedEffect> channel;
  lifecycle::HostProgress progress{channel.outbox()};
  EXPECT_FALSE(progress.process(lifecycle::Reserve{seat}));
  EXPECT_TRUE(progress.process(lifecycle::RequestSeat{}));
  EXPECT_TRUE(progress.process(lifecycle::Reserve{seat}));
  EXPECT_FALSE(progress.process(lifecycle::Reserve{seat}));
  EXPECT_EQ(progress.binding(), seat);
  EXPECT_TRUE(progress.process(lifecycle::OfferSetup{}));
  EXPECT_TRUE(progress.process(lifecycle::Grant{}));
  EXPECT_TRUE(progress.granted());
  EXPECT_TRUE(progress.process(lifecycle::TakeReady{}));
  EXPECT_TRUE(progress.process(lifecycle::Stream{}));
  EXPECT_FALSE(progress.process(lifecycle::Stream{}));
  auto effects = channel.outbox().drain();
  ASSERT_EQ(effects.size(), 5U);
  EXPECT_TRUE(std::holds_alternative<ReserveSeat>(effects[0]));
  EXPECT_TRUE(std::holds_alternative<control::AdmissionPending>(
      std::get<SendControl>(effects[1]).message));
  EXPECT_EQ(std::get<RequestSourceJoin>(effects[2]).binding, seat);
  EXPECT_EQ(std::get<GrantSeat>(effects[3]).binding, seat);
  EXPECT_EQ(std::get<StartMedia>(effects[4]).binding, seat);

  EXPECT_TRUE(progress.process(lifecycle::Vacate{}));
  effects = channel.outbox().drain();
  ASSERT_EQ(effects.size(), 4U);
  EXPECT_TRUE(std::holds_alternative<StopMedia>(effects[0]));
  EXPECT_TRUE(std::holds_alternative<InjectNeutral>(effects[1]));
  EXPECT_TRUE(std::holds_alternative<RequestSourceRemoval>(effects[2]));
  EXPECT_TRUE(std::holds_alternative<ReleaseSeat>(effects[3]));
  EXPECT_TRUE(progress.in<lifecycle::Vacated>());
  EXPECT_FALSE(progress.process(lifecycle::Vacate{}));
  EXPECT_EQ(progress.binding(), seat);
}

TEST(HostProgressLifecycle, NeutralizesOnceUntilTouched) {
  lifecycle::Channel<InvitedEffect> channel;
  lifecycle::HostProgress progress{channel.outbox()};
  EXPECT_TRUE(progress.process(lifecycle::RequestSeat{}));
  EXPECT_TRUE(progress.process(lifecycle::Reserve{seat}));
  (void)channel.outbox().drain();
  EXPECT_TRUE(progress.process(lifecycle::Touch{}));
  EXPECT_TRUE(progress.process(lifecycle::Neutralize{}));
  EXPECT_FALSE(progress.process(lifecycle::Neutralize{}));
  EXPECT_TRUE(progress.process(lifecycle::Touch{}));
  const auto effects = channel.outbox().drain();
  ASSERT_EQ(effects.size(), 1U);
  EXPECT_EQ(std::get<InjectNeutral>(effects[0]).binding, seat);
}

TEST(HostProgressLifecycle, VerifiesInOrder) {
  lifecycle::Channel<InvitedEffect> channel;
  lifecycle::HostProgress progress{channel.outbox()};
  EXPECT_FALSE(progress.process(lifecycle::SendConfirmation{}));
  EXPECT_TRUE(progress.process(lifecycle::OfferShare{}));
  EXPECT_FALSE(progress.process(lifecycle::OfferShare{}));
  EXPECT_TRUE(progress.process(lifecycle::SendConfirmation{}));
  EXPECT_TRUE(progress.process(lifecycle::ConfirmPeer{}));
  EXPECT_TRUE(progress.in<lifecycle::PeerConfirmed>());
  EXPECT_TRUE(progress.process(lifecycle::Nominate{}));
  EXPECT_FALSE(progress.process(lifecycle::Nominate{}));
  EXPECT_TRUE(progress.process(lifecycle::Approve{}));
  EXPECT_FALSE(progress.process(lifecycle::Approve{}));
}

TEST(GuestProgressLifecycle, FollowsTheHostAndStopsWhatItStarted) {
  lifecycle::Channel<VisitingEffect> channel;
  lifecycle::GuestProgress progress{channel.outbox()};
  EXPECT_FALSE(progress.process(lifecycle::Configure{}));
  EXPECT_TRUE(progress.process(lifecycle::AwaitAdmission{}));
  EXPECT_FALSE(progress.process(lifecycle::AwaitAdmission{}));
  EXPECT_TRUE(progress.process(lifecycle::Configure{}));
  EXPECT_TRUE(progress.process(lifecycle::ReportSourceReady{}));
  EXPECT_FALSE(progress.process(lifecycle::ReportSourceReady{}));
  EXPECT_TRUE(progress.process(lifecycle::TakeGrant{control::SeatGranted{
      seat, 4, 2, libchiaking::net::kakusan::button_mask}}));
  EXPECT_TRUE(progress.process(lifecycle::ReportReady{seat}));
  EXPECT_TRUE(progress.process(lifecycle::Activate{seat}));
  auto effects = channel.outbox().drain();
  ASSERT_EQ(effects.size(), 4U);
  EXPECT_TRUE(std::holds_alternative<control::SourceReady>(
      std::get<SendControl>(effects[0]).message));
  EXPECT_EQ(std::get<PrepareController>(effects[1]).grant.binding, seat);
  EXPECT_TRUE(std::holds_alternative<control::Ready>(
      std::get<SendControl>(effects[2]).message));
  EXPECT_EQ(std::get<StartMediaReception>(effects[3]).binding, seat);
  EXPECT_TRUE(progress.process(lifecycle::Vacate{}));
  effects = channel.outbox().drain();
  ASSERT_EQ(effects.size(), 2U);
  EXPECT_TRUE(std::holds_alternative<StopMediaReception>(effects[0]));
  EXPECT_TRUE(std::holds_alternative<StopRumble>(effects[1]));
}

TEST(GuestProgressLifecycle, LeavesNothingToStopBeforeTheSetup) {
  lifecycle::Channel<VisitingEffect> channel;
  lifecycle::GuestProgress progress{channel.outbox()};
  EXPECT_TRUE(progress.process(lifecycle::AwaitAdmission{}));
  EXPECT_TRUE(progress.process(lifecycle::Vacate{}));
  EXPECT_TRUE(channel.outbox().drain().empty());
  EXPECT_TRUE(progress.in<lifecycle::Vacated>());
}

TEST(GuestProgressLifecycle, AnswersThenConfirms) {
  lifecycle::Channel<VisitingEffect> channel;
  lifecycle::GuestProgress progress{channel.outbox()};
  EXPECT_FALSE(progress.process(lifecycle::ReturnConfirmation{}));
  EXPECT_TRUE(progress.process(lifecycle::AnswerShare{}));
  EXPECT_TRUE(progress.process(lifecycle::ReturnConfirmation{}));
  EXPECT_TRUE(progress.in<lifecycle::ConfirmationReturned>());
  const auto effects = channel.outbox().drain();
  ASSERT_EQ(effects.size(), 1U);
  EXPECT_TRUE(std::holds_alternative<control::PakeB>(
      std::get<SendControl>(effects[0]).message));
}

}  // namespace
