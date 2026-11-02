#include "src/core/play/adapters/links/session_diagnostics.hpp"

#include "src/core/play/adapters/links/link_failure.hpp"

#include <magic_enum/magic_enum.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::play {

namespace {

/// @brief Lists the AV lanes that carried audio, as `tag:units` pairs.
///
/// @param units Audio units received per lane tag.
/// @return The pairs separated by commas, or `none`.
auto audio_lanes(const decltype(net::protocol::remote_play::StreamDiagnostics::
                                    audio_lane_units)& units) -> std::string {
  std::string text;
  for (std::size_t tag = 0; tag < units.size(); ++tag) {
    if (units[tag] == 0) continue;
    if (!text.empty()) text += ',';
    text += std::format("{}:{}", tag, units[tag]);
  }
  return text.empty() ? std::string{"none"} : text;
}

}  // namespace

void SessionDiagnostics::stage(
    net::protocol::remote_play::FailureStage stage) noexcept {
  const auto bit = std::uint64_t{1} << static_cast<unsigned>(stage);
  if (seen_ & bit) return;
  seen_ |= bit;
  began_[static_cast<unsigned>(stage)] = now();
  record(LinkStages::operation(stage), diagnostics::Outcome::begin);
}

void SessionDiagnostics::complete(
    net::protocol::remote_play::FailureStage stage) noexcept {
  const auto index = static_cast<unsigned>(stage);
  const auto bit = std::uint64_t{1} << index;
  if (completed_ & bit) return;
  completed_ |= bit;
  const auto end = now();
  record(LinkStages::operation(stage), diagnostics::Outcome::succeeded, 0,
         end >= began_[index] ? end - began_[index] : 0);
}

void SessionDiagnostics::failure(const LinkFailure& failure,
                                 bool cleanup) const noexcept {
  if (!sink()) return;
  const bool cancelled = failure.cancelled();
  try {
    write(
        cancelled ? platform::LogSeverity::info : platform::LogSeverity::error,
        std::format(
            "{} operation={} outcome={} stage={} result={} HTTP={} "
            "reason=0x{:08x} "
            "received={} sent={} detail=\"{}\"",
            diagnostics::name(component()), cleanup ? "cleanup" : "session",
            cancelled ? "cancelled" : "failed",
            diagnostics::name(LinkStages::operation(failure.stage())),
            magic_enum::enum_name(failure.result()), failure.http_status(),
            failure.console_reason(), failure.received_bytes(),
            failure.sent_bytes(), failure.describe()));
  } catch (...) {
  }
}

void SessionDiagnostics::event(
    const net::protocol::remote_play::StreamEvent& event) noexcept {
  using diagnostics::Operation;
  using diagnostics::Outcome;
  if (const auto* state =
          std::get_if<net::protocol::remote_play::event::StateChanged>(&event))
    record_state(Operation::stream_state, state->state);
  if (std::holds_alternative<
          net::protocol::remote_play::event::StreamInfoReady>(event))
    record(Operation::stream_info, Outcome::succeeded);
  if (const auto* media =
          std::get_if<net::protocol::remote_play::event::EncodedMedia>(
              &event)) {
    bool* first =
        media->kind == net::protocol::remote_play::EncodedMediaKind::video
            ? &video_
        : media->kind == net::protocol::remote_play::EncodedMediaKind::audio
            ? &audio_
            : nullptr;
    if (first && !*first) {
      *first = true;
      record(media->kind == net::protocol::remote_play::EncodedMediaKind::video
                 ? Operation::first_video
                 : Operation::first_audio,
             Outcome::observed, media->bytes.size());
    }
  }
}

void SessionDiagnostics::active(
    net::protocol::remote_play::ActiveHandshakeState state) noexcept {
  if (active_ == state) return;
  active_ = state;
  if (!sink()) return;
  try {
    write(platform::LogSeverity::info,
          std::format("{} active_state={}", diagnostics::name(component()),
                      magic_enum::enum_name(state)));
  } catch (...) {
  }
}

