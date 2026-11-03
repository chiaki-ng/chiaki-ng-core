#include <libchiaking/detail/secret_array.hpp>
#include <libchiaking/platform/websocket/curl/provider.hpp>

#include "src/core/utils/memory/c_handle.hpp"
#include "src/platform/curl/header_list.hpp"
#include "src/platform/curl/native_trust.hpp"
#include "src/platform/curl/transfer.hpp"
#include "src/platform/net/network_stack.hpp"

#include <curl/curl.h>

#if LIBCHIAKING_CURL_AIA
#include "aia_chain.hpp"
#endif

#if __has_include(<sys/socket.h>)
#include <fcntl.h>
#include <sys/socket.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::platform {
class ActiveWebSocketConnection final : public WebSocketConnection {
 public:
  struct AiaCacheSlot {
    Mutex& guard;
    std::string& intermediates;
  };

 private:
  using HeaderList = curl_detail::HeaderList;

  static constexpr std::size_t receive_chunk_size = 16 * detail::bytes_per_kib;
  static constexpr int socket_wait_quantum_ms = 50;
  static constexpr std::size_t inbound_queue_max_messages = 64;
  static constexpr std::size_t inbound_queue_max_bytes =
      256 * detail::bytes_per_kib;

  using CurlHandle = memory::CHandle<CURL, curl_easy_cleanup>;
  using CurlMulti = memory::CHandle<CURLM, curl_multi_cleanup>;
  using CurlUrl = memory::CHandle<CURLU, curl_url_cleanup>;
  using CurlString = memory::CHandle<char, curl_free>;

  [[nodiscard]] static auto validate_url(std::string_view value)
      -> Expected<void, WebSocketError> {
    const auto scheme = curl_detail::url_scheme(value);
    if (!scheme || (*scheme != "ws" && *scheme != "wss"))
      return unexpected(WebSocketError::invalid_url);
    return {};
  }

  [[nodiscard]] static auto map_curl_error(CURLcode code) noexcept
      -> WebSocketError {
    if (code == CURLE_NOT_BUILT_IN) return WebSocketError::unsupported;
    return curl_detail::failure_of<WebSocketError>(code);
  }

  using Instant = std::chrono::microseconds;

  static auto now_of(const ClockProvider& clock) noexcept -> Instant {
    return Instant{static_cast<Instant::rep>(clock.now_microseconds())};
  }

#if LIBCHIAKING_CURL_AIA
#if defined(CURLSSLOPT_SCHANNEL_AIA)
  static constexpr long aia_schannel_option = CURLSSLOPT_SCHANNEL_AIA;
#else
  static constexpr long aia_schannel_option = 0;
#endif

  static bool aia_schannel_backend() noexcept {
    const curl_version_info_data* version = curl_version_info(CURLVERSION_NOW);
    return version != nullptr && version->ssl_version != nullptr &&
           std::string_view(version->ssl_version).find("Schannel") !=
               std::string_view::npos;
  }

  static bool aia_backend_supported() noexcept {
    const curl_version_info_data* version = curl_version_info(CURLVERSION_NOW);
    if (version == nullptr || version->ssl_version == nullptr) return false;
    if (aia_schannel_backend()) return aia_schannel_option != 0;
    CurlHandle probe(curl_easy_init());
    return probe &&
           curl_easy_setopt(probe.get(), CURLOPT_CAINFO_BLOB,
                            static_cast<curl_blob*>(nullptr)) == CURLE_OK;
  }

  static constexpr unsigned ipv4_address_bits = 32;

  struct Ipv4Block {
    std::uint32_t network;
    unsigned prefix_length;
  };

  static constexpr std::array<Ipv4Block, 14> aia_special_use_ipv4{{
    {0x00000000U, 8},
    {0x0a000000U, 8},
    {0x7f000000U, 8},
    {0x64400000U, 10},
    {0xa9fe0000U, 16},
    {0xac100000U, 12},
    {0xc0a80000U, 16},
    {0xc0000000U, 24},
    {0xc0000200U, 24},
    {0xc0586300U, 24},
    {0xc6120000U, 15},
    {0xc6336400U, 24},
    {0xcb007100U, 24},
    {0xe0000000U, 3},
  }};
  static constexpr std::chrono::milliseconds aia_fetch_timeout{10000};
  static constexpr std::size_t aia_max_download_bytes =
      64 * detail::bytes_per_kib;
  static constexpr long aia_max_redirects = 3;
  static constexpr std::size_t file_chunk_size = 4096;

