/// @file
/// @brief Declares VersionNegotiation, which decides when the Takion version a
/// console chose is committed.
///
/// The client offers its versions in a TAKIONPROTOCOLREQUEST; the console
/// answers with a TAKIONPROTOCOLREQUESTACK naming one. The choice is committed
/// only once the offer has been acknowledged and no reliable message is in
/// flight either way, so no message straddles the framing change. A choice
/// outside the offer, no choice, or no answer within the negotiation timeout
/// settles on the bootstrap version. A console offering versions of its own
/// is answered with the highest one allowed.
#pragma once

#include "src/core/net/protocol/takion/client_types.hpp"
#include "src/core/net/protocol/takion/profile.hpp"

#include <boost/sml.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>

namespace libchiaking::net::protocol::takion::client_detail {
namespace version_negotiation {
namespace sml = boost::sml;

/// @brief No offer sent yet.
struct UnofferedState {};
/// @brief Offer sent, not yet acknowledged.
struct OfferedState {};
/// @brief Offer acknowledged; no answer yet.
struct DeliveredState {};
/// @brief Answer held; waiting for the offer's acknowledgement.
struct AnsweredState {};
/// @brief Answer held and offer delivered; waiting for a quiet channel.
struct ReadyState {};
/// @brief Version released for commit.
struct CommittedState {};

/// @brief Event: the offer was sent.
struct OfferSent {
  /// @brief When, in monotonic microseconds.
  std::uint64_t now_us{};
};
/// @brief Event: the reliable channel has nothing outstanding.
struct OfferDelivered {};
/// @brief Event: the console's answer arrived.
struct AnswerHeard {
  /// @brief The version it chose, if any.
  std::optional<std::uint32_t> selected_version;
};
/// @brief Event: check the negotiation timeout.
struct WaitChecked {
  /// @brief The current time, in monotonic microseconds.
  std::uint64_t now_us{};
};
/// @brief Event: may a version be committed now?
struct CommitAsked {
  /// @brief Whether no reliable message is in flight either way.
  bool channel_quiet{};
};

/// @brief The negotiation's memory, shared by reference with the machine.
class Context {
 public:
  /// @brief Makes the context.
  ///
  /// @param config The client's configuration; must outlive the context.
  explicit Context(const ClientConfig& config) noexcept : config_(config) {}

  /// @brief Events no transition applied to.
  std::size_t unexpected_events{};

  /// @brief Tells whether the client negotiates.
  ///
  /// @return Whether the selection is NegotiatedTakionProfiles.
  auto negotiated() const noexcept -> bool {
    return std::holds_alternative<NegotiatedTakionProfiles>(
        config_.profile_selection);
  }

  /// @brief Tells whether a version may be chosen.
  ///
  /// @param version The version.
  /// @return Whether it is implemented and in the offer.
  auto allows(std::uint32_t version) const noexcept -> bool {
    const auto* negotiation =
        std::get_if<NegotiatedTakionProfiles>(&config_.profile_selection);
    return negotiation && is_implemented_takion_version(version) &&
           std::ranges::find(negotiation->supported_versions, version) !=
               negotiation->supported_versions.end();
  }

  /// @brief Records when the offer was sent.
  ///
  /// @param now_us When, in monotonic microseconds.
  void note_offer(std::uint64_t now_us) noexcept { offered_us_ = now_us; }

  /// @brief Holds the console's choice, or the bootstrap version when it is not
  /// allowed or absent.
  ///
  /// @param selected_version The console's choice.
  void hold(std::optional<std::uint32_t> selected_version) noexcept {
    chosen_ = selected_version && allows(*selected_version)
                  ? *selected_version
                  : config_.bootstrap_version;
  }

  /// @brief Holds the bootstrap version.
  void hold_bootstrap() noexcept { chosen_ = config_.bootstrap_version; }

  /// @brief Tells whether the console took too long to answer.
  ///
  /// @param now_us The current time, in monotonic microseconds.
  /// @return Whether a timeout is configured and has passed since the offer.
  auto waited_too_long(std::uint64_t now_us) const noexcept -> bool {
    return config_.negotiation_timeout_us && now_us >= offered_us_ &&
           now_us - offered_us_ > *config_.negotiation_timeout_us;
  }

  /// @brief Releases the held version for commit.
  void release() noexcept { released_ = chosen_; }

  /// @brief Takes the released version.
  ///
  /// @return It, once; nothing otherwise.
  auto take_released() noexcept -> std::optional<std::uint32_t> {
    return std::exchange(released_, std::nullopt);
  }

