/// @file
/// @brief Fixed values of a running stream: timeouts and intervals, buffer
/// capacities, PSN route request ids and the default feedback pacing.
#pragma once

#include <cstddef>
#include <cstdint>

namespace libchiaking::net::protocol::remote_play {

/// @brief How long each window of the active session's handshake may take:
/// connecting and committing the version, BIG/BANG and the cipher install,
/// and the stream info; in microseconds (5 s).
inline constexpr std::uint64_t handshake_stage_timeout_us = 5'000'000;
/// @brief How long the Takion version negotiation waits for an answer, in
/// microseconds (1 s).
inline constexpr std::uint64_t takion_negotiation_timeout_us = 1'000'000;
/// @brief How long a start waits on the control channel before going on
/// without it, in microseconds (10 s): at home for the session id before one
/// is synthesised, over PSN before the data route is opened.
inline constexpr std::uint64_t ctrl_start_wait_us = 10'000'000;
/// @brief How often a congestion report is sent unless the stream info names
/// another interval, in microseconds (200 ms).
inline constexpr std::uint64_t congestion_report_interval_us = 200'000;
/// @brief The shortest congestion report interval taken from a stream info,
/// in microseconds (50 ms).
inline constexpr std::uint64_t congestion_report_interval_min_us = 50'000;
/// @brief The longest congestion report interval taken from a stream info,
/// in microseconds (2 s).
inline constexpr std::uint64_t congestion_report_interval_max_us = 2'000'000;
/// @brief How long a keyframe request waits for a keyframe before it is
/// sent again, in microseconds (1 s): the request travels as one reliable
/// message, and if it is lost the stream would otherwise wait for a keyframe
/// for ever.
inline constexpr std::uint64_t idr_retry_interval_us = 1'000'000;
/// @brief How many video frames in a row may be refused for a missing
/// reference before a keyframe is requested (10, a sixth of a second at 60
/// frames per second).
inline constexpr std::uint32_t refusals_before_idr = 10;
/// @brief How long a session with a console may receive nothing before it
/// fails, in microseconds (30 s), reset by any packet the console sends. This
/// waits as long as the console's own 30 s control watchdog, since until then
/// a console that still hears this client keeps the session.
inline constexpr std::uint64_t console_peer_silence_timeout_us = 30'000'000;
/// @brief How long the stream may receive nothing from the console before the
/// gap counts as network trouble, in microseconds (1 s).
inline constexpr std::uint64_t network_trouble_gap_us = 1'000'000;
/// @brief How often a Takion heartbeat is sent, in microseconds (1 s).
inline constexpr std::uint64_t heartbeat_interval_us = 1'000'000;
/// @brief How often a session over PSN sends a heartbeat reply on its own
/// control channel while the stream plays, asked for or not, in microseconds
/// (4.95 s). The console resets its 30 s control watchdog only on a heartbeat
/// reply.
inline constexpr std::uint64_t ctrl_keepalive_interval_us = 4'950'000;
/// @brief How long a PSN start waits for the session id before synthesising
/// one, in microseconds (5 s).
inline constexpr std::uint64_t session_id_wait_us = 5'000'000;
/// @brief How long a home start waits after the session request is accepted
/// before sending the control request, in microseconds (10 ms).
inline constexpr std::uint64_t ctrl_ready_delay_us = 10'000;
/// @brief How long each poll of the PSN control route waits, in
/// microseconds (1 ms).
inline constexpr std::uint64_t ctrl_receive_poll_us = 1'000;
/// @brief How long a PSN start idles between steps, in milliseconds.
inline constexpr std::uint64_t wait_slice_ms = 10;
/// @brief The round trip assumed when calibration measured none, in
/// microseconds (1 ms).
inline constexpr std::uint64_t fallback_rtt_us = 1'000;
/// @brief How often stream health is logged, in microseconds (5 s).
inline constexpr std::uint64_t health_log_interval_us = 5'000'000;

/// @brief The largest assembled video frame, in bytes (4 MiB).
inline constexpr std::size_t max_frame_bytes = 4 * 1024 * 1024;
/// @brief The most media bytes queued for the caller, in bytes (4 MiB).
inline constexpr std::size_t media_queue_max_bytes = 4 * 1024 * 1024;
/// @brief The capacity of the control outbound queue, the video reorder
/// queue and the feedback history outbox, in entries.
inline constexpr std::size_t queue_capacity = 64;
/// @brief The capacity of the audio and completion rings, in entries.
inline constexpr std::size_t ring_capacity = 128;
/// @brief The most events a feedback history window holds.
inline constexpr std::size_t feedback_history_capacity = 16;
/// @brief The most units a PS cloud frame may declare.
inline constexpr std::size_t max_units = 512;
/// @brief The most source plus parity units a video frame may have.
inline constexpr std::size_t video_max_total_units = 256;
/// @brief The Takion video codec byte for H.264 (16-pixel macroblocks).
inline constexpr std::uint8_t video_codec_avc = 3;
/// @brief The Takion video codec byte for H.265 (64-pixel coding tree units).
inline constexpr std::uint8_t video_codec_hevc = 6;
/// @brief The capacity of a PS5 launch specification's JSON, in bytes.
inline constexpr std::size_t launch_spec_json_capacity = 2048;
/// @brief How many control frames a home session logs.
inline constexpr std::size_t ctrl_frames_logged_max = 8;
/// @brief The most sockets a PSN start opens to probe its NAT.
inline constexpr std::size_t max_nat_probe_sockets = 4096;
/// @brief How many failure stages the session diagnostics can time.
inline constexpr std::size_t failure_stage_slots = 64;

/// @brief The PSN session request id of the control route's offer.
inline constexpr std::uint64_t ctrl_route_offer_req_id = 2;
/// @brief The PSN session request id of the control route's accept.
inline constexpr std::uint64_t ctrl_route_accept_req_id = 3;
/// @brief The PSN session request id of the data route's offer.
inline constexpr std::uint64_t data_route_offer_req_id = 5;
/// @brief The PSN session request id of the data route's accept.
inline constexpr std::uint64_t data_route_accept_req_id = 6;

/// @brief How often an unchanged controller state is resent, in
/// microseconds (200 ms).
inline constexpr std::uint64_t default_state_refresh_interval_us = 200'000;
/// @brief How far a motion value may move before the state counts as
/// changed.
inline constexpr float default_motion_tolerance = 0.0000001F;
/// @brief How many recent history events each history packet repeats.
inline constexpr auto default_recent_history_events =
    static_cast<std::uint8_t>(feedback_history_capacity);
/// @brief How many times a history window is resent after its first send.
inline constexpr std::uint8_t default_history_repeats = 2;
/// @brief Whether each button, trigger and touch edge is sent as its own
/// history window, so two edges are never merged into one.
inline constexpr bool default_per_edge_flush = true;
/// @brief How long between history resends, in microseconds (20 ms).
inline constexpr std::uint64_t default_history_repeat_interval_us = 20'000;

}  // namespace libchiaking::net::protocol::remote_play
