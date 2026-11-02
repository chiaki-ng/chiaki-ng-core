#include <libchiaking/platform/http/curl/multi_provider.hpp>

#include <libchiaking/platform/net/default.hpp>
#include <libchiaking/platform/net/types.hpp>

#include "src/core/utils/memory/c_handle.hpp"
#include "src/platform/curl/header_list.hpp"
#include "src/platform/curl/transfer.hpp"
#include "src/platform/http/curl/curl_transfer.hpp"
#include "src/platform/http/curl/transport_log.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace libchiaking::platform {

class CurlMultiHttpProvider::TransferLoop {
  using Instant = std::chrono::microseconds;
  using Outcome = Expected<HttpResponse, HttpError>;
  using CurlHandle = memory::CHandle<CURL, curl_easy_cleanup>;
  using MultiHandle = memory::CHandle<CURLM, curl_multi_cleanup>;
  using ShareHandle = memory::CHandle<CURLSH, curl_share_cleanup>;
  using SlistHandle = memory::CHandle<curl_slist, curl_slist_free_all>;
  using UrlHandle = memory::CHandle<CURLU, curl_url_cleanup>;
  using CurlText = memory::CHandle<char, curl_free>;

  static constexpr std::chrono::milliseconds caller_slice{50};
  static constexpr std::chrono::milliseconds woken_poll{1000};

  class Target {
   public:
    static auto of(const HttpRequest& request) -> Target {
      Target target;
      UrlHandle url(curl_url());
      if (!url) return target;
      const auto text = curl_detail::terminated(request.url.view());
      if (curl_url_set(url.get(), CURLUPART_URL, text.data(), 0) != CURLUE_OK)
        return target;
      char* host = nullptr;
      char* port = nullptr;
      if (curl_url_get(url.get(), CURLUPART_HOST, &host, 0) != CURLUE_OK)
        return target;
      const CurlText owned_host(host);
      if (curl_url_get(url.get(), CURLUPART_PORT, &port, CURLU_DEFAULT_PORT) !=
          CURLUE_OK)
        return target;
      const CurlText owned_port(port);
      target.host_ = owned_host.get();
      target.port_ = owned_port.get();
      return target;
    }

    auto host() const noexcept -> std::string_view { return host_; }

    auto port() const noexcept -> std::string_view { return port_; }

    auto named() const noexcept -> bool {
      return !host_.empty() && !port_.empty() && !host_.starts_with('[') &&
             !parse_ip_address(host_);
    }

   private:
    std::string host_;
    std::string port_;
  };

  class AddressCache {
   public:
    AddressCache(ThreadingProvider& threading, const ClockProvider& clock,
                 std::chrono::seconds lifetime)
        : dns_(threading, clock),
          clock_(clock),
          lifetime_(lifetime),
          guard_(threading) {}

    auto resolve(const Target& target, const StopToken& stop_token,
                 curl_detail::TransportLog& log)
        -> Expected<std::string, HttpError> {
      const std::string key =
          std::format("{}:{}", target.host(), target.port());
      const auto now = now_of(clock_);
      {
        const Lock lock{guard_};
        const auto cached = entries_.find(key);
        if (cached != entries_.end() && cached->second.expires > now)
          return cached->second.resolve;
      }
      const auto found = dns_.resolve(target.host(), target.port());
      if (stop_token.stop_requested()) return unexpected(HttpError::cancelled);
      if (!found) return unexpected(failure_of(found.error()));
      std::string addresses;
      for (const auto& address : found.value()) {
        if (!addresses.empty()) addresses += ',';
        addresses += address.is_ipv6() ? std::format("[{}]", to_string(address))
                                       : to_string(address);
      }
      if (addresses.empty()) return unexpected(HttpError::disconnected);
      std::string entry = std::format("{}:{}", key, addresses);
      log.repeat(curl_detail::TransportLog::Repeating::caller_lookups,
                 LogSeverity::verbose, [&] {
                   return std::format("resolved {} on a caller's thread",
                                      target.host());
                 });
      const Lock lock{guard_};
      std::erase_if(entries_, [now](const auto& item) {
        return item.second.expires <= now;
      });
      entries_.insert_or_assign(
          key, Entry{.resolve = entry, .expires = now + lifetime_});
      return entry;
    }

