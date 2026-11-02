/// @file
/// @brief Declares SearchPlan, the schedule of a network search: which
/// addresses to send a search to and when, and which replies are still
/// fresh.
///
/// The plan is pure bookkeeping on a monotonic clock given in each call; it
/// sends nothing. NetworkSearch asks it for destinations, sends to them and
/// hands it the replies.
#pragma once

#include "libchiaking/detail/consoles/network_search.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/net/interfaces.hpp"
#include "src/core/consoles/model/search/search_reply.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace libchiaking::consoles {

/// @brief When and where one network search probes, and what it has heard.
///
/// Every broadcast_interval the plan probes each local interface's
/// broadcast address, every console that answered recently and every pinned
/// address. When the search ranges ask for unicast, the hosts of each
/// configured subnet that no local interface is on are also swept,
/// batch_size addresses every batch_interval, and the sweep starts over
/// cycle_interval after it finishes. A reply stays fresh for
/// reply_lifetime after it was last heard.
///
/// A plain value; not safe to share between threads.
class SearchPlan {
 public:
  /// @brief Why the interface list was refused.
  enum class Refusal : std::uint8_t {
    /// @brief More than interface_limit usable interfaces were listed.
    too_many_interfaces,
  };

  /// @brief How far the search has got, for a frontend to show.
  struct Progress {
    /// @brief How many unicast addresses one sweep covers.
    std::size_t targets{};
    /// @brief How many of them the current sweep has probed; back to zero
    /// when a sweep finishes.
    std::size_t visited{};
    /// @brief Whether any usable IPv4 interface is up.
    bool network_available{false};
    /// @brief Whether the last probes were refused by the operating system,
    /// such as a missing local network permission. Set by NetworkSearch.
    bool permission_denied{false};
    /// @brief The broadcast addresses probed, in ascending order.
    std::vector<platform::IpAddress> broadcast_addresses;
    /// @brief The local networks those broadcasts reach, as
    /// `address/prefix` labels or, without a prefix length, the broadcast
    /// address.
    std::vector<std::string> broadcast_ranges;
    /// @brief The configured subnets being swept; empty when no network is
    /// available.
    std::vector<Subnet> unicast_ranges;

    /// @brief Compares every field.
    friend auto operator==(const Progress&, const Progress&) noexcept
        -> bool = default;
  };