auto SessionDiagnostics::health_due() const noexcept -> bool {
  return !last_health_ ||
         now() - *last_health_ >=
             net::protocol::remote_play::health_log_interval_us;
}

void SessionDiagnostics::health(
    const net::protocol::remote_play::StreamDiagnostics& value) noexcept {
  if (!health_due()) return;
  last_health_ = now();
  if (!sink() || !value.available) return;
  try {
    write(
        platform::LogSeverity::info,
        std::format(
            "{} media_health video_units={} assembled={} rejected={} crypto={} "
            "dependency={} repaired={} drop_requested={} assembly={} stale={} "
            "metadata={} "
            "duplicate={} "
            "invalid={} late_parity={} "
            "dropped_posts={} reorder_skipped={} reorder_dropped={} "
            "send_dropped={} lost_ranges={} unrepaired={} socket_buffer={} "
            "late_reads={} longest_read_gap_us={} fec_repaired={} "
            "audio_lanes={}",
            diagnostics::name(component()), value.video_units,
            value.assembled_video, value.rejected_packets,
            value.crypto_rejections, value.dependency_rejections,
            value.repaired_frames, value.dropped_on_request,
            value.assembly_rejections, value.video_stale, value.video_metadata,
            value.video_duplicate, value.video_invalid, value.video_late_parity,
            value.dropped_posts, value.reorder_skipped, value.reorder_dropped,
            value.send_dropped, value.lost_ranges, value.unrepaired_ranges,
            value.socket_receive_buffer, value.late_reads,
            value.longest_read_gap_us, value.video_fec_repaired,
            audio_lanes(value.audio_lane_units)));
    if (const auto& round_trip = value.round_trip; round_trip.present)
      write(
          platform::LogSeverity::info,
          std::format("{} round_trip samples={} smoothed_us={} "
                      "variation_us={} median_us={} p90_us={} p99_us={} "
                      "max_us={}",
                      diagnostics::name(component()), round_trip.spread.samples,
                      round_trip.smoothed_us, round_trip.variation_us,
                      round_trip.spread.median_us, round_trip.spread.p90_us,
                      round_trip.spread.p99_us, round_trip.spread.max_us));
    if (value.lost_ranges != reported_lost_ranges_ ||
        value.late_reads != reported_late_reads_) {
      write(platform::LogSeverity::warning,
            std::format("{} video loss: {} lost ranges, {} not repaired by "
                        "FEC; {} reorder skips; {} late socket reads, longest "
                        "{} us; socket buffer {} bytes",
                        diagnostics::name(component()), value.lost_ranges,
                        value.unrepaired_ranges, value.reorder_skipped,
                        value.late_reads, value.longest_read_gap_us,
                        value.socket_receive_buffer));
      reported_lost_ranges_ = value.lost_ranges;
      reported_late_reads_ = value.late_reads;
    }
    if (value.video_counts.packets != reported_video_count_packets_) {
      const auto& counts = value.video_counts;
      write(platform::LogSeverity::warning,
            std::format("{} video_counts packets={} first_frame={} "
                        "first_unit={} first_total={} first_fec={} "
                        "first_video_word={} first_bytes={}",
                        diagnostics::name(component()), counts.packets,
                        counts.frame, counts.unit, counts.total, counts.fec,
                        counts.video_word, counts.bytes));
      reported_video_count_packets_ = counts.packets;
    }
    const auto& input = value.input;
    write(
        platform::LogSeverity::info,
        std::format(
            "{} input state_posts={} state_refused={} state_sequence={} "
            "history_posts={} history_refused={} history_sequence={} "
            "history_wraps={} events={} windows={} shed={} waiting={} "
            "resends={} ignored={} seats=0x{:x} held=0x{:x},0x{:x},0x{:x},"
            "0x{:x} latched=0x{:x},0x{:x},0x{:x},0x{:x}",
            diagnostics::name(component()), input.state_posts,
            input.state_refused, input.state_sequence, input.history_posts,
            input.history_refused, input.history_sequence, input.history_wraps,
            input.history_events, input.windows_queued, input.windows_shed,
            input.windows_waiting, input.resends, input.ignored_samples,
            input.active_seats, input.held_buttons[0], input.held_buttons[1],
            input.held_buttons[2], input.held_buttons[3],
            input.latched_buttons[0], input.latched_buttons[1],
            input.latched_buttons[2], input.latched_buttons[3]));
    const auto& reported = reported_input_;
    if (input.state_refused != reported.state_refused ||
        input.history_refused != reported.history_refused ||
        input.windows_shed != reported.windows_shed ||
        input.history_wraps != reported.history_wraps) {
      write(platform::LogSeverity::warning,
            std::format("{} input trouble: {} state and {} history packets "
                        "refused; {} history windows shed; history sequence "
                        "wrapped {} times",
                        diagnostics::name(component()), input.state_refused,
                        input.history_refused, input.windows_shed,
                        input.history_wraps));
      reported_input_ = input;
    }
    if (!value.audio.present) return;
    const auto& audio = value.audio;
    write(platform::LogSeverity::info,
          std::format(
              "{} cloud_audio top_channels={} main_present={} main_channels={} "
              "profile_bits=0x{:08x} channel_types=0x{:08x} lanes=0x{:08x} "
              "first_present={} lane={} profile={} mode={} "
              "frame={} unit={} total={} fec={} packet_bytes={} "
              "current_channels={} "
              "selection_present={} selection_lane={} selection_profile={}",
              diagnostics::name(component()), audio.top_channels,
              audio.main_present ? 1U : 0U, audio.main_channels,
              audio.main_profile_bits, audio.channel_type_bits, audio.lane_bits,
              audio.packet_present ? 1U : 0U, audio.packet_lane,
              audio.packet_profile, audio.packet_mode, audio.packet_frame,
              audio.packet_unit, audio.packet_total, audio.packet_fec,
              audio.packet_bytes, audio.current_channels,
              audio.selection_present ? 1U : 0U, audio.selection_lane,
              audio.selection_profile));
  } catch (...) {
  }
}