   private:
    struct Entry {
      std::string resolve;
      Instant expires;
    };

    static auto failure_of(NetError error) noexcept -> HttpError {
      if (error == NetError::timed_out) return HttpError::timed_out;
      return HttpError::disconnected;
    }

    DefaultDnsProvider dns_;
    const ClockProvider& clock_;
    std::chrono::seconds lifetime_;
    Mutex guard_;
    std::unordered_map<std::string, Entry> entries_;
  };

  class Job {
   public:
    Job(ThreadingProvider& threading, const ClockProvider& clock,
        HttpRequest request, StopToken stop_token, Target target,
        std::optional<std::string> resolve)
        : request_(std::move(request)),
          stop_token_(std::move(stop_token)),
          target_(std::move(target)),
          resolve_(std::move(resolve)),
          submitted_(now_of(clock)),
          guard_(threading),
          done_(threading, clock) {}

    auto request() const noexcept -> const HttpRequest& { return request_; }

    auto stop_token() const noexcept -> const StopToken& { return stop_token_; }

    auto target() const noexcept -> const Target& { return target_; }

    auto resolve() const noexcept -> const std::optional<std::string>& {
      return resolve_;
    }

    auto submitted() const noexcept -> Instant { return submitted_; }

    void complete(Outcome outcome) noexcept {
      {
        const Lock lock{guard_};
        if (outcome_) return;
        outcome_.emplace(std::move(outcome));
      }
      done_.notify_all();
    }

    auto wait_for(std::chrono::nanoseconds slice) -> std::optional<Outcome> {
      Lock lock{guard_};
      static_cast<void>(
          done_.wait_for(lock, slice, [this] { return outcome_.has_value(); }));
      if (!outcome_) return std::nullopt;
      return std::move(outcome_);
    }

    void abandon() noexcept {
      abandoned_.store(true, std::memory_order_release);
    }

    auto wanted() const noexcept -> bool {
      return !abandoned_.load(std::memory_order_acquire) &&
             !stop_token_.stop_requested();
    }

    auto resend_once() noexcept -> bool {
      return !resent_.exchange(true, std::memory_order_acq_rel);
    }

   private:
    HttpRequest request_;
    StopToken stop_token_;
    Target target_;
    std::optional<std::string> resolve_;
    Instant submitted_;
    Mutex guard_;
    ConditionVariable done_;
    std::optional<Outcome> outcome_;
    std::atomic<bool> abandoned_{false};
    std::atomic<bool> resent_{false};
  };

  class Batch {
   public:
    static auto create(const CurlMultiSettings& settings, bool multiplex)
        -> Expected<std::unique_ptr<Batch>, HttpError> {
      MultiHandle multi(curl_multi_init());
      if (!multi) return unexpected(HttpError::backend_failure);
      const auto limit = [](std::size_t value) {
        return static_cast<long>(
            std::min<std::size_t>(value, std::numeric_limits<long>::max()));
      };
      if (curl_multi_setopt(multi.get(), CURLMOPT_MAX_HOST_CONNECTIONS,
                            limit(settings.max_host_connections)) != CURLM_OK ||
          curl_multi_setopt(multi.get(), CURLMOPT_MAX_TOTAL_CONNECTIONS,
                            limit(settings.max_total_connections)) !=
              CURLM_OK ||
          curl_multi_setopt(
              multi.get(), CURLMOPT_PIPELINING,
              multiplex ? CURLPIPE_MULTIPLEX : CURLPIPE_NOTHING) != CURLM_OK)
        return unexpected(HttpError::backend_failure);
      try {
        return std::unique_ptr<Batch>(new Batch(std::move(multi), multiplex));
      } catch (...) {
        return unexpected(HttpError::backend_failure);
      }
    }

    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;

    ~Batch() {
      while (!transfers_.empty())
        end(transfers_.size() - 1, unexpected(HttpError::cancelled));
    }

    auto handle() const noexcept -> CURLM* { return multi_.get(); }

    auto running() const noexcept -> std::size_t { return transfers_.size(); }