  /// @brief Most usable interfaces accepted.
  static constexpr std::size_t interface_limit = 128;
  /// @brief Most distinct consoles remembered at once; further ones are
  /// dropped.
  static constexpr std::size_t reply_limit = 128;
  /// @brief Unicast addresses probed per batch.
  static constexpr std::size_t batch_size = 64;
  /// @brief Time between broadcast rounds.
  static constexpr std::chrono::microseconds broadcast_interval{1'000'000};
  /// @brief Time between unicast batches within a sweep.
  static constexpr std::chrono::microseconds batch_interval{200'000};
  /// @brief Time from the end of one unicast sweep to the start of the next.
  static constexpr std::chrono::microseconds cycle_interval{15'000'000};
  /// @brief How long a reply stays fresh after it was last heard.
  static constexpr std::chrono::microseconds reply_lifetime{10'000'000};

  /// @brief Makes a plan with no interfaces yet; nothing is probed until
  /// update_interfaces() lists one, except by a plan that cannot broadcast.
  ///
  /// A plan that cannot broadcast leaves the interfaces' broadcast addresses
  /// out of every round and sends only to unicast addresses: the pinned ones,
  /// the consoles that replied and the swept subnets. Its pinned addresses
  /// and replies are probed whether or not an interface is usable.
  ///
  /// @param ranges What to search: broadcast only, or with unicast subnets.
  /// @param broadcasts Whether the network searched over can broadcast.
  explicit SearchPlan(SearchRanges ranges, bool broadcasts = true) noexcept;

  /// @brief Takes the current list of network interfaces.
  ///
  /// Interfaces that are down, loopback, not IPv4 or unaddressed are ignored.
  /// When the usable list changed, the unicast targets are rebuilt from the
  /// subnets no interface is on and those with a network provider of their
  /// own, remembered replies are dropped and the schedule starts again at
  /// once.
  ///
  /// @param interfaces The interfaces the system lists.
  /// @return True when the usable list changed, false when it is the same.
  /// @retval Refusal::too_many_interfaces More than interface_limit are
  ///     usable; nothing changed.
  [[nodiscard]] auto update_interfaces(
      std::span<const platform::InterfaceInfo> interfaces)
      -> Expected<bool, Refusal>;
  /// @brief Remembers a reply, replacing an earlier one from the same
  /// console.
  ///
  /// @param reply The reply.
  /// @param now_us When it arrived, in monotonic microseconds.
  void receive(SearchReply reply, std::uint64_t now_us);
  /// @brief Adds an address to every broadcast round, such as a known
  /// console's last address. IPv6 addresses and repeats are ignored.
  ///
  /// @param address The address.
  void keep_probing(platform::IpAddress address);
  /// @brief The addresses due a search now, advancing the schedule.
  ///
  /// @param now_us The monotonic time, in microseconds.
  /// @return The addresses, each once and in ascending order; empty when
  ///     nothing is due, or no interface is usable and the plan broadcasts.
  auto destinations(std::uint64_t now_us) -> std::vector<platform::IpAddress>;
  /// @brief The fresh replies.
  ///
  /// @param now_us The monotonic time, in microseconds.
  /// @return One reply per console, sorted by hardware id.
  auto replies(std::uint64_t now_us) const -> std::vector<SearchReply>;
  /// @brief How far the search has got.
  ///
  /// @return The progress; Progress::permission_denied is always false here.
  auto progress() const -> Progress;
  /// @brief When the plan next has something to do: a broadcast round, a
  /// unicast batch or a reply going stale.
  ///
  /// @param now_us The monotonic time, in microseconds.
  /// @return The time in monotonic microseconds, never before `now_us`; or
  ///     nothing when no interface is usable and the plan broadcasts.
  [[nodiscard]] auto next_deadline_us(std::uint64_t now_us) const
      -> std::optional<std::uint64_t>;
  /// @brief How many usable interfaces the plan holds.
  ///
  /// @return The count.
  auto interface_count() const noexcept -> std::size_t {
    return interfaces_.size();
  }
  /// @brief What the plan searches.
  ///
  /// @return The ranges given at construction.
  auto ranges() const noexcept -> const SearchRanges& { return ranges_; }

 private:
  /// @brief A remembered reply.
  struct Seen {
    /// @brief The latest reply from the console.
    SearchReply reply;
    /// @brief When it arrived, in monotonic microseconds.
    std::uint64_t seen_us{};
  };

  /// @brief Bits in an IPv4 address.
  static constexpr unsigned address_bits = 32;

  /// @brief Counts an interval in the microseconds of the monotonic clock.
  ///
  /// @param interval One of the plan's intervals, never negative.
  /// @return Its length in microseconds.
  static constexpr auto interval_us(std::chrono::microseconds interval) noexcept
      -> std::uint64_t {
    return static_cast<std::uint64_t>(interval.count());
  }

  /// @brief Whether a reply is still fresh.
  ///
  /// @param seen_us When it arrived, in monotonic microseconds.
  /// @param now_us The monotonic time, in microseconds.
  /// @return True within reply_lifetime of arrival, and when the clock
  ///     reads earlier than the arrival.
  static constexpr auto alive(std::uint64_t seen_us,
                              std::uint64_t now_us) noexcept -> bool {
    return now_us < seen_us || now_us - seen_us < interval_us(reply_lifetime);
  }

  /// @brief Whether an interface takes part in the search.
  ///
  /// @param interface The interface.
  /// @return True when it is up (administratively, or else operationally),
  ///     not loopback, and has a specified IPv4 address.
  static auto eligible(const platform::InterfaceInfo& interface) noexcept
      -> bool;
  /// @brief Whether an interface has a broadcast address to probe.
  ///
  /// @param interface The interface.
  /// @return True for a specified IPv4 broadcast address.
  static auto broadcasts(const platform::InterfaceInfo& interface) noexcept
      -> bool;
  /// @brief The key interfaces are sorted by, so the same set always compares
  /// equal.
  ///
  /// @param interface The interface.
  /// @return Its address, name, index, broadcast flag, prefix length and
  ///     broadcast address.
  static auto ordering(const platform::InterfaceInfo& interface) noexcept
      -> std::tuple<std::uint32_t, std::string_view, std::uint32_t, bool,
                    std::optional<std::uint8_t>, std::uint32_t>;
  /// @brief Whether two interface entries are identical in every field the
  /// plan reads or reports.
  ///
  /// @param left One entry.
  /// @param right The other.
  /// @return True when they are the same.
  static auto same(const platform::InterfaceInfo& left,
                   const platform::InterfaceInfo& right) noexcept -> bool;
  /// @brief The label of the network an interface broadcasts to.
  ///
  /// @param interface The interface.
  /// @return `network/prefix`, or the broadcast address when the prefix
  ///     length is unknown or invalid.
  static auto range_label(const platform::InterfaceInfo& interface)
      -> std::string;

  /// @brief Drops replies that went stale.
  ///
  /// @param now_us The monotonic time, in microseconds.
  void prune(std::uint64_t now_us);

  /// @brief What the plan searches.
  SearchRanges ranges_;
  /// @brief Whether the interfaces' broadcast addresses are searched.
  bool broadcasts_;
  /// @brief The usable interfaces, sorted by ordering().
  std::vector<platform::InterfaceInfo> interfaces_;
  /// @brief The unicast addresses of one sweep, as IPv4 numbers in ascending
  /// order.
  std::vector<std::uint32_t> targets_;
  /// @brief Addresses added to every broadcast round, as IPv4 numbers.
  std::vector<std::uint32_t> pinned_;
  /// @brief The configured subnets being swept.
  std::vector<Subnet> active_subnets_;
  /// @brief The remembered replies, one per console, at most reply_limit.
  std::vector<Seen> replies_;
  /// @brief Index into #targets_ of the next address to sweep.
  std::size_t cursor_{};
  /// @brief When the next broadcast round is due, in monotonic microseconds.
  std::uint64_t next_broadcast_us_{};
  /// @brief When the next unicast batch is due, in monotonic microseconds.
  std::uint64_t next_sweep_us_{};
};

}  // namespace libchiaking::consoles
