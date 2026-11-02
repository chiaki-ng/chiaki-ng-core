#include <libchiaking/platform/http/curl/provider.hpp>

#include "src/core/utils/memory/c_handle.hpp"
#include "src/platform/curl/transfer.hpp"
#include "src/platform/http/curl/curl_transfer.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace libchiaking::platform {
class CurlHttpProvider::ConnectionCache {
  using CurlHandle = memory::CHandle<CURL, curl_easy_cleanup>;

 public:
  static auto create(ThreadingProvider& threading,
                     const CurlPoolSettings& settings) noexcept
      -> Expected<std::unique_ptr<ConnectionCache>, HttpError> {
    if (!curl_detail::ready()) return unexpected(HttpError::backend_failure);
    memory::CHandle<CURLSH, curl_share_cleanup> native(curl_share_init());
    if (!native) return unexpected(HttpError::backend_failure);
    try {
      std::unique_ptr<ConnectionCache> cache(
          new ConnectionCache(threading, std::move(native), settings));
      const bool configured =
          curl_share_setopt(cache->share_.get(), CURLSHOPT_LOCKFUNC, lock) ==
              CURLSHE_OK &&
          curl_share_setopt(cache->share_.get(), CURLSHOPT_UNLOCKFUNC,
                            unlock) == CURLSHE_OK &&
          curl_share_setopt(cache->share_.get(), CURLSHOPT_USERDATA,
                            cache.get()) == CURLSHE_OK &&
          curl_share_setopt(cache->share_.get(), CURLSHOPT_SHARE,
                            CURL_LOCK_DATA_SSL_SESSION) == CURLSHE_OK &&
          curl_share_setopt(cache->share_.get(), CURLSHOPT_SHARE,
                            CURL_LOCK_DATA_DNS) == CURLSHE_OK;
      if (!configured) return unexpected(HttpError::backend_failure);
      return cache;
    } catch (...) {
      return unexpected(HttpError::backend_failure);
    }
  }

  ConnectionCache(const ConnectionCache&) = delete;
  ConnectionCache& operator=(const ConnectionCache&) = delete;

  [[nodiscard]] static auto perform(ConnectionCache* connections,
                                    const HttpRequest& request,
                                    const StopToken& stop_token)
      -> Expected<HttpResponse, HttpError>;

  auto drop() noexcept -> std::size_t {
    std::vector<CurlHandle> closing;
    closing.reserve(settings_.idle_handles);
    {
      const Lock guard{idle_guard_};
      ++generation_;
      closing.swap(idle_);
    }
    return closing.size();
  }

 private:
  struct Attempt {
    CURLcode code;
    Expected<HttpResponse, HttpError> response;
  };

  [[nodiscard]] static auto attempt(const ConnectionCache* connections,
                                    const HttpRequest& request,
                                    const StopToken& stop_token) -> Attempt;

  class Lease {
   public:
    explicit Lease(const ConnectionCache* owner) noexcept
        : owner_(owner),
          generation_(owner != nullptr ? owner->generation() : 0),
          handle_(owner != nullptr ? owner->idle_handle() : nullptr) {
      if (!handle_) handle_.reset(curl_easy_init());
    }

    [[nodiscard]] auto bound() const noexcept -> bool {
      return owner_ == nullptr || owner_->bound(handle_.get());
    }

    ~Lease() {
      if (owner_ != nullptr && handle_)
        owner_->keep(std::move(handle_), generation_);
    }

    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

    CURL* get() const noexcept { return handle_.get(); }

    explicit operator bool() const noexcept {
      return static_cast<bool>(handle_);
    }

   private:
    const ConnectionCache* owner_;
    std::uint64_t generation_;
    CurlHandle handle_;
  };

  ConnectionCache(ThreadingProvider& threading,
                  memory::CHandle<CURLSH, curl_share_cleanup> share,
                  const CurlPoolSettings& settings)
      : mutexes_(make_mutexes(threading,
                              std::make_index_sequence<CURL_LOCK_DATA_LAST>{})),
        share_(std::move(share)),
        settings_(settings),
        idle_guard_(threading) {
    idle_.reserve(settings_.idle_handles);
  }