    void start(const std::shared_ptr<Job>& job, CURLSH* share,
               bool http2_allowed) noexcept {
      try {
        CurlHandle easy(curl_easy_init());
        auto transfer = curl_detail::CurlTransfer::prepare(
            easy.get(), share, job->request(), job->stop_token());
        if (!transfer) {
          job->complete(unexpected(transfer.error()));
          return;
        }
        SlistHandle resolve;
        if (job->resolve()) {
          resolve.reset(curl_slist_append(nullptr, job->resolve()->c_str()));
          if (!resolve || curl_easy_setopt(easy.get(), CURLOPT_RESOLVE,
                                           resolve.get()) != CURLE_OK) {
            job->complete(unexpected(HttpError::backend_failure));
            return;
          }
        }
        if (!http2_allowed &&
            curl_easy_setopt(easy.get(), CURLOPT_HTTP_VERSION,
                             static_cast<long>(CURL_HTTP_VERSION_1_1)) !=
                CURLE_OK) {
          job->complete(unexpected(HttpError::backend_failure));
          return;
        }
        if (multiplex_ &&
            curl_easy_setopt(easy.get(), CURLOPT_PIPEWAIT, 1L) != CURLE_OK) {
          job->complete(unexpected(HttpError::backend_failure));
          return;
        }
        if (curl_multi_add_handle(multi_.get(), easy.get()) != CURLM_OK) {
          job->complete(unexpected(HttpError::backend_failure));
          return;
        }
        transfers_.push_back(Transfer{.job = job,
                                      .easy = std::move(easy),
                                      .resolve = std::move(resolve),
                                      .transfer = std::move(transfer).value()});
      } catch (...) {
        job->complete(unexpected(HttpError::backend_failure));
      }
    }

    void cancel_unwanted() noexcept {
      for (std::size_t index = transfers_.size(); index-- > 0;) {
        if (!transfers_[index].job->wanted())
          end(index, unexpected(HttpError::cancelled));
      }
    }

    auto step(std::vector<std::shared_ptr<Job>>& resending) noexcept -> bool {
      int still_running = 0;
      if (curl_multi_perform(multi_.get(), &still_running) != CURLM_OK) {
        while (!transfers_.empty())
          end(transfers_.size() - 1, unexpected(HttpError::backend_failure));
        return false;
      }
      int queued = 0;
      while (CURLMsg* message = curl_multi_info_read(multi_.get(), &queued)) {
        if (message->msg != CURLMSG_DONE) continue;
        const auto found =
            std::ranges::find_if(transfers_, [message](const Transfer& item) {
              return item.easy.get() == message->easy_handle;
            });
        if (found == transfers_.end()) continue;
        const auto index = static_cast<std::size_t>(found - transfers_.begin());
        if (message->data.result == CURLE_SSL_CONNECT_ERROR &&
            found->job->wanted() && found->job->resend_once()) {
          resend(index, message->data.result, resending);
          continue;
        }
        end(index, found->transfer->finish(message->data.result));
      }
      return true;
    }

    void wait(std::chrono::milliseconds timeout) noexcept {
      const auto bounded = std::min<std::chrono::milliseconds::rep>(
          timeout.count(), std::numeric_limits<int>::max());
      static_cast<void>(curl_multi_poll(multi_.get(), nullptr, 0,
                                        static_cast<int>(bounded), nullptr));
    }

    auto busiest_host() const -> std::pair<std::string, std::size_t> {
      std::map<std::string_view, std::size_t> counts;
      for (const auto& item : transfers_) ++counts[item.job->target().host()];
      const auto top = std::ranges::max_element(
          counts, [](const auto& left, const auto& right) {
            return left.second < right.second;
          });
      if (top == counts.end()) return {std::string{}, 0};
      return {std::string{top->first}, top->second};
    }

   private:
    struct Transfer {
      std::shared_ptr<Job> job;
      CurlHandle easy;
      SlistHandle resolve;
      std::unique_ptr<curl_detail::CurlTransfer> transfer;
    };

    Batch(MultiHandle multi, bool multiplex) noexcept
        : multi_(std::move(multi)), multiplex_(multiplex) {}

    void resend(std::size_t index, CURLcode result,
                std::vector<std::shared_ptr<Job>>& resending) noexcept {
      Transfer finished = std::move(transfers_[index]);
      transfers_.erase(transfers_.begin() + static_cast<std::ptrdiff_t>(index));
      static_cast<void>(
          curl_multi_remove_handle(multi_.get(), finished.easy.get()));
      try {
        resending.push_back(finished.job);
      } catch (...) {
        finished.job->complete(finished.transfer->finish(result));
      }
    }