  [[nodiscard]] static bool aia_public_ipv4(std::uint32_t address) noexcept {
    return std::none_of(
        aia_special_use_ipv4.begin(), aia_special_use_ipv4.end(),
        [address](const Ipv4Block& block) {
          const auto shift = ipv4_address_bits - block.prefix_length;
          return (address >> shift) == (block.network >> shift);
        });
  }

  static curl_socket_t aia_open_socket(void* user, curlsocktype purpose,
                                       curl_sockaddr* address) noexcept {
    const auto& stop_token = *static_cast<const platform::StopToken*>(user);
    if (stop_token.stop_requested() || purpose != CURLSOCKTYPE_IPCXN ||
        address->family != AF_INET)
      return CURL_SOCKET_BAD;
    const auto* bytes =
        reinterpret_cast<const unsigned char*>(&address->addr) + 4;
    const std::uint32_t ipv4 =
        (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) |
        (std::uint32_t{bytes[2]} << 8) | std::uint32_t{bytes[3]};
    if (!aia_public_ipv4(ipv4)) return CURL_SOCKET_BAD;
#if defined(WSA_FLAG_NO_HANDLE_INHERIT)
    return ::WSASocketW(address->family, address->socktype, address->protocol,
                        nullptr, 0,
                        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
#elif defined(SOCK_CLOEXEC)
    return ::socket(address->family, address->socktype | SOCK_CLOEXEC,
                    address->protocol);
#else
    const auto handle =
        ::socket(address->family, address->socktype, address->protocol);
    if (handle != CURL_SOCKET_BAD) (void)::fcntl(handle, F_SETFD, FD_CLOEXEC);
    return handle;
#endif
  }

  [[nodiscard]] static bool aia_configure_transfer(
      CURL* curl, const platform::StopToken& stop_token) noexcept {
    return curl_easy_setopt(curl, CURLOPT_PROXY, "") == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4) ==
               CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https") ==
               CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https") ==
               CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION,
                            aia_open_socket) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, &stop_token) ==
               CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                            curl_detail::stop_on_request) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &stop_token) ==
               CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                            static_cast<long>(aia_fetch_timeout.count())) ==
               CURLE_OK;
  }

  static std::size_t aia_download_write(char* data, std::size_t size,
                                        std::size_t count,
                                        void* user) noexcept {
    auto& body = *static_cast<std::vector<std::byte>*>(user);
    const std::size_t total = size * count;
    if (total > aia_max_download_bytes - body.size()) return 0;
    try {
      const auto* bytes = reinterpret_cast<const std::byte*>(data);
      body.insert(body.end(), bytes, bytes + total);
    } catch (...) {
      return 0;
    }
    return total;
  }

  [[nodiscard]] static auto aia_fetch(const std::string& url,
                                      const platform::StopToken& stop_token)
      -> std::optional<std::vector<std::byte>> {
    CurlHandle curl(curl_easy_init());
    if (!curl || !aia_configure_transfer(curl.get(), stop_token))
      return std::nullopt;
    std::vector<std::byte> body;
    if (curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str()) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION,
                         aia_download_write) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, aia_max_redirects) !=
            CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L) != CURLE_OK)
      return std::nullopt;
    if (curl_easy_perform(curl.get()) != CURLE_OK ||
        stop_token.stop_requested() || body.empty())
      return std::nullopt;
    return body;
  }

  [[nodiscard]] static auto aia_peek_leaf(std::string_view url,
                                          const platform::StopToken& stop_token)
      -> std::optional<std::vector<std::byte>> {
    const auto text = curl_detail::terminated(url);
    CurlUrl parsed(curl_url());
    if (!parsed ||
        curl_url_set(parsed.get(), CURLUPART_URL, text.data(), 0) != CURLUE_OK)
      return std::nullopt;
    char* raw_host = nullptr;
    char* raw_port = nullptr;
    if (curl_url_get(parsed.get(), CURLUPART_HOST, &raw_host, 0) != CURLUE_OK)
      return std::nullopt;
    CurlString host(raw_host);
    if (curl_url_get(parsed.get(), CURLUPART_PORT, &raw_port,
                     CURLU_DEFAULT_PORT) != CURLUE_OK)
      return std::nullopt;
    CurlString port(raw_port);
    const std::string probe =
        std::string("https://") + host.get() + ":" + port.get() + "/";
    CurlHandle curl(curl_easy_init());
    if (!curl || !aia_configure_transfer(curl.get(), stop_token) ||
        curl_easy_setopt(curl.get(), CURLOPT_URL, probe.c_str()) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_CERTINFO, 1L) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_CONNECT_ONLY, 1L) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_FORBID_REUSE, 1L) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 0L) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 0L) != CURLE_OK)
      return std::nullopt;
    if (curl_easy_perform(curl.get()) != CURLE_OK ||
        stop_token.stop_requested())
      return std::nullopt;
    curl_certinfo* info = nullptr;
    if (curl_easy_getinfo(curl.get(), CURLINFO_CERTINFO, &info) != CURLE_OK ||
        info == nullptr || info->num_of_certs <= 0)
      return std::nullopt;
    for (curl_slist* entry = info->certinfo[0]; entry != nullptr;
         entry = entry->next) {
      const std::string_view field(entry->data);
      if (field.rfind("Cert:", 0) != 0) continue;
      const auto pem = field.substr(5);
      return detail::AiaChain::certificate_der(
          std::as_bytes(std::span{pem.data(), pem.size()}));
    }
    return std::nullopt;
  }

  [[nodiscard]] static auto aia_default_trust()
      -> std::optional<detail::AiaTrust> {
    CurlHandle curl(curl_easy_init());
    if (!curl) return std::nullopt;
    char* file = nullptr;
    char* path = nullptr;
    if (curl_easy_getinfo(curl.get(), CURLINFO_CAINFO, &file) != CURLE_OK ||
        curl_easy_getinfo(curl.get(), CURLINFO_CAPATH, &path) != CURLE_OK)
      return std::nullopt;
    detail::AiaTrust trust;
    if (aia_schannel_backend()) {
      if (file != nullptr) return std::nullopt;
      trust.roots = detail::AiaChain::native_roots();
      if (trust.roots.empty()) return std::nullopt;
      return trust;
    }
    if (file == nullptr) return std::nullopt;
    trust.ca_file = file;
    if (path != nullptr) trust.ca_path = path;
    return trust;
  }

  [[nodiscard]] static auto aia_bundle(const AiaCacheSlot& cache,
                                       const detail::AiaTrust& trust)
      -> std::optional<std::string> {
    std::string intermediates;
    {
      const Lock lock{cache.guard};
      intermediates = cache.intermediates;
    }
    if (intermediates.empty()) return std::nullopt;
    if (aia_schannel_backend()) return intermediates;
    std::FILE* file = std::fopen(trust.ca_file.c_str(), "rb");
    if (file == nullptr) return std::nullopt;
    std::string bundle;
    std::array<char, file_chunk_size> chunk{};
    std::size_t read = 0;
    while ((read = std::fread(chunk.data(), 1, chunk.size(), file)) > 0)
      bundle.append(chunk.data(), read);
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) return std::nullopt;
    bundle.push_back('\n');
    bundle += intermediates;
    return bundle;
  }

  [[nodiscard]] static bool aia_recover(const AiaCacheSlot& cache,
                                        std::string_view url,
                                        const detail::AiaTrust& trust,
                                        const platform::StopToken& stop_token) {
    const auto leaf = aia_peek_leaf(url, stop_token);
    if (!leaf) return false;
    const auto recovered = detail::AiaChain::recover_intermediates(
        *leaf,
        [&stop_token](const std::string& issuer) {
          return aia_fetch(issuer, stop_token);
        },
        trust);
    if (!recovered || recovered->empty()) return false;
    const Lock lock{cache.guard};
    if (!cache.intermediates.empty() && cache.intermediates.back() != '\n')
      cache.intermediates.push_back('\n');
    cache.intermediates += *recovered;
    return true;
  }

  static bool certificate_failure(CURL* curl, CURLcode result) noexcept {
    long verify_result = 0;
    (void)curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &verify_result);
    return result == CURLE_PEER_FAILED_VERIFICATION ||
           result == CURLE_SSL_CERTPROBLEM || verify_result != 0;
  }
