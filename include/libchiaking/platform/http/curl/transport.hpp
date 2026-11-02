/// @file
/// @brief Declares the settings that choose and tune the curl HTTP transport,
/// and CurlCapabilities, what the linked libcurl can do.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace libchiaking::platform {

/// @brief Which curl HTTP provider runs.
enum class HttpTransport : std::uint8_t {
  /// @brief CurlHttpProvider: pooled easy handles, each request blocking on
  /// its caller's thread.
  pooled,
  /// @brief CurlMultiHttpProvider: every request on one multi handle driven
  /// by a worker thread. Falls back to pooled when it cannot be created.
  multi,
  /// @brief multi when the capability probe finds a usable multi handle,
  /// otherwise pooled.
  automatic,
};

/// @brief Where the multi provider resolves host names.
enum class CurlDnsStrategy : std::uint8_t {
  /// @brief libcurl resolves inside the transfer loop. Only safe when
  /// libcurl resolves asynchronously; otherwise a slow lookup stalls every
  /// transfer.
  curl,
  /// @brief The caller's thread resolves before the request is queued and
  /// hands the addresses to libcurl with `CURLOPT_RESOLVE`.
  caller,
  /// @brief curl when libcurl resolves asynchronously, otherwise caller.
  automatic,
};

/// @brief Whether the multi provider may multiplex requests over HTTP/2.
enum class CurlHttp2Policy : std::uint8_t {
  /// @brief Never multiplex; every request gets a connection of its own.
  off,
  /// @brief Multiplex when libcurl was built with HTTP/2.
  when_available,
};

/// @brief Tuning for CurlHttpProvider's pool of idle easy handles.
///
/// Every open connection holds a TLS session in the backend, and some
/// backends allow a process only a few. The idle connections the pool may hold
/// are at most idle_handles times connections_per_handle. The defaults suit a
/// desktop; the Nintendo Switch's TLS service wants both near 1.
struct CurlPoolSettings {
  /// @brief Most idle easy handles kept for reuse. A handle returned while
  /// the pool is full is closed with its connections. The most recently
  /// returned handle is reused first, so the others age out under light use.
  std::size_t idle_handles = 8;
  /// @brief Most connections each easy handle keeps open, its
  /// `CURLOPT_MAXCONNECTS`. 0 keeps libcurl's own limit.
  std::size_t connections_per_handle = 5;
};

/// @brief Tuning for CurlMultiHttpProvider.
///
/// The defaults suit a desktop. A console with fewer sockets to spare should
/// lower both connection limits, for example to 4 and 8.
struct CurlMultiSettings {
  /// @brief Most connections open to one host. Further requests to it
  /// queue inside libcurl. 0 means no limit.
  std::size_t max_host_connections = 6;
  /// @brief Most connections open in total. 0 means no limit.
  std::size_t max_total_connections = 16;
  /// @brief Longest wait for socket activity while transfers run. New
  /// requests and cancellations are noticed within this interval when
  /// libcurl cannot be woken.
  std::chrono::milliseconds busy_poll{10};
  /// @brief Where host names are resolved.
  CurlDnsStrategy dns = CurlDnsStrategy::automatic;
  /// @brief Whether requests may share HTTP/2 connections.
  CurlHttp2Policy http2 = CurlHttp2Policy::when_available;
  /// @brief How long the transfer loop may go without a turn while a
  /// request waits on it before the request fails with
  /// HttpError::timed_out. A request also gives up this long after its own
  /// total timeout.
  std::chrono::milliseconds stall_timeout{5000};
  /// @brief How long an address resolved on a caller's thread is reused.
  std::chrono::seconds dns_cache_lifetime{60};
};

/// @brief The transport choice and its tuning, passed when a platform is
/// assembled.
struct HttpTransportSettings {
  /// @brief Which provider runs.
  HttpTransport transport = HttpTransport::pooled;
  /// @brief Tuning used when the multi provider runs.
  CurlMultiSettings multi;
  /// @brief Tuning used when the pooled provider runs, chosen or fallen back
  /// to.
  CurlPoolSettings pool;
  /// @brief Set when libcurl's TLS backend performs the handshake in
  /// blocking mode, as curl-libnx on the Nintendo Switch does. The probe
  /// cannot see it, so the frontend declares it. automatic then keeps the
  /// pooled transport, and a forced multi warns that every new connection
  /// stalls the other transfers.
  bool tls_handshake_blocks = false;
};

/// @brief What the linked libcurl supports, as found at run time.
///
/// Plain data so a frontend can show it and a test can describe a libcurl it
/// does not have, such as the Nintendo Switch build.
struct CurlCapabilities {
  /// @brief A multi handle can be created.
  bool multi = false;
  /// @brief `curl_multi_wakeup` works. Builds configured with
  /// `--disable-socketpair` lack it.
  bool wakeup = false;
  /// @brief libcurl resolves host names without blocking its caller.
  bool asynchronous_dns = false;
  /// @brief libcurl was built with HTTP/2.
  bool http2 = false;
  /// @brief The TLS backend handshakes without blocking, so a new
  /// connection does not stall a multi handle's other transfers. libcurl
  /// cannot report it: detect() assumes it, and
  /// HttpTransportSettings::tls_handshake_blocks withdraws it.
  bool asynchronous_tls_handshake = false;
  /// @brief The libcurl version text.
  std::string version;

  /// @brief Probes the linked libcurl, initialising it on first use.
  ///
  /// @return What the library reports and what a throwaway multi handle
  ///     can do. Every field is false when libcurl cannot be initialised.
  [[nodiscard]] static auto detect() noexcept -> CurlCapabilities;
};

}  // namespace libchiaking::platform