    void end(std::size_t index, Outcome outcome) noexcept {
      Transfer finished = std::move(transfers_[index]);
      transfers_.erase(transfers_.begin() + static_cast<std::ptrdiff_t>(index));
      static_cast<void>(
          curl_multi_remove_handle(multi_.get(), finished.easy.get()));
      finished.job->complete(std::move(outcome));
    }

    MultiHandle multi_;
    bool multiplex_;
    std::vector<Transfer> transfers_;
  };

  enum class State : std::uint8_t { running, stopping };

  class CallerScope {
   public:
    explicit CallerScope(TransferLoop& loop) noexcept : loop_(loop) {
      const Lock lock{loop_.guard_};
      ++loop_.callers_;
    }

    CallerScope(const CallerScope&) = delete;
    CallerScope& operator=(const CallerScope&) = delete;

    ~CallerScope() {
      {
        const Lock lock{loop_.guard_};
        --loop_.callers_;
      }
      loop_.callers_left_.notify_all();
    }

   private:
    TransferLoop& loop_;
  };

  class Settled final : public PendingHttpResponse::Source {
   public:
    explicit Settled(Outcome outcome) noexcept : outcome_(std::move(outcome)) {}

    auto wait() -> Outcome override { return std::move(outcome_); }

   private:
    Outcome outcome_;
  };

  class Waiting final : public PendingHttpResponse::Source {
   public:
    Waiting(TransferLoop& loop, std::shared_ptr<Job> job) noexcept
        : loop_(loop), job_(std::move(job)) {}

    Waiting(const Waiting&) = delete;
    Waiting& operator=(const Waiting&) = delete;

    ~Waiting() override {
      if (waited_) return;
      job_->abandon();
      loop_.wake();
    }

    auto wait() -> Outcome override {
      const CallerScope caller(loop_);
      waited_ = true;
      try {
        return loop_.wait(*job_);
      } catch (...) {
        return unexpected(HttpError::backend_failure);
      }
    }

   private:
    TransferLoop& loop_;
    std::shared_ptr<Job> job_;
    bool waited_ = false;
  };

 public:
  static auto create(const CurlMultiSettings& settings,
                     const CurlCapabilities& capabilities,
                     ThreadingProvider& threading, const ClockProvider& clock,
                     DiagnosticsProvider* diagnostics)
      -> Expected<std::unique_ptr<TransferLoop>, HttpError> {
    if (!capabilities.multi || !curl_detail::ready())
      return unexpected(HttpError::backend_failure);
    ShareHandle share(curl_share_init());
    if (!share ||
        curl_share_setopt(share.get(), CURLSHOPT_SHARE,
                          CURL_LOCK_DATA_SSL_SESSION) != CURLSHE_OK ||
        curl_share_setopt(share.get(), CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS) !=
            CURLSHE_OK)
      return unexpected(HttpError::backend_failure);
    const bool multiplex =
        settings.http2 == CurlHttp2Policy::when_available && capabilities.http2;
    auto batch = Batch::create(settings, multiplex);
    if (!batch) return unexpected(batch.error());
    try {
      std::unique_ptr<TransferLoop> loop(new TransferLoop(
          settings, capabilities, threading, clock, diagnostics,
          std::move(share), std::move(batch).value()));
      auto worker =
          Thread::start(threading, "curl multi", ThreadRole::transport,
                        [raw = loop.get()] { raw->run(); });
      if (!worker) return unexpected(HttpError::backend_failure);
      loop->worker_ = std::move(worker).value();
      loop->log_.multi_limits(settings, capabilities, loop->dns_);
      return loop;
    } catch (...) {
      return unexpected(HttpError::backend_failure);
    }
  }

  TransferLoop(const TransferLoop&) = delete;
  TransferLoop& operator=(const TransferLoop&) = delete;

  ~TransferLoop() { stop(); }

  void stop() noexcept {
    {
      const Lock lock{guard_};
      state_ = State::stopping;
      wake_locked();
    }
    work_.notify_all();
    if (worker_.joinable()) static_cast<void>(worker_.join());
    Lock lock{guard_};
    callers_left_.wait(lock, [this] { return callers_ == 0; });
  }

