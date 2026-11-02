/// @file
/// @brief Declares GuestProgress, how far the guest has got beyond the shared
/// channel: answering the code exchange, then following the host from
/// admission to playing.
#pragma once

#include "src/core/kakusan/model/player_events.hpp"
#include "src/core/kakusan/model/player_lifecycle.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/controller_frame.hpp"
#include "src/core/net/kakusan/identifiers.hpp"

#include <boost/sml.hpp>

namespace libchiaking::kakusan::player_lifecycle {

/// @brief Guest verification state: waiting for both codes and the host's
/// share. The initial verification state.
struct AwaitingShare {};
/// @brief Guest verification state: PakeB was sent.
struct ShareAnswered {};
/// @brief Guest verification state: the host's ConfirmA matched and ConfirmB
/// was sent.
struct ConfirmationReturned {};

/// @brief Guest seat state: the host has not said admission is pending. The
/// initial seat state.
struct Unadmitted {};
/// @brief Guest seat state: admission is pending and the source setup is
/// awaited.
struct AdmissionAwaited {};
/// @brief Guest seat state: the setup arrived and the forwarded stream is
/// being opened.
struct ReceiverConfigured {};
/// @brief Guest seat state: the stream opened and SourceReady was sent.
struct SourceReadySent {};
/// @brief Guest seat state: the seat was granted and the controller is being
/// prepared.
struct Seated {};
/// @brief Guest seat state: the controller is ready and Ready was sent.
struct ReadySent {};
/// @brief Guest seat state: Active arrived and media reception started.
struct Playing {};

/// @brief Event: the code exchange is answered; queues PakeB.
struct AnswerShare {
  /// @brief The guest's share.
  net::kakusan::control::Share share{};
};
/// @brief Event: the host's ConfirmA matched and ConfirmB is being sent.
struct ReturnConfirmation {};
/// @brief Event: the host said admission is pending.
struct AwaitAdmission {};
/// @brief Event: the source setup was accepted.
struct Configure {};
/// @brief Event: the forwarded stream opened; queues SourceReady.
struct ReportSourceReady {
  /// @brief The hash of the setup accepted.
  net::kakusan::Digest256 setup_hash{};
};
/// @brief Event: the host granted the seat; queues PrepareController.
struct TakeGrant {
  /// @brief The grant.
  net::kakusan::control::SeatGranted grant;
};
/// @brief Event: the controller is ready; queues Ready.
struct ReportReady {
  /// @brief The seat granted.
  net::kakusan::SeatBinding binding;
};
/// @brief Event: the host sent Active; queues StartMediaReception.
struct Activate {
  /// @brief The seat granted.
  net::kakusan::SeatBinding binding;
};

/// @brief The transition table of the guest's progress.
///
/// Two orthogonal regions:
///
/// - verification: AwaitingShare, ShareAnswered (PakeB queued),
///   ConfirmationReturned;
/// - seat: Unadmitted, AdmissionAwaited, ReceiverConfigured, SourceReadySent
///   (SourceReady queued), Seated (PrepareController queued), ReadySent
///   (Ready queued), Playing (StartMediaReception queued); Vacate from any of
///   them reaches Vacated, queuing StopMediaReception once the receiver was
///   configured and StopRumble once playing.
struct GuestFsm {
  /// @brief Builds the transition table for Boost.SML.
  /// @return The table of both regions.
  auto operator()() const noexcept {
    using namespace sml;
    const auto answer = [](const AnswerShare& event,
                           Outbox<VisitingEffect>& outbox) {
      (void)outbox.send(net::kakusan::control::PakeB{event.share});
    };
    const auto source_ready = [](const ReportSourceReady& event,
                                 Outbox<VisitingEffect>& outbox) {
      (void)outbox.send(net::kakusan::control::SourceReady{event.setup_hash});
    };
    const auto seat = [](const TakeGrant& event,
                         Outbox<VisitingEffect>& outbox) {
      outbox.push(PrepareController{event.grant});
    };
    const auto ready = [](const ReportReady& event,
                          Outbox<VisitingEffect>& outbox) {
      (void)outbox.send(net::kakusan::control::Ready{event.binding});
    };
    const auto play = [](const Activate& event,
                         Outbox<VisitingEffect>& outbox) {
      outbox.push(StartMediaReception{event.binding});
    };
    const auto close = [](Outbox<VisitingEffect>& outbox) {
      outbox.push(StopMediaReception{});
    };
    const auto stop = [](Outbox<VisitingEffect>& outbox) {
      outbox.push(StopMediaReception{});
      outbox.push(StopRumble{});
    };
    return make_transition_table(
        *state<AwaitingShare> + event<AnswerShare> / answer =
            state<ShareAnswered>,
        state<ShareAnswered> + event<ReturnConfirmation> =
            state<ConfirmationReturned>,

        *state<Unadmitted> + event<AwaitAdmission> = state<AdmissionAwaited>,
        state<AdmissionAwaited> + event<Configure> = state<ReceiverConfigured>,
        state<ReceiverConfigured> + event<ReportSourceReady> / source_ready =
            state<SourceReadySent>,
        state<SourceReadySent> + event<TakeGrant> / seat = state<Seated>,
        state<Seated> + event<ReportReady> / ready = state<ReadySent>,
        state<ReadySent> + event<Activate> / play = state<Playing>,
        state<Unadmitted> + event<Vacate> = state<Vacated>,
        state<AdmissionAwaited> + event<Vacate> = state<Vacated>,
        state<ReceiverConfigured> + event<Vacate> / close = state<Vacated>,
        state<SourceReadySent> + event<Vacate> / close = state<Vacated>,
        state<Seated> + event<Vacate> / close = state<Vacated>,
        state<ReadySent> + event<Vacate> / close = state<Vacated>,
        state<Playing> + event<Vacate> / stop = state<Vacated>);
  }
};

/// @brief The guest's progress, over the player's outbox.
///
/// Owned by one VisitingPlayer and used under its owner's lock; not
/// thread-safe, neither copyable nor movable since the machine refers to the
/// outbox by address. Events the table does not allow are refused.
class GuestProgress {
 public:
  /// @brief Starts both regions in their initial states.
  /// @param outbox The player's outbox; must outlive the machine.
  explicit GuestProgress(Outbox<VisitingEffect>& outbox) : machine_(outbox) {}
  /// @brief Not copyable; the machine refers to the outbox.
  GuestProgress(const GuestProgress&) = delete;
  /// @brief Not copy-assignable; the machine refers to the outbox.
  GuestProgress& operator=(const GuestProgress&) = delete;
  /// @brief Not movable; the machine refers to the outbox.
  GuestProgress(GuestProgress&&) = delete;
  /// @brief Not move-assignable; the machine refers to the outbox.
  GuestProgress& operator=(GuestProgress&&) = delete;

  /// @brief Feeds an event.
  /// @tparam Event One of the guest events.
  /// @param event The event.
  /// @return Whether the table allowed it.
  template <class Event>
  auto process(const Event& event) -> bool {
    return machine_.process_event(event);
  }
  /// @brief Tells whether a region is in a state.
  /// @tparam State One of the guest states.
  /// @return True when some region is in it.
  template <class State>
  auto in() const noexcept -> bool {
    return machine_.is(sml::state<State>);
  }

 private:
  /// @brief The SML machine.
  sml::sm<GuestFsm> machine_;
};

}  // namespace libchiaking::kakusan::player_lifecycle
