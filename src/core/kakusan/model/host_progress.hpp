/// @file
/// @brief Declares HostProgress, how far the host has taken one invited
/// guest beyond the shared channel: finding a path, the code exchange, the
/// host's approval, the seat and the guest's input.
#pragma once

#include "src/core/kakusan/model/player_events.hpp"
#include "src/core/kakusan/model/player_lifecycle.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/controller_frame.hpp"

#include <boost/sml.hpp>

#include <optional>

namespace libchiaking::kakusan::player_lifecycle {

/// @brief Nomination state: no network path to the guest yet. The initial
/// nomination state.
struct Unnominated {};
/// @brief Nomination state: a network path to the guest was found.
struct Nominated {};

/// @brief Host verification state: waiting for both codes. The initial
/// verification state.
struct AwaitingCodes {};
/// @brief Host verification state: PakeA was sent and the guest's share is
/// awaited.
struct ShareOffered {};
/// @brief Host verification state: the guest's share arrived and ConfirmA
/// was sent.
struct ConfirmationSent {};
/// @brief Host verification state: the guest's ConfirmB matched.
struct PeerConfirmed {};

/// @brief Approval state: the host has not approved the guest. The initial
/// approval state.
struct Unapproved {};
/// @brief Approval state: the host approved the guest for a seat.
struct Approved {};

/// @brief Host seat state: no seat asked for. The initial seat state.
struct Unseated {};
/// @brief Host seat state: a Kakusan seat reservation was asked for.
struct Requested {};
/// @brief Host seat state: the Kakusan seat is held and the console was asked
/// to seat the guest.
struct Held {};
/// @brief Host seat state: the console seated the guest and the source setup
/// was sent.
struct SetupOffered {};
/// @brief Host seat state: the guest confirmed the setup and the seat was
/// granted.
struct Granted {};
/// @brief Host seat state: the guest's controller is ready and Active was
/// sent.
struct Readied {};
/// @brief Host seat state: the stream is being forwarded to the guest.
struct Streaming {};

/// @brief Input state: input arrived recently, or play just began. The
/// initial input state.
struct InputFresh {};
/// @brief Input state: a neutral state was sent since the last input.
struct InputNeutral {};

/// @brief Event: a network path to the guest was found.
struct Nominate {};
/// @brief Event: the code exchange began; queues PakeA.
struct OfferShare {
  /// @brief The host's share.
  net::kakusan::control::Share share{};
};
/// @brief Event: the guest's share was taken and ConfirmA is being sent.
struct SendConfirmation {};
/// @brief Event: the guest's ConfirmB matched.
struct ConfirmPeer {};
/// @brief Event: the host approved the guest.
struct Approve {};
/// @brief Event: the guest is verified and approved; queues ReserveSeat.
struct RequestSeat {};
/// @brief Event: the wanted Kakusan seat was reserved; queues
/// AdmissionPending and RequestSourceJoin.
struct Reserve {
  /// @brief The seat.
  net::kakusan::SeatBinding binding;
};
/// @brief Event: the console seated the guest and the setup is being sent.
struct OfferSetup {};
/// @brief Event: the guest confirmed the setup; queues GrantSeat.
struct Grant {};
/// @brief Event: the guest's controller is ready.
struct TakeReady {};
/// @brief Event: Active was acknowledged; queues StartMedia.
struct Stream {};
/// @brief Event: the guest's input arrived, or play began.
struct Touch {};
/// @brief Event: the guest has been idle too long; queues InjectNeutral.
struct Neutralize {};

/// @brief What the host machine reads and writes beside its states.
struct HostContext {
  /// @brief The player's outbox; never null.
  Outbox<InvitedEffect>* outbox;
  /// @brief The guest's Kakusan seat, once reserved; kept after ending.
  std::optional<net::kakusan::SeatBinding> binding{};
};

/// @brief The transition table of the host's progress with one guest.
///
/// Five orthogonal regions:
///
/// - nomination: Unnominated to Nominated, once;
/// - verification: AwaitingCodes, ShareOffered (PakeA queued),
///   ConfirmationSent, PeerConfirmed;
/// - approval: Unapproved to Approved, once;
/// - seat: Unseated, Requested (ReserveSeat queued), Held (AdmissionPending
///   and RequestSourceJoin queued), SetupOffered, Granted (GrantSeat
///   queued), Readied, Streaming (StartMedia queued); Vacate from any of them
///   reaches Vacated, queuing in this order whichever of StopMedia,
///   InjectNeutral, RequestSourceRemoval and ReleaseSeat undo what was set
///   up;
/// - input: InputFresh and InputNeutral; Neutralize queues InjectNeutral and
///   Touch returns, accepted while already fresh.
struct HostFsm {
  /// @brief Builds the transition table for Boost.SML.
  /// @return The table of all five regions.
  auto operator()() const noexcept {
    using namespace sml;
    const auto offer = [](const OfferShare& event, HostContext& context) {
      (void)context.outbox->send(net::kakusan::control::PakeA{event.share});
    };
    const auto request = [](HostContext& context) {
      context.outbox->push(ReserveSeat{});
    };
    const auto reserve = [](const Reserve& event, HostContext& context) {
      context.binding = event.binding;
      (void)context.outbox->send(net::kakusan::control::AdmissionPending{});
      context.outbox->push(RequestSourceJoin{event.binding});
    };
    const auto grant = [](HostContext& context) {
      context.outbox->push(GrantSeat{*context.binding});
    };
    const auto stream = [](HostContext& context) {
      context.outbox->push(StartMedia{*context.binding});
    };
    const auto release = [](HostContext& context) {
      context.outbox->push(RequestSourceRemoval{*context.binding});
      context.outbox->push(ReleaseSeat{*context.binding});
    };
    const auto unjoin = [](HostContext& context) {
      context.outbox->push(InjectNeutral{*context.binding});
      context.outbox->push(RequestSourceRemoval{*context.binding});
      context.outbox->push(ReleaseSeat{*context.binding});
    };
    const auto unstream = [](HostContext& context) {
      context.outbox->push(StopMedia{*context.binding});
      context.outbox->push(InjectNeutral{*context.binding});
      context.outbox->push(RequestSourceRemoval{*context.binding});
      context.outbox->push(ReleaseSeat{*context.binding});
    };
    const auto neutralize = [](HostContext& context) {
      context.outbox->push(InjectNeutral{*context.binding});
    };
    const auto unchanged = [] {};
    return make_transition_table(
        *state<Unnominated> + event<Nominate> = state<Nominated>,

        *state<AwaitingCodes> + event<OfferShare> / offer = state<ShareOffered>,
        state<ShareOffered> + event<SendConfirmation> = state<ConfirmationSent>,
        state<ConfirmationSent> + event<ConfirmPeer> = state<PeerConfirmed>,

        *state<Unapproved> + event<Approve> = state<Approved>,

        *state<Unseated> + event<RequestSeat> / request = state<Requested>,
        state<Requested> + event<Reserve> / reserve = state<Held>,
        state<Held> + event<OfferSetup> = state<SetupOffered>,
        state<SetupOffered> + event<Grant> / grant = state<Granted>,
        state<Granted> + event<TakeReady> = state<Readied>,
        state<Readied> + event<Stream> / stream = state<Streaming>,
        state<Unseated> + event<Vacate> = state<Vacated>,
        state<Requested> + event<Vacate> = state<Vacated>,
        state<Held> + event<Vacate> / release = state<Vacated>,
        state<SetupOffered> + event<Vacate> / unjoin = state<Vacated>,
        state<Granted> + event<Vacate> / unjoin = state<Vacated>,
        state<Readied> + event<Vacate> / unjoin = state<Vacated>,
        state<Streaming> + event<Vacate> / unstream = state<Vacated>,

        *state<InputFresh> + event<Neutralize> / neutralize =
            state<InputNeutral>,
        state<InputFresh> + event<Touch> / unchanged,
        state<InputNeutral> + event<Touch> = state<InputFresh>);
  }
};

/// @brief The host's progress with one guest, over the player's outbox.
///
/// Owned by one InvitedPlayer and used under its owner's lock; not
/// thread-safe, neither copyable nor movable since the machine refers to its
/// context by address. Events the table does not allow are refused.
class HostProgress {
 public:
  /// @brief Starts every region in its initial state.
  /// @param outbox The player's outbox; must outlive the machine.
  explicit HostProgress(Outbox<InvitedEffect>& outbox)
      : context_{&outbox}, machine_(context_) {}
  /// @brief Not copyable; the machine refers to its context.
  HostProgress(const HostProgress&) = delete;
  /// @brief Not copy-assignable; the machine refers to its context.
  HostProgress& operator=(const HostProgress&) = delete;
  /// @brief Not movable; the machine refers to its context.
  HostProgress(HostProgress&&) = delete;
  /// @brief Not move-assignable; the machine refers to its context.
  HostProgress& operator=(HostProgress&&) = delete;

  /// @brief Feeds an event.
  /// @tparam Event One of the host events.
  /// @param event The event.
  /// @return Whether the table allowed it.
  template <class Event>
  auto process(const Event& event) -> bool {
    return machine_.process_event(event);
  }
  /// @brief Tells whether a region is in a state.
  /// @tparam State One of the host states.
  /// @return True when some region is in it.
  template <class State>
  auto in() const noexcept -> bool {
    return machine_.is(sml::state<State>);
  }
  /// @brief Tells whether the seat was granted, so taking it back sends
  /// SeatRevoked.
  /// @return True at Granted, Readied and Streaming.
  auto granted() const noexcept -> bool {
    return in<Granted>() || in<Readied>() || in<Streaming>();
  }
  /// @brief The guest's Kakusan seat.
  /// @return The binding, once reserved; kept after ending.
  auto binding() const noexcept -> std::optional<net::kakusan::SeatBinding> {
    return context_.binding;
  }

 private:
  /// @brief The outbox and the seat.
  HostContext context_;
  /// @brief The SML machine.
  sml::sm<HostFsm> machine_;
};

}  // namespace libchiaking::kakusan::player_lifecycle