#endif

  class CurlWebSocketConnection final : public WebSocketConnection {
   public:
    [[nodiscard]] static auto create(const ClockProvider& clock,
                                     CurlHandle handle,
                                     std::size_t maximum_message_bytes,
                                     std::chrono::milliseconds ping_interval,
                                     std::chrono::milliseconds pong_timeout,
                                     WebSocketCloseMode close_mode,
                                     bool ping_immediately)
        -> Expected<std::unique_ptr<CurlWebSocketConnection>, WebSocketError> {
      CurlMulti multi{curl_multi_init()};
      if (multi.get() == nullptr)
        return unexpected(WebSocketError::backend_failure);
      return std::unique_ptr<CurlWebSocketConnection>{
          new CurlWebSocketConnection(
              clock, std::move(handle), std::move(multi), maximum_message_bytes,
              ping_interval, pong_timeout, close_mode, ping_immediately)};
    }

    ~CurlWebSocketConnection() override { (void)close(); }

    [[nodiscard]] auto receive(const platform::StopToken& stop_token)
        -> Expected<WebSocketMessage, WebSocketError> override {
      try {
        if (closed_ || !handle_ || !multi_)
          return unexpected(WebSocketError::disconnected);

        for (;;) {
          if (stop_token.stop_requested())
            return unexpected(WebSocketError::cancelled);

          const auto now = now_of(clock_);
          if (!awaiting_pong_ && ping_interval_.count() > 0 &&
              now >= next_ping_) {
            const auto ping_result = send_control_frame(CURLWS_PING);
            if (!ping_result) return unexpected(ping_result.error());
            awaiting_pong_ = pong_timeout_.count() > 0;
            pong_deadline_ = now + pong_timeout_;
            next_ping_ = now + ping_interval_;
          }

          SecretArray<receive_chunk_size> buffer;
          std::size_t received = 0;
          const curl_ws_frame* metadata = nullptr;
          const CURLcode result =
              curl_ws_recv(handle_.get(), buffer.data(), buffer.size(),
                           &received, &metadata);
          if (result == CURLE_AGAIN) {
            if (awaiting_pong_ && pong_timeout_.count() > 0 &&
                now >= pong_deadline_)
              return unexpected(WebSocketError::timed_out);
            const auto waited =
                wait_for_socket(CURL_WAIT_POLLIN, socket_wait_quantum_ms);
            if (!waited) return unexpected(waited.error());
            continue;
          }
          if (result != CURLE_OK) return unexpected(map_curl_error(result));
          if (metadata == nullptr)
            return unexpected(WebSocketError::protocol_error);

          const int type_flags =
              metadata->flags & (CURLWS_TEXT | CURLWS_BINARY | CURLWS_CLOSE |
                                 CURLWS_PING | CURLWS_PONG);
          if (type_flags == CURLWS_CLOSE) {
            closed_ = true;
            handle_.reset();
            return unexpected(WebSocketError::disconnected);
          }
          if (type_flags == CURLWS_PING) continue;
          if (type_flags == CURLWS_PONG) {
            awaiting_pong_ = false;
            continue;
          }
          if (type_flags != CURLWS_TEXT && type_flags != CURLWS_BINARY)
            return unexpected(WebSocketError::protocol_error);

          const WebSocketMessageType type = type_flags == CURLWS_TEXT
                                                ? WebSocketMessageType::text
                                                : WebSocketMessageType::binary;
          if (message_type_.has_value() && *message_type_ != type)
            return unexpected(WebSocketError::protocol_error);
          message_type_ = type;

          if (received > std::numeric_limits<std::size_t>::max() -
                             message_payload_.size() ||
              (maximum_message_bytes_ != 0 &&
               (message_payload_.size() > maximum_message_bytes_ ||
                received > maximum_message_bytes_ - message_payload_.size())))
            return unexpected(WebSocketError::message_too_large);
          const auto appended = message_payload_.try_append(
              std::span<const std::byte>{buffer.data(), received});
          if (!appended) return unexpected(WebSocketError::backend_failure);

          if (metadata->bytesleft == 0 &&
              (metadata->flags & CURLWS_CONT) == 0) {
            WebSocketMessage message{
                .type = *message_type_,
                .payload = std::move(message_payload_),
            };
            message_type_.reset();
            return message;
          }
        }
      } catch (...) {
        return unexpected(WebSocketError::backend_failure);
      }
    }

    void interrupt_receive() noexcept override {
      if (multi_) static_cast<void>(curl_multi_wakeup(multi_.get()));
    }

    auto close() -> Expected<void, WebSocketError> override {
      if (closed_) return {};
      closed_ = true;
      if (!handle_) return {};
      if (close_mode_ == WebSocketCloseMode::transport_only) {
        handle_.reset();
        return {};
      }
      const auto sent = send_control_frame(CURLWS_CLOSE);
      handle_.reset();
      if (!sent && sent.error() != WebSocketError::disconnected)
        return unexpected(sent.error());
      return {};
    }

   private:
    CurlWebSocketConnection(const ClockProvider& clock, CurlHandle handle,
                            CurlMulti multi, std::size_t maximum_message_bytes,
                            std::chrono::milliseconds ping_interval,
                            std::chrono::milliseconds pong_timeout,
                            WebSocketCloseMode close_mode,
                            bool ping_immediately) noexcept
        : clock_(clock),
          handle_(std::move(handle)),
          multi_(std::move(multi)),
          maximum_message_bytes_(maximum_message_bytes),
          ping_interval_(ping_interval),
          pong_timeout_(pong_timeout),
          next_ping_(now_of(clock) + (ping_immediately
                                          ? std::chrono::milliseconds{0}
                                          : ping_interval)),
          close_mode_(close_mode) {}

    [[nodiscard]] auto wait_for_socket(short events, int timeout_ms)
        -> Expected<void, WebSocketError> {
      curl_socket_t socket = CURL_SOCKET_BAD;
      if (curl_easy_getinfo(handle_.get(), CURLINFO_ACTIVESOCKET, &socket) !=
              CURLE_OK ||
          socket == CURL_SOCKET_BAD)
        return unexpected(WebSocketError::disconnected);
      curl_waitfd wait_fd{
          .fd = socket,
          .events = events,
          .revents = 0,
      };
      int descriptor_count = 0;
      if (curl_multi_poll(multi_.get(), &wait_fd, 1, timeout_ms,
                          &descriptor_count) != CURLM_OK)
        return unexpected(WebSocketError::backend_failure);
      return {};
    }

    [[nodiscard]] auto send_control_frame(unsigned int flags)
        -> Expected<void, WebSocketError> {
      constexpr char empty_payload = '\0';
      const auto deadline = now_of(clock_) + std::chrono::seconds(1);
      for (;;) {
        std::size_t sent = 0;
        const CURLcode result =
            curl_ws_send(handle_.get(), &empty_payload, 0, &sent, 0, flags);
        if (result == CURLE_OK) return {};
        if (result != CURLE_AGAIN) return unexpected(map_curl_error(result));
        if (now_of(clock_) >= deadline)
          return unexpected(WebSocketError::timed_out);
        const auto waited =
            wait_for_socket(CURL_WAIT_POLLOUT, socket_wait_quantum_ms);
        if (!waited) return unexpected(waited.error());
      }
    }

    const ClockProvider& clock_;
    CurlHandle handle_;
    CurlMulti multi_;
    SecretBytes message_payload_;
    std::optional<WebSocketMessageType> message_type_;
    std::size_t maximum_message_bytes_{0};
    std::chrono::milliseconds ping_interval_{0};
    std::chrono::milliseconds pong_timeout_{0};
    Instant next_ping_;
    Instant pong_deadline_{};
    WebSocketCloseMode close_mode_;
    bool awaiting_pong_{false};
    bool closed_{false};
  };

  class StopState final : public platform::StopState {
   public:
    std::atomic<bool> stopped{};
    bool stop_requested() const noexcept override { return stopped.load(); }
  };

 public:
  [[nodiscard]] static auto connect(ThreadingProvider& threading,
                                    const ClockProvider& clock,
                                    const AiaCacheSlot& aia,
                                    const WebSocketRequest& request,
                                    const platform::StopToken& stop_token)
      -> Expected<std::unique_ptr<WebSocketConnection>, WebSocketError>;

  ActiveWebSocketConnection(ThreadingProvider& threading,
                            const ClockProvider& clock,
                            std::unique_ptr<WebSocketConnection> connection)
      : connection_(std::move(connection)),
        stop_(std::make_shared<StopState>()),
        mutex_(threading),
        ready_(threading, clock) {}

  ~ActiveWebSocketConnection() override { (void)close(); }

  [[nodiscard]] auto start(ThreadingProvider& threading) -> bool {
    auto worker = Thread::start(threading, "websocket receive",
                                ThreadRole::control, [this] { run(); });
    if (!worker) return false;
    worker_ = std::move(worker).value();
    return true;
  }

  auto receive(const platform::StopToken& stop)
      -> Expected<WebSocketMessage, WebSocketError> override {
    if (stop.stop_requested()) return unexpected(WebSocketError::cancelled);
    Lock lock{mutex_};
    const bool ready = ready_.wait(lock, stop, [this] {
      return closed_ || !messages_.empty() || error_.has_value();
    });
    if (!ready || stop.stop_requested())
      return unexpected(WebSocketError::cancelled);
    if (closed_) return unexpected(WebSocketError::disconnected);
    if (!messages_.empty()) {
      auto message = std::move(messages_.front());
      messages_.pop_front();
      queued_bytes_ -= message.payload.size();
      return message;
    }
    return unexpected(*error_);
  }

  void interrupt_receive() noexcept override {
    const Lock lock{mutex_};
    ready_.notify_all();
  }

  auto close() -> Expected<void, WebSocketError> override {
    stop_->stopped = true;
    if (worker_.joinable()) {
      connection_->interrupt_receive();
      static_cast<void>(worker_.join());
    }
    const Lock lock{mutex_};
    closed_ = true;
    messages_.clear();
    queued_bytes_ = 0;
    ready_.notify_all();
    if (close_error_) return unexpected(*close_error_);
    return {};
  }

 private:
  void run() noexcept {
    try {
      const platform::StopToken stop{stop_};
      for (;;) {
        auto result = connection_->receive(stop);
        const Lock lock{mutex_};
        if (!result) {
          error_ = result.error();
          break;
        }
        auto message = std::move(result).value();
        if (messages_.size() >= inbound_queue_max_messages ||
            message.payload.size() > inbound_queue_max_bytes - queued_bytes_) {
          error_ = WebSocketError::message_too_large;
          break;
        }
        const auto size = message.payload.size();
        messages_.push_back(std::move(message));
        queued_bytes_ += size;
        ready_.notify_all();
      }
    } catch (...) {
      const Lock lock{mutex_};
      error_ = WebSocketError::backend_failure;
    }
    try {
      auto result = connection_->close();
      if (!result) {
        const Lock lock{mutex_};
        close_error_ = result.error();
      }
    } catch (...) {
      const Lock lock{mutex_};
      close_error_ = WebSocketError::backend_failure;
    }
    ready_.notify_all();
  }

  std::unique_ptr<WebSocketConnection> connection_;
  std::shared_ptr<StopState> stop_;
  Mutex mutex_;
  ConditionVariable ready_;
  std::deque<WebSocketMessage> messages_;
  std::size_t queued_bytes_{};
  std::optional<WebSocketError> error_;
  std::optional<WebSocketError> close_error_;
  bool closed_{};
  Thread worker_;
};

