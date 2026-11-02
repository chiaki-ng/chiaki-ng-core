#include "src/core/net/ntp/query.hpp"

#include <array>
#include <span>

namespace libchiaking::net::ntp {

Query::Query(platform::UdpSocket& socket, platform::Poller& poller,
             const platform::ClockProvider& monotonic) noexcept
    : socket_(socket), poller_(poller), monotonic_(monotonic) {}

auto Query::ask(const platform::SocketEndpoint& server, Timestamp nonce,
                const platform::StopToken& stop,
                std::chrono::microseconds timeout) -> Expected<Sample, Error> {
  using platform::PollEvent;
  using platform::PollRequest;
  if (stop.stop_requested()) return unexpected(stopped(stop));
  if (timeout <= std::chrono::microseconds::zero() || server.port == 0 ||
      server.address.is_unspecified() || &socket_.owner() != &poller_.owner())
    return unexpected(Error::invalid_input);
  const auto datagram = Packet::request(nonce);
  const auto sent_us = monotonic_.now_microseconds();
  const auto sent = socket_.send_to(datagram, server);
  if (!sent || sent.value() != datagram.size())
    return unexpected(Error::send_failed);

  const auto deadline_us =
      sent_us + static_cast<std::uint64_t>(timeout.count());
  const PollRequest requests[] = {
      {.pollable = &socket_, .tag = 1U, .interest = PollEvent::readable}};
  for (;;) {
    if (stop.stop_requested()) return unexpected(stopped(stop));
    const auto now_us = monotonic_.now_microseconds();
    if (now_us >= deadline_us) return unexpected(Error::timeout);
    const auto ready = poller_.wait(
        requests, std::chrono::microseconds{deadline_us - now_us}, stop);
    if (!ready) return unexpected(Error::net_failed);
    if (ready.value().stop_requested) return unexpected(stopped(stop));
    for (const auto& event : ready.value().ready) {
      if (event.tag != 1U ||
          (event.ready & PollEvent::readable) == PollEvent::none)
        continue;
      auto answered = answer(server, nonce, sent_us);
      if (!answered) return unexpected(answered.error());
      if (answered.value()) return std::move(*answered.value());
    }
  }
}

auto Query::answer(const platform::SocketEndpoint& server, Timestamp nonce,
                   std::uint64_t sent_us)
    -> Expected<std::optional<Sample>, Error> {
  std::array<std::byte, receive_buffer_size> buffer{};
  const auto received = socket_.receive_from(buffer);
  const auto received_us = monotonic_.now_microseconds();
  if (!received)
    return received.error() == platform::NetError::would_block
               ? Expected<std::optional<Sample>, Error>{std::nullopt}
               : unexpected(Error::net_failed);
  if (received.value().source != server ||
      received.value().size > buffer.size())
    return std::optional<Sample>{};
  const auto reply = Packet::reply(
      std::span<const std::byte>{buffer.data(), received.value().size}, nonce);
  if (!reply)
    return is_stray(reply.error())
               ? Expected<std::optional<Sample>, Error>{std::nullopt}
               : unexpected(Error::rejected);
  auto sample = sample_of(reply.value(), server.address, sent_us, received_us);
  if (!sample) return unexpected(sample.error());
  return std::optional<Sample>{std::move(sample).value()};
}

auto Query::sample_of(const Packet& reply, const platform::IpAddress& server,
                      std::uint64_t sent_us, std::uint64_t received_us)
    -> Expected<Sample, Error> {
  if (received_us < sent_us) return unexpected(Error::rejected);
  const std::chrono::microseconds round_trip{
      static_cast<std::int64_t>(received_us - sent_us)};
  const auto held_by_server =
      reply.transmitted().time() - reply.received().time();
  if (held_by_server < std::chrono::microseconds::zero() ||
      held_by_server > round_trip)
    return unexpected(Error::rejected);
  const auto delay = round_trip - held_by_server;
  if (delay >= max_round_trip) return unexpected(Error::rejected);
  return Sample{.server = server,
                .time = reply.transmitted().time() + delay / 2,
                .at_us = received_us};
}

auto Query::stopped(const platform::StopToken& stop) noexcept -> Error {
  return stop.deadline_expired() ? Error::timeout : Error::cancelled;
}

auto Query::is_stray(Packet::Rejection rejection) noexcept -> bool {
  return rejection == Packet::Rejection::short_packet ||
         rejection == Packet::Rejection::origin_mismatch;
}

}  // namespace libchiaking::net::ntp