  auto perform(const HttpRequest& request, const StopToken& stop_token)
      -> Outcome {
    return start(request, stop_token).wait();
  }

  auto start(const HttpRequest& request, const StopToken& stop_token)
      -> PendingHttpResponse {
    return PendingHttpResponse{queued(request, stop_token)};
  }

  auto queued(const HttpRequest& request, const StopToken& stop_token)
      -> std::unique_ptr<PendingHttpResponse::Source> {
    const CallerScope caller(*this);
    const auto settled =
        [](Outcome outcome) -> std::unique_ptr<PendingHttpResponse::Source> {
      return std::make_unique<Settled>(std::move(outcome));
    };
    try {
      if (const auto checked =
              curl_detail::CurlTransfer::check(request, stop_token);
          !checked)
        return settled(unexpected(checked.error()));
      Target target = Target::of(request);
      std::optional<std::string> resolve;
      if (dns_ == CurlDnsStrategy::caller && target.named() &&
          request.tls_configuration.proxy_policy !=
              HttpProxyPolicy::explicit_proxy) {
        auto entry = addresses_.resolve(target, stop_token, log_);
        if (!entry) return settled(unexpected(entry.error()));
        resolve = std::move(entry).value();
      }
      const auto job =
          std::make_shared<Job>(threading_, clock_, request.clone(), stop_token,
                                std::move(target), std::move(resolve));
      if (!submit(job)) return settled(unexpected(HttpError::cancelled));
      return std::make_unique<Waiting>(*this, job);
    } catch (...) {
      return settled(unexpected(HttpError::backend_failure));
    }
  }

  auto drop_connections() noexcept -> std::size_t {
    {
      const Lock lock{guard_};
      ++drops_requested_;
      wake_locked();
    }
    work_.notify_all();
    return running_.load(std::memory_order_acquire);
  }

  auto capabilities() const noexcept -> const CurlCapabilities& {
    return capabilities_;
  }

  auto dns() const noexcept -> CurlDnsStrategy { return dns_; }

 private:
  TransferLoop(const CurlMultiSettings& settings,
               const CurlCapabilities& capabilities,
               ThreadingProvider& threading, const ClockProvider& clock,
               DiagnosticsProvider* diagnostics, ShareHandle share,
               std::unique_ptr<Batch> batch)
      : threading_(threading),
        clock_(clock),
        settings_(settings),
        capabilities_(capabilities),
        dns_(resolved_dns(settings, capabilities)),
        multiplex_(settings.http2 == CurlHttp2Policy::when_available &&
                   capabilities.http2),
        log_(diagnostics, threading, clock),
        addresses_(threading, clock, settings.dns_cache_lifetime),
        share_(std::move(share)),
        guard_(threading),
        work_(threading, clock),
        callers_left_(threading, clock),
        active_(std::move(batch)) {}

  static auto now_of(const ClockProvider& clock) noexcept -> Instant {
    return Instant{static_cast<Instant::rep>(clock.now_microseconds())};
  }

  static auto resolved_dns(const CurlMultiSettings& settings,
                           const CurlCapabilities& capabilities) noexcept
      -> CurlDnsStrategy {
    if (settings.dns != CurlDnsStrategy::automatic) return settings.dns;
    return capabilities.asynchronous_dns ? CurlDnsStrategy::curl
                                         : CurlDnsStrategy::caller;
  }

  auto submit(const std::shared_ptr<Job>& job) -> bool {
    {
      const Lock lock{guard_};
      if (state_ != State::running) return false;
      pending_.push_back(job);
      wake_locked();
    }
    work_.notify_all();
    return true;
  }

  void wake() noexcept {
    {
      const Lock lock{guard_};
      wake_locked();
    }
    work_.notify_all();
  }

  void wake_locked() noexcept {
    if (capabilities_.wakeup && active_)
      static_cast<void>(curl_multi_wakeup(active_->handle()));
  }

  auto last_turn() const noexcept -> Instant {
    return Instant{last_turn_.load(std::memory_order_acquire)};
  }

  void mark_turn() noexcept {
    last_turn_.store(now_of(clock_).count(), std::memory_order_release);
  }

