#include "libchiaking/detail/kakusan/guest_visit.hpp"

#include <string>
#include <utility>

namespace libchiaking::kakusan {

auto VisitOutcome::describe(Status status, std::optional<Refusal> refusal,
                            const std::optional<media::MediaFault>& media)
    -> std::string {
  switch (status) {
    case Status::media_failed:
      return media->stage + " failed (code " + std::to_string(media->cause) +
             ").";
    case Status::visit_failed:
      return "The shared session ended: " + std::string{refusal->name()} + ".";
    case Status::completed:
    case Status::cancelled:
      break;
  }
  return {};
}

VisitOutcome::VisitOutcome(Status status, std::optional<Refusal> refusal,
                           std::optional<media::MediaFault> media)
    : status_(status),
      refusal_(refusal),
      media_(std::move(media)),
      message_(describe(status_, refusal_, media_)) {}

auto VisitOutcome::code() const noexcept -> int {
  switch (status_) {
    case Status::completed:
      return 0;
    case Status::cancelled:
      return cancelled_code;
    case Status::media_failed:
      return media_base + static_cast<int>(media_->kind);
    case Status::visit_failed:
      return refusal_base + refusal_->code();
  }
  return 0;
}

}  // namespace libchiaking::kakusan
