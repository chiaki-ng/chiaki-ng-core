/// @file
/// @brief Declares the player machines' outbox and the shared channel
/// machine: whether the encrypted link is up and whether the invitation has
/// been used up.
///
/// Both players, InvitedPlayer on the host and VisitingPlayer on the guest,
/// queue every effect of a step in one Outbox owned by their Channel, and
/// drain it once the step is over, so no step hands effects back through an
/// out-parameter. The side-specific progress lives in HostProgress and
/// GuestProgress, which borrow the same outbox.
#pragma once

#include "src/core/kakusan/model/player_events.hpp"
#include "src/core/net/kakusan/ack_token.hpp"
#include "src/core/net/kakusan/control_message.hpp"

#include <boost/sml.hpp>

#include <utility>
#include <vector>

namespace libchiaking::kakusan::player_lifecycle {

/// @brief Boost.SML, which the machines are written in.
namespace sml = boost::sml;

/// @brief The effects of one step of a player, in the order to apply them,
/// and the tokens its control messages are sent with.
///
/// Owned by the player's Channel and borrowed by its side's progress
/// machine; the player drains it after each step. Not thread-safe.
///
/// @tparam Effect The player's effect variant, VisitingEffect or
///     InvitedEffect.
template <class Effect>
class Outbox {
 public:
  /// @brief Queues an effect.
  /// @param effect The effect.
  void push(Effect effect) { effects_.push_back(std::move(effect)); }
  /// @brief Queues a control message to the peer with a fresh token.
  /// @param message The message.
  /// @return The token its acknowledgement will carry.
  auto send(net::kakusan::control::Message message) -> net::kakusan::AckToken {
    return queue(std::move(message), false);
  }
  /// @brief Queues the last control message before leaving, whose failure to
  /// send is not reported back.
  /// @param message The message.
  /// @return The token its acknowledgement will carry.
  auto send_last(net::kakusan::control::Message message)
      -> net::kakusan::AckToken {
    return queue(std::move(message), true);
  }
  /// @brief Hands over everything queued so far and empties the outbox.
  /// @return The effects, oldest first.
  auto drain() -> std::vector<Effect> { return std::exchange(effects_, {}); }

 private:
  /// @brief Queues a control message with the next token.
  /// @param message The message.
  /// @param terminal Whether it is the last message before leaving.
  /// @return The token used.
  auto queue(net::kakusan::control::Message message, bool terminal)
      -> net::kakusan::AckToken {
    const auto token = next_token_++;
    effects_.push_back(SendControl{std::move(message), token, terminal});
    return token;
  }

  /// @brief The effects queued since the last drain.
  std::vector<Effect> effects_;
  /// @brief The token the next control message is sent with; zero means none.
  net::kakusan::AckToken next_token_{1};
};

/// @brief Link state: the encrypted link has not come up. The initial link
/// state.
struct LinkDown {};
/// @brief Link state: the encrypted link is up.
struct LinkUp {};
/// @brief Invitation state: the invitation may still be used. The initial
/// invitation state.
struct InvitationHeld {};
/// @brief Invitation state: ConsumeInvitation has been asked for.
struct InvitationSpent {};

/// @brief Event: the encrypted link came up.
struct LinkOpened {};
/// @brief Event: the guest was admitted or the attempt is over, so the
/// invitation is used up.
struct Spend {};

/// @brief Seat state shared by both sides: the visit ended and whatever was
/// set up for it has been undone; final.
struct Vacated {};
/// @brief Event shared by both sides: the visit is ending, so undo whatever
/// was set up for it.
struct Vacate {};

/// @brief The transition table of the shared channel.
///
/// Two orthogonal regions. The link region goes from LinkDown to LinkUp
/// once. The invitation region goes from InvitationHeld to InvitationSpent on
/// the first Spend, queuing ConsumeInvitation; later Spends are accepted and
/// change nothing.
///
/// @tparam Effect The player's effect variant.
template <class Effect>
struct ChannelFsm {
  /// @brief Builds the transition table for Boost.SML.
  /// @return The table of both regions.
  auto operator()() const noexcept {
    using namespace sml;
    const auto spend = [](Outbox<Effect>& outbox) {
      outbox.push(ConsumeInvitation{});
    };
    const auto unchanged = [] {};
    return make_transition_table(
        *state<LinkDown> + event<LinkOpened> = state<LinkUp>,

        *state<InvitationHeld> + event<Spend> / spend = state<InvitationSpent>,
        state<InvitationSpent> + event<Spend> / unchanged);
  }
};

/// @brief The shared channel's machine and the outbox it owns.
///
/// Owned by one PlayerChannel; not thread-safe, neither copyable nor movable
/// since the machine refers to the outbox by address.
///
/// @tparam Effect The player's effect variant.
template <class Effect>
class Channel {
 public:
  /// @brief Starts with the link down and the invitation held.
  Channel() : machine_(outbox_) {}
  /// @brief Not copyable; the machine refers to the outbox.
  Channel(const Channel&) = delete;
  /// @brief Not copy-assignable; the machine refers to the outbox.
  Channel& operator=(const Channel&) = delete;
  /// @brief Not movable; the machine refers to the outbox.
  Channel(Channel&&) = delete;
  /// @brief Not move-assignable; the machine refers to the outbox.
  Channel& operator=(Channel&&) = delete;

  /// @brief The outbox every effect of the player is queued in.
  /// @return The outbox.
  auto outbox() noexcept -> Outbox<Effect>& { return outbox_; }
  /// @brief Records that the encrypted link came up.
  /// @return False when it was already up.
  auto open() -> bool { return machine_.process_event(LinkOpened{}); }
  /// @brief Tells whether the encrypted link has come up.
  /// @return True from open() on.
  auto is_open() const noexcept -> bool {
    return machine_.is(sml::state<LinkUp>);
  }
  /// @brief Uses up the invitation, queuing ConsumeInvitation the first time.
  void spend() { static_cast<void>(machine_.process_event(Spend{})); }

 private:
  /// @brief Every effect queued by the player.
  Outbox<Effect> outbox_;
  /// @brief The SML machine.
  sml::sm<ChannelFsm<Effect>> machine_;
};

}  // namespace libchiaking::kakusan::player_lifecycle