  template <std::size_t... Index>
  static auto make_mutexes(ThreadingProvider& threading,
                           std::index_sequence<Index...>)
      -> std::array<Mutex, sizeof...(Index)> {
    return {{(static_cast<void>(Index), Mutex{threading})...}};
  }

  CurlHandle idle_handle() const noexcept {
    const Lock guard{idle_guard_};
    if (idle_.empty()) return nullptr;
    CurlHandle newest = std::move(idle_.back());
    idle_.pop_back();
    return newest;
  }

  [[nodiscard]] auto bound(CURL* curl) const noexcept -> bool {
    if (settings_.connections_per_handle == 0) return true;
    const auto limit = static_cast<long>(std::min<std::size_t>(
        settings_.connections_per_handle, std::numeric_limits<long>::max()));
    return curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, limit) == CURLE_OK;
  }

  std::uint64_t generation() const noexcept {
    const Lock guard{idle_guard_};
    return generation_;
  }

  void keep(CurlHandle handle, std::uint64_t generation) const noexcept {
    curl_easy_reset(handle.get());
    const Lock guard{idle_guard_};
    if (generation != generation_ || idle_.size() >= settings_.idle_handles)
      return;
    idle_.push_back(std::move(handle));
  }

  static void lock(CURL*, curl_lock_data data, curl_lock_access,
                   void* user) noexcept {
    static_cast<ConnectionCache*>(user)->mutex_for(data).lock();
  }

  static void unlock(CURL*, curl_lock_data data, void* user) noexcept {
    static_cast<ConnectionCache*>(user)->mutex_for(data).unlock();
  }

  Mutex& mutex_for(curl_lock_data data) noexcept {
    const auto index = static_cast<std::size_t>(data);
    return mutexes_[index < mutexes_.size() ? index : 0];
  }

  std::array<Mutex, CURL_LOCK_DATA_LAST> mutexes_;
  memory::CHandle<CURLSH, curl_share_cleanup> share_;
  CurlPoolSettings settings_;
  mutable Mutex idle_guard_;
  mutable std::vector<CurlHandle> idle_;
  std::uint64_t generation_ = 0;
};

CurlHttpProvider::CurlHttpProvider(ThreadingProvider& threading,
                                   const CurlPoolSettings& settings) {
  auto cache = ConnectionCache::create(threading, settings);
  if (cache) connections_ = std::move(cache).value();
}

CurlHttpProvider::~CurlHttpProvider() = default;

auto CurlHttpProvider::perform(const HttpRequest& request,
                               const StopToken& stop_token)
    -> Expected<HttpResponse, HttpError> {
  return ConnectionCache::perform(connections_.get(), request, stop_token);
}

auto CurlHttpProvider::drop_connections() noexcept -> std::size_t {
  return connections_ ? connections_->drop() : 0;
}

auto CurlHttpProvider::ConnectionCache::perform(ConnectionCache* connections,
                                                const HttpRequest& request,
                                                const StopToken& stop_token)
    -> Expected<HttpResponse, HttpError> {
  try {
    auto first = attempt(connections, request, stop_token);
    if (first.code != CURLE_SSL_CONNECT_ERROR || connections == nullptr ||
        stop_token.stop_requested())
      return std::move(first.response);
    connections->drop();
    return attempt(connections, request, stop_token).response;
  } catch (...) {
    return unexpected(HttpError::backend_failure);
  }
}

auto CurlHttpProvider::ConnectionCache::attempt(
    const ConnectionCache* connections, const HttpRequest& request,
    const StopToken& stop_token) -> Attempt {
  const Lease curl(connections);
  auto transfer = curl_detail::CurlTransfer::prepare(
      curl.get(), connections != nullptr ? connections->share_.get() : nullptr,
      request, stop_token);
  if (!transfer) return {CURLE_OK, unexpected(transfer.error())};
  if (!curl.bound()) return {CURLE_OK, unexpected(HttpError::backend_failure)};
  const CURLcode code = curl_easy_perform(curl.get());
  return {code, transfer.value()->finish(code)};
}

}  // namespace libchiaking::platform