 private:
  /// @brief The client's configuration.
  const ClientConfig& config_;
  /// @brief When the offer was sent, in monotonic microseconds.
  std::uint64_t offered_us_{};
  /// @brief The held version.
  std::uint32_t chosen_{};
  /// @brief The released version, until taken.
  std::optional<std::uint32_t> released_;
};

/// @brief The negotiation's transition table.
struct Fsm {
  /// @brief Builds the table.
  ///
  /// @return The table.
  auto operator()() const noexcept {
    using namespace sml;

    const auto note_offer = [](const OfferSent& event, Context& context) {
      context.note_offer(event.now_us);
    };
    const auto hold = [](const AnswerHeard& event, Context& context) {
      context.hold(event.selected_version);
    };
    const auto hold_bootstrap = [](Context& context) {
      context.hold_bootstrap();
    };
    const auto release = [](Context& context) { context.release(); };
    const auto nothing = [] {};

    const auto negotiated = [](const Context& context) {
      return context.negotiated();
    };
    const auto waited_too_long = [](const WaitChecked& event,
                                    const Context& context) {
      return context.waited_too_long(event.now_us);
    };
    const auto quiet = [](const CommitAsked& event) {
      return event.channel_quiet;
    };

    return make_transition_table(
        *state<UnofferedState> + event<OfferSent> / note_offer =
            state<OfferedState>,
        state<UnofferedState> + event<AnswerHeard>[negotiated] / hold =
            state<ReadyState>,
        state<UnofferedState> + event<AnswerHeard>[!negotiated] / nothing,
        state<UnofferedState> + event<OfferDelivered> / nothing,
        state<UnofferedState> + event<WaitChecked> / nothing,
        state<UnofferedState> + event<CommitAsked> / nothing,

        state<OfferedState> + event<OfferSent> / note_offer,
        state<OfferedState> + event<OfferDelivered> = state<DeliveredState>,
        state<OfferedState> + event<AnswerHeard>[negotiated] / hold =
            state<AnsweredState>,
        state<OfferedState> + event<AnswerHeard>[!negotiated] / nothing,
        state<OfferedState> + event<WaitChecked>[waited_too_long] /
                                  hold_bootstrap = state<AnsweredState>,
        state<OfferedState> + event<WaitChecked>[!waited_too_long] / nothing,
        state<OfferedState> + event<CommitAsked> / nothing,

        state<DeliveredState> + event<OfferSent> / note_offer =
            state<OfferedState>,
        state<DeliveredState> + event<OfferDelivered> / nothing,
        state<DeliveredState> + event<AnswerHeard>[negotiated] / hold =
            state<ReadyState>,
        state<DeliveredState> + event<AnswerHeard>[!negotiated] / nothing,
        state<DeliveredState> + event<WaitChecked>[waited_too_long] /
                                    hold_bootstrap = state<ReadyState>,
        state<DeliveredState> + event<WaitChecked>[!waited_too_long] / nothing,
        state<DeliveredState> + event<CommitAsked> / nothing,

        state<AnsweredState> + event<OfferSent> / nothing,
        state<AnsweredState> + event<OfferDelivered> = state<ReadyState>,
        state<AnsweredState> + event<AnswerHeard> / nothing,
        state<AnsweredState> + event<WaitChecked> / nothing,
        state<AnsweredState> + event<CommitAsked> / nothing,

        state<ReadyState> + event<OfferSent> = state<AnsweredState>,
        state<ReadyState> + event<OfferDelivered> / nothing,
        state<ReadyState> + event<AnswerHeard> / nothing,
        state<ReadyState> + event<WaitChecked> / nothing,
        state<ReadyState> + event<CommitAsked>[quiet] / release =
            state<CommittedState>,
        state<ReadyState> + event<CommitAsked>[!quiet] / nothing,

        state<CommittedState> + event<OfferSent> / nothing,
        state<CommittedState> + event<OfferDelivered> / nothing,
        state<CommittedState> + event<AnswerHeard> / nothing,
        state<CommittedState> + event<WaitChecked> / nothing,
        state<CommittedState> + event<CommitAsked> / nothing);
  }
};

}  // namespace version_negotiation

/// @brief One client's version negotiation. Used under the client's lock; not
/// copyable.
class VersionNegotiation {
 public:
  /// @brief Makes a negotiation that has not offered yet.
  ///
  /// @param config The client's configuration; must outlive this object.
  explicit VersionNegotiation(const ClientConfig& config)
      : context_(config), machine_(context_) {}
  /// @brief Not copyable.
  VersionNegotiation(const VersionNegotiation&) = delete;
  /// @brief Not copy-assignable.
  auto operator=(const VersionNegotiation&) -> VersionNegotiation& = delete;

  /// @brief Records that the offer was sent.
  ///
  /// @param now_us When, in monotonic microseconds.
  void offer_sent(std::uint64_t now_us) {
    process(version_negotiation::OfferSent{now_us});
  }

  /// @brief Records that the reliable channel has nothing outstanding, which
  /// delivers the offer.
  void offer_delivered() { process(version_negotiation::OfferDelivered{}); }

  /// @brief Records the console's answer.
  ///
  /// @param selected_version The version it chose, if any.
  void answer_heard(std::optional<std::uint32_t> selected_version) {
    process(version_negotiation::AnswerHeard{selected_version});
  }

  /// @brief Checks the negotiation timeout.
  ///
  /// @param now_us The current time, in monotonic microseconds.
  void check_wait(std::uint64_t now_us) {
    process(version_negotiation::WaitChecked{now_us});
  }

  /// @brief Asks whether a version can be committed now.
  ///
  /// @param channel_quiet Whether no reliable message is in flight either way.
  /// @return The version to commit, once; nothing otherwise.
  auto commit(bool channel_quiet) -> std::optional<std::uint32_t> {
    process(version_negotiation::CommitAsked{channel_quiet});
    return context_.take_released();
  }

  /// @brief Picks the version to answer a console's own offer with.
  ///
  /// @param offered The console's versions.
  /// @return The highest one implemented and in our offer; nothing when none
  ///     is.
  auto highest_allowed(std::span<const std::uint32_t> offered) const noexcept
      -> std::optional<std::uint32_t> {
    std::optional<std::uint32_t> selected;
    for (const auto version : offered) {
      if (context_.allows(version) && (!selected || version > *selected))
        selected = version;
    }
    return selected;
  }

 private:
  /// @brief Feeds an event to the machine, counting it if refused.
  ///
  /// @tparam Input One of the negotiation events.
  /// @param event The event.
  template <class Input>
  void process(const Input& event) {
    if (!machine_.process_event(event)) ++context_.unexpected_events;
  }

  /// @brief The memory.
  version_negotiation::Context context_;
  /// @brief The machine.
  boost::sml::sm<version_negotiation::Fsm> machine_;
};

}  // namespace libchiaking::net::protocol::takion::client_detail
