/// @file
/// @brief Umbrella header for Kakusan: Kakusan, SharedSession and
/// GuestVisit with their value types.
#pragma once

namespace libchiaking::kakusan {

/// @brief Where a host shares a session and a guest joins one. Get it from
/// ChiakiNg::kakusan.
class Kakusan;
/// @brief A play::Session being shared with remote guests, as the host controls
/// it.
class SharedSession;
/// @brief A snapshot of one guest as the host sees them.
struct PlayerSummary;
/// @brief How a guest should be seated on the console.
struct GuestSeating;
/// @brief How the host forwards the stream to guests, for experiments and
/// diagnostics.
struct Tuning;
/// @brief One guest's visit to a host's shared session.
class GuestVisit;
/// @brief How a GuestVisit ended.
class VisitOutcome;

}  // namespace libchiaking::kakusan

#include "libchiaking/detail/kakusan/guest_visit.hpp"
#include "libchiaking/detail/kakusan/kakusan.hpp"
#include "libchiaking/detail/kakusan/shared_session.hpp"
#include "libchiaking/kakusan_types.hpp"
#include "libchiaking/play.hpp"