  auto wait(Job& job) -> Outcome {
    bool woken = false;
    for (;;) {
      if (auto outcome = job.wait_for(caller_slice)) return std::move(*outcome);
      if (!woken && job.stop_token().stop_requested()) {
        wake();
        woken = true;
      }
      const auto now = now_of(clock_);
      const auto staleness = now - std::max(last_turn(), job.submitted());
      const auto total = job.request().total_timeout;
      const bool overdue =
          total.count() > 0 &&
          now - job.submitted() > total + settings_.stall_timeout;
      if (staleness <= settings_.stall_timeout && !overdue) continue;
      job.abandon();
      wake();
      report_stall(job, now, staleness);
      return unexpected(HttpError::timed_out);
    }
  }

  void report_stall(const Job& job, Instant now, Instant staleness) noexcept {
    using std::chrono::duration_cast;
    using std::chrono::milliseconds;
    const auto silent = duration_cast<milliseconds>(staleness).count();
    const auto waited =
        duration_cast<milliseconds>(now - job.submitted()).count();
    const auto queued = running_.load(std::memory_order_acquire);
    log_.repeat(curl_detail::TransportLog::Repeating::stall,
                LogSeverity::warning, [&] {
                  return std::format(
                      "the transfer loop has not turned for {} ms "
                      "with {} transfers running",
                      silent, queued);
                });
    log_.repeat(
        curl_detail::TransportLog::Repeating::stall_timeout, LogSeverity::error,
        [&] {
          return std::format(
              "{} to {} gave up after {} ms waiting on the transfer loop",
              job.request().label.empty() ? std::string_view{"request"}
                                          : job.request().label,
              job.target().host(), waited);
        });
  }

  void run() noexcept {
    for (;;) {
      std::deque<std::shared_ptr<Job>> arriving;
      {
        Lock lock{guard_};
        work_.wait(lock, [this] {
          return state_ != State::running || !pending_.empty() ||
                 drops_requested_ != drops_applied_ || busy();
        });
        mark_turn();
        if (state_ != State::running) break;
        arriving.swap(pending_);
        if (drops_requested_ != drops_applied_) {
          drops_applied_ = drops_requested_;
          renew_locked();
        }
      }
      for (const auto& job : arriving)
        active_->start(job, share_.get(), multiplex_ || !capabilities_.http2);
      turn();
    }
    shutdown();
  }

  void renew_locked() noexcept {
    auto fresh = Batch::create(settings_, multiplex_);
    if (!fresh) {
      log_.write(LogSeverity::error,
                 "connections could not be dropped because a new multi handle "
                 "could not be created");
      return;
    }
    retiring_.push_back(std::move(active_));
    active_ = std::move(fresh).value();
  }

  auto busy() const noexcept -> bool {
    return active_->running() != 0 ||
           std::ranges::any_of(retiring_, [](const auto& batch) {
             return batch->running() != 0;
           });
  }

  void turn() noexcept {
    std::vector<std::shared_ptr<Job>> resending;
    active_->cancel_unwanted();
    if (!active_->step(resending))
      log_.write(LogSeverity::error,
                 "libcurl failed to drive the transfers; they were failed");
    for (const auto& batch : retiring_) {
      batch->cancel_unwanted();
      static_cast<void>(batch->step(resending));
    }
    if (!resending.empty()) {
      {
        const Lock lock{guard_};
        renew_locked();
      }
      log_.write(LogSeverity::warning,
                 "a TLS connection could not be set up; idle connections were "
                 "closed and the request sent again");
    }
    std::erase_if(retiring_,
                  [](const auto& batch) { return batch->running() == 0; });
    for (const auto& job : resending)
      active_->start(job, share_.get(), multiplex_ || !capabilities_.http2);
    std::size_t running = active_->running();
    for (const auto& batch : retiring_) running += batch->running();
    running_.store(running, std::memory_order_release);
    report_queue();
    mark_turn();
    if (running != 0)
      active_->wait(capabilities_.wakeup && retiring_.empty()
                        ? woken_poll
                        : settings_.busy_poll);
  }