auto SessionDiagnostics::outcome_trace(
    const std::optional<LinkFailure>& primary,
    const std::optional<LinkFailure>& cleanup,
    const net::protocol::remote_play::QuitDetail& quit) -> std::string {
  std::string text;
  const auto field = [&text](std::string_view value) {
    if (!text.empty()) text += ' ';
    text += value;
  };
  if (primary)
    text = std::format(
        "route={} stage={} result={} HTTP={} reason=0x{:08x} received={} "
        "cancelled={} detail=\"{}\"",
        primary->route() ? primary->route()->name()
                         : std::string_view{"unknown"},
        magic_enum::enum_name(primary->stage()),
        magic_enum::enum_name(primary->result()), primary->http_status(),
        primary->console_reason(), primary->received_bytes(),
        primary->cancelled() ? 1 : 0, primary->describe());
  if (cleanup)
    text += std::format(" cleanup: stage={} result={} detail=\"{}\"",
                        magic_enum::enum_name(cleanup->stage()),
                        magic_enum::enum_name(cleanup->result()),
                        cleanup->describe());
  if (quit.reason != net::protocol::remote_play::QuitReason::none) {
    field(std::format("quit={}", magic_enum::enum_name(quit.reason)));
    if (!quit.text.empty())
      field(std::format("console_reason=\"{}\"",
                        quit.text.substr(0, console_reason_limit)));
  }
  if (primary && !primary->message().empty())
    field(std::format("error_message=\"{}\"",
                      primary->message().substr(0, error_message_limit)));
  return text;
}

void SessionDiagnostics::write(platform::LogSeverity severity,
                               const std::string& line) const noexcept {
  sink()->write({severity, line});
}

}  // namespace libchiaking::play