CurlWebSocketProvider::CurlWebSocketProvider(ThreadingProvider& threading,
                                             const ClockProvider& clock)
    : threading_(threading), clock_(clock), intermediates_guard_(threading) {}

auto CurlWebSocketProvider::connect(const WebSocketRequest& request,
                                    const platform::StopToken& stop_token)
    -> Expected<std::unique_ptr<WebSocketConnection>, WebSocketError> {
  return ActiveWebSocketConnection::connect(
      threading_, clock_,
      ActiveWebSocketConnection::AiaCacheSlot{intermediates_guard_,
                                              intermediates_},
      request, stop_token);
}

auto ActiveWebSocketConnection::connect(ThreadingProvider& threading,
                                        const ClockProvider& clock,
                                        const AiaCacheSlot& aia,
                                        const WebSocketRequest& request,
                                        const platform::StopToken& stop_token)
    -> Expected<std::unique_ptr<WebSocketConnection>, WebSocketError> {
  try {
    if (!curl_detail::ready())
      return unexpected(WebSocketError::backend_failure);
    if (stop_token.stop_requested())
      return unexpected(WebSocketError::cancelled);
    const auto valid_url = validate_url(request.url.view());
    if (!valid_url) return unexpected(valid_url.error());
    if (curl_detail::contains_nul(request.subprotocol) ||
        request.subprotocol.find_first_of("\r\n") != std::string::npos)
      return unexpected(WebSocketError::protocol_error);
    if (!std::ranges::all_of(request.headers, [](const HttpHeader& header) {
          return curl_detail::acceptable_header(header.name,
                                                header.value.view());
        }))
      return unexpected(WebSocketError::protocol_error);

    const auto& tls = request.tls_configuration;
    if (const auto usable = curl_detail::usable_tls<WebSocketError>(tls);
        !usable)
      return unexpected(usable.error());
    const auto connect_timeout =
        curl_detail::milliseconds_option(request.connect_timeout);
    if (!connect_timeout || request.ping_interval.count() < 0 ||
        request.pong_timeout.count() < 0)
      return unexpected(WebSocketError::backend_failure);

    HeaderList headers;
    for (const auto& header : request.headers) {
      if (!headers.append(header))
        return unexpected(WebSocketError::backend_failure);
    }
    if (!request.subprotocol.empty() &&
        !headers.append("Sec-WebSocket-Protocol", request.subprotocol))
      return unexpected(WebSocketError::backend_failure);
    struct Attempt {
      CurlHandle curl;
      CURLcode result;
    };
    const auto url = curl_detail::terminated(request.url.view());
    const auto attempt = [&](const std::string* aia_bundle)
        -> Expected<Attempt, WebSocketError> {
#if !LIBCHIAKING_CURL_AIA
      (void)aia_bundle;
      (void)aia;
#endif
      CurlHandle curl(curl_easy_init());
      if (!curl) return unexpected(WebSocketError::backend_failure);
      auto set = [&curl](CURLoption option, auto value) noexcept {
        return curl_easy_setopt(curl.get(), option, value);
      };
      if (set(CURLOPT_URL, url.data()) != CURLE_OK ||
          set(CURLOPT_PROTOCOLS_STR, "ws,wss") != CURLE_OK ||
          set(CURLOPT_CONNECT_ONLY, 2L) != CURLE_OK ||
          set(CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK ||
          set(CURLOPT_SSL_VERIFYPEER, 1L) != CURLE_OK ||
          set(CURLOPT_SSL_VERIFYHOST, 2L) != CURLE_OK ||
          set(CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
          set(CURLOPT_CONNECTTIMEOUT_MS, connect_timeout.value()) != CURLE_OK ||
          set(CURLOPT_TIMEOUT_MS, connect_timeout.value()) != CURLE_OK ||
          set(CURLOPT_XFERINFOFUNCTION, curl_detail::stop_on_request) !=
              CURLE_OK ||
          set(CURLOPT_XFERINFODATA, &stop_token) != CURLE_OK ||
          set(CURLOPT_NOPROGRESS, 0L) != CURLE_OK)
        return unexpected(WebSocketError::backend_failure);

      if (headers.get() != nullptr &&
          set(CURLOPT_HTTPHEADER, headers.get()) != CURLE_OK)
        return unexpected(WebSocketError::backend_failure);

      if (const auto applied = curl_detail::apply_tls<WebSocketError>(
              curl.get(), tls,
              curl_detail::uses_native_ca(tls.trust_mode) ? CURLSSLOPT_NATIVE_CA
                                                          : 0L);
          !applied)
        return unexpected(applied.error());

#if LIBCHIAKING_CURL_AIA
      curl_blob aia_blob{};
      if (aia_bundle != nullptr) {
        aia_blob.data = const_cast<char*>(aia_bundle->data());
        aia_blob.len = aia_bundle->size();
        aia_blob.flags = CURL_BLOB_COPY;
        const long options =
            static_cast<long>(CURLSSLOPT_NO_PARTIALCHAIN) |
            (aia_schannel_backend() ? aia_schannel_option : 0L);
        if (set(CURLOPT_SSL_OPTIONS, options) != CURLE_OK ||
            set(CURLOPT_CAINFO_BLOB, &aia_blob) != CURLE_OK)
          return unexpected(WebSocketError::tls_trust_failed);
      }
#endif
      const CURLcode result = curl_easy_perform(curl.get());
      if (set(CURLOPT_HTTPHEADER, static_cast<curl_slist*>(nullptr)) !=
              CURLE_OK ||
          set(CURLOPT_TIMEOUT_MS, 0L) != CURLE_OK ||
          set(CURLOPT_XFERINFOFUNCTION,
              static_cast<curl_xferinfo_callback>(nullptr)) != CURLE_OK ||
          set(CURLOPT_XFERINFODATA, static_cast<void*>(nullptr)) != CURLE_OK ||
          set(CURLOPT_NOPROGRESS, 1L) != CURLE_OK)
        return unexpected(WebSocketError::backend_failure);
      return Attempt{std::move(curl), result};
    };

#if LIBCHIAKING_CURL_AIA
    const auto trust =
        tls.trust_mode == TlsTrustMode::system && aia_backend_supported()
            ? aia_default_trust()
            : std::nullopt;
    auto bundle = trust ? aia_bundle(aia, *trust) : std::nullopt;
    auto attempted = attempt(bundle ? &*bundle : nullptr);
#else
    auto attempted = attempt(nullptr);
#endif
    if (!attempted) return unexpected(attempted.error());
#if LIBCHIAKING_CURL_AIA
    if (attempted.value().result != CURLE_OK && trust &&
        !stop_token.stop_requested() &&
        certificate_failure(attempted.value().curl.get(),
                            attempted.value().result) &&
        aia_recover(aia, request.url.view(), *trust, stop_token)) {
      bundle = aia_bundle(aia, *trust);
      if (bundle) {
        attempted = attempt(&*bundle);
        if (!attempted) return unexpected(attempted.error());
      }
    }
#endif
    CurlHandle curl = std::move(attempted.value().curl);
    const CURLcode result = attempted.value().result;
    if (result != CURLE_OK) {
      if (result == CURLE_ABORTED_BY_CALLBACK && stop_token.stop_requested())
        return unexpected(WebSocketError::cancelled);
      if (result == CURLE_URL_MALFORMAT &&
          tls.proxy_policy == HttpProxyPolicy::explicit_proxy)
        return unexpected(WebSocketError::proxy_failed);
      return unexpected(map_curl_error(result));
    }

    auto connection = CurlWebSocketConnection::create(
        clock, std::move(curl), request.maximum_message_bytes,
        request.ping_interval, request.pong_timeout, request.close_mode,
        request.ping_immediately);
    if (!connection) return unexpected(connection.error());
    auto active = std::make_unique<ActiveWebSocketConnection>(
        threading, clock, std::move(connection).value());
    if (!active->start(threading))
      return unexpected(WebSocketError::backend_failure);
    return std::unique_ptr<WebSocketConnection>{std::move(active)};
  } catch (...) {
    return unexpected(WebSocketError::backend_failure);
  }
}

}  // namespace libchiaking::platform