  void report_queue() noexcept {
    try {
      const auto total = active_->running();
      if (settings_.max_total_connections != 0 &&
          total > settings_.max_total_connections) {
        log_.repeat(curl_detail::TransportLog::Repeating::connection_queue,
                    LogSeverity::verbose, [&] {
                      return std::format(
                          "{} transfers share at most {} "
                          "connections; the rest wait in libcurl",
                          total, settings_.max_total_connections);
                    });
        return;
      }
      if (settings_.max_host_connections == 0 || multiplex_) return;
      const auto [host, count] = active_->busiest_host();
      if (count > settings_.max_host_connections)
        log_.repeat(curl_detail::TransportLog::Repeating::connection_queue,
                    LogSeverity::verbose, [&] {
                      return std::format(
                          "{} transfers to {} share at most {} "
                          "connections; the rest wait in libcurl",
                          count, host, settings_.max_host_connections);
                    });
    } catch (...) {
    }
  }

  void shutdown() noexcept {
    std::deque<std::shared_ptr<Job>> abandoned;
    {
      const Lock lock{guard_};
      abandoned.swap(pending_);
    }
    for (const auto& job : abandoned)
      job->complete(unexpected(HttpError::cancelled));
    retiring_.clear();
    std::unique_ptr<Batch> closing;
    {
      const Lock lock{guard_};
      closing = std::move(active_);
    }
    running_.store(0, std::memory_order_release);
  }

  ThreadingProvider& threading_;
  const ClockProvider& clock_;
  CurlMultiSettings settings_;
  CurlCapabilities capabilities_;
  CurlDnsStrategy dns_;
  bool multiplex_;
  curl_detail::TransportLog log_;
  AddressCache addresses_;
  ShareHandle share_;
  Mutex guard_;
  ConditionVariable work_;
  ConditionVariable callers_left_;
  State state_ = State::running;
  std::deque<std::shared_ptr<Job>> pending_;
  std::uint64_t drops_requested_ = 0;
  std::uint64_t drops_applied_ = 0;
  std::size_t callers_ = 0;
  std::unique_ptr<Batch> active_;
  std::vector<std::unique_ptr<Batch>> retiring_;
  std::atomic<std::size_t> running_{0};
  std::atomic<Instant::rep> last_turn_{0};
  Thread worker_;
};

auto CurlMultiHttpProvider::create(const CurlMultiSettings& settings,
                                   ThreadingProvider& threading,
                                   const ClockProvider& clock,
                                   DiagnosticsProvider* diagnostics)
    -> Expected<std::unique_ptr<CurlMultiHttpProvider>, HttpError> {
  return create(settings, CurlCapabilities::detect(), threading, clock,
                diagnostics);
}

auto CurlMultiHttpProvider::create(const CurlMultiSettings& settings,
                                   const CurlCapabilities& capabilities,
                                   ThreadingProvider& threading,
                                   const ClockProvider& clock,
                                   DiagnosticsProvider* diagnostics)
    -> Expected<std::unique_ptr<CurlMultiHttpProvider>, HttpError> {
  auto loop = TransferLoop::create(settings, capabilities, threading, clock,
                                   diagnostics);
  if (!loop) return unexpected(loop.error());
  try {
    return std::unique_ptr<CurlMultiHttpProvider>(
        new CurlMultiHttpProvider(std::move(loop).value()));
  } catch (...) {
    return unexpected(HttpError::backend_failure);
  }
}

CurlMultiHttpProvider::CurlMultiHttpProvider(
    std::unique_ptr<TransferLoop> loop) noexcept
    : loop_(std::move(loop)) {}

CurlMultiHttpProvider::~CurlMultiHttpProvider() { loop_->stop(); }

auto CurlMultiHttpProvider::perform(const HttpRequest& request,
                                    const StopToken& stop_token)
    -> Expected<HttpResponse, HttpError> {
  return loop_->perform(request, stop_token);
}

auto CurlMultiHttpProvider::start(const HttpRequest& request,
                                  const StopToken& stop_token)
    -> PendingHttpResponse {
  return loop_->start(request, stop_token);
}

auto CurlMultiHttpProvider::drop_connections() noexcept -> std::size_t {
  return loop_->drop_connections();
}

auto CurlMultiHttpProvider::capabilities() const noexcept
    -> const CurlCapabilities& {
  return loop_->capabilities();
}

auto CurlMultiHttpProvider::dns() const noexcept -> CurlDnsStrategy {
  return loop_->dns();
}

}  // namespace libchiaking::platform
