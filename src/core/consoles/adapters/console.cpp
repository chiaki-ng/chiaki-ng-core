#include "libchiaking/platform/diagnostics/provider.hpp"
#include "src/core/consoles/adapters/roster/console_core.hpp"
#include "src/core/consoles/adapters/titles/console_library.hpp"
#include "src/core/consoles/adapters/titles/title_start.hpp"
#include "src/core/consoles/known_consoles_core.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace libchiaking::consoles {

Console::Core::Core(KnownConsoles::Core& home, ConsoleStore& store,
                    platform::DiagnosticsProvider* log, SavedConsole facts,
                    bool saved)
    : home_(home),
      store_(store),
      log_(log),
      hardware_id_(facts.hardware_id),
      generation_(facts.firmware.generation()),
      changes_(home.threading()),
      mutex_(home.threading()),
      firmware_(facts.firmware),
      name_(std::move(facts.name)),
      last_address_(std::move(facts.last_address)),
      saved_(saved) {}

auto Console::Core::firmware() const -> Firmware {
  platform::Lock lock{mutex_};
  return firmware_;
}

auto Console::Core::name() const -> std::string {
  platform::Lock lock{mutex_};
  return name_.text();
}

auto Console::Core::name_source() const -> ConsoleName::Source {
  platform::Lock lock{mutex_};
  return name_.source();
}

auto Console::Core::last_address() const -> std::optional<Host> {
  platform::Lock lock{mutex_};
  return last_address_;
}

auto Console::Core::presence() const -> Presence {
  platform::Lock lock{mutex_};
  return presence_;
}

auto Console::Core::running_title() const -> std::optional<RunningTitle> {
  platform::Lock lock{mutex_};
  return running_title_;
}

auto Console::Core::is_paired() const -> bool {
  platform::Lock lock{mutex_};
  return !pairings_.empty();
}

auto Console::Core::paired_accounts() const -> std::vector<AccountId> {
  platform::Lock lock{mutex_};
  std::vector<AccountId> accounts;
  accounts.reserve(pairings_.size());
  std::ranges::transform(pairings_, std::back_inserter(accounts),
                         &Pairing::account);
  return accounts;
}

auto Console::Core::rename(std::string name) -> Expected<void, Unchanged> {
  auto wanted =
      ConsoleName::create(std::move(name), ConsoleName::Source::renamed);
  if (!wanted) return unexpected(Unchanged::invalid_name);
  platform::Lock changes{changes_};
  if (forgotten_) return unexpected(Unchanged::forgotten);
  auto next = facts();
  next.name = *wanted;
  if (saved_ && !store_.save(next)) return unexpected(Unchanged::not_saved);
  platform::Lock lock{mutex_};
  name_ = std::move(*wanted);
  return {};
}

auto Console::Core::moved_to(Host host) -> Expected<void, Unchanged> {
  if (!host.reaches(generation_))
    return unexpected(Unchanged::address_not_for_generation);
  platform::Lock changes{changes_};
  if (forgotten_) return unexpected(Unchanged::forgotten);
  auto next = facts();
  next.last_address = host;
  if (saved_ && !store_.save(next)) return unexpected(Unchanged::not_saved);
  platform::Lock lock{mutex_};
  last_address_ = std::move(host);
  return {};
}

auto Console::Core::facts() const -> SavedConsole {
  platform::Lock lock{mutex_};
  return SavedConsole{hardware_id_, firmware_, name_, last_address_};
}

auto Console::Core::holds(AccountId account) const -> bool {
  platform::Lock lock{mutex_};
  return std::ranges::find(pairings_, account, &Pairing::account) !=
         pairings_.end();
}

auto Console::Core::keys_of(AccountId account) const
    -> std::optional<PairingKeys> {
  platform::Lock lock{mutex_};
  const auto found = std::ranges::find(pairings_, account, &Pairing::account);
  if (found == pairings_.end()) return std::nullopt;
  return found->keys.clone();
}

auto Console::Core::device_uid_of(AccountId account) const
    -> std::optional<DeviceUid> {
  platform::Lock lock{mutex_};
  const auto found = std::ranges::find(pairings_, account, &Pairing::account);
  return found != pairings_.end() ? found->device_uid : std::nullopt;
}

auto Console::Core::seen(Firmware firmware,
                         const std::optional<ConsoleName>& name,
                         const Host& address, Presence presence,
                         std::optional<RunningTitle> running_title) -> bool {
  platform::Lock lock{mutex_};
  if (forgotten_) return false;
  bool changed = false;
  if (firmware.generation() == generation_ && firmware != firmware_) {
    firmware_ = firmware;
    changed = true;
  }
  if (name && name->replaces(name_) && *name != name_) {
    name_ = *name;
    changed = true;
  }
  if (address.reaches(generation_) && last_address_ != address) {
    last_address_ = address;
    changed = true;
  }
  presence_ = presence;
  running_title_ = std::move(running_title);
  return changed;
}

auto Console::Core::learn(const ConsoleName& name) -> bool {
  platform::Lock lock{mutex_};
  if (forgotten_ || !name.replaces(name_) || name == name_) return false;
  name_ = name;
  return true;
}

void Console::Core::remembered(const ConsoleName& saved_name) {
  platform::Lock changes{changes_};
  saved_ = true;
  platform::Lock lock{mutex_};
  if (saved_name.replaces(name_)) name_ = saved_name;
}

void Console::Core::lost(Presence presence) {
  platform::Lock lock{mutex_};
  presence_ = presence;
  running_title_.reset();
}

void Console::Core::pair(AccountId account, PairingKeys keys,
                         std::optional<DeviceUid> device_uid) {
  platform::Lock lock{mutex_};
  const auto found = std::ranges::find(pairings_, account, &Pairing::account);
  if (found != pairings_.end()) {
    found->keys = std::move(keys);
    found->device_uid = device_uid;
    return;
  }
  pairings_.push_back(Pairing{account, std::move(keys), device_uid});
}

void Console::Core::keep_facts() {
  platform::Lock changes{changes_};
  if (!saved_ || forgotten_) return;
  if (!store_.save(facts()) && log_ != nullptr)
    log_->write({platform::LogSeverity::warning,
                 "consoles: a console's new facts were not saved"});
}

auto Console::Core::keep_pairing(AccountId account, PairedConsole paired,
                                 const std::optional<Host>& at,
                                 std::optional<DeviceUid> device_uid)
    -> Expected<void, ConsoleStore::Error> {
  platform::Lock changes{changes_};
  const auto previous = facts();
  auto next = previous;
  next.firmware = paired.firmware;
  if (paired.name && paired.name->replaces(next.name)) next.name = *paired.name;
  if (at && at->reaches(generation_)) next.last_address = at;
  if (!device_uid) device_uid = device_uid_of(account);
  const bool was_saved = saved_;
  if (const auto kept = store_.save(next); !kept)
    return unexpected(kept.error());
  if (const auto kept = store_.save(
          SavedPairing{account, hardware_id_, paired.keys.clone(), device_uid});
      !kept) {
    const auto restored =
        was_saved ? store_.save(previous) : store_.remove_console(hardware_id_);
    if (!restored && log_ != nullptr)
      log_->write(
          {platform::LogSeverity::warning,
           "consoles: a console row was left behind by a pairing that was not "
           "saved"});
    return unexpected(kept.error());
  }
  {
    platform::Lock lock{mutex_};
    firmware_ = next.firmware;
    name_ = std::move(next.name);
    last_address_ = std::move(next.last_address);
  }
  saved_ = true;
  pair(account, std::move(paired.keys), device_uid);
  return {};
}

auto Console::Core::link(AccountId account, DeviceUid device_uid)
    -> Expected<void, Unchanged> {
  platform::Lock changes{changes_};
  if (forgotten_) return unexpected(Unchanged::forgotten);
  auto keys = keys_of(account);
  if (!keys) return unexpected(Unchanged::not_paired);
  if (device_uid_of(account) == device_uid) return {};
  if (saved_ && !store_.save(SavedPairing{account, hardware_id_, keys->clone(),
                                          device_uid}))
    return unexpected(Unchanged::not_saved);
  pair(account, std::move(*keys), device_uid);
  return {};
}

auto Console::Core::forget() -> Expected<void, ConsoleStore::Error> {
  platform::Lock changes{changes_};
  if (forgotten_) return {};
  if (saved_) {
    if (const auto removed = store_.remove_console(hardware_id_); !removed)
      return unexpected(removed.error());
  }
  platform::Lock lock{mutex_};
  forgotten_ = true;
  saved_ = false;
  pairings_.clear();
  presence_ = Presence::unknown;
  running_title_.reset();
  return {};
}

auto Console::Core::unpair(AccountId account)
    -> Expected<void, ConsoleStore::Error> {
  platform::Lock changes{changes_};
  if (forgotten_ || !holds(account)) return {};
  if (saved_) {
    if (const auto removed = store_.remove_pairing(account, hardware_id_);
        !removed)
      return unexpected(removed.error());
  }
  platform::Lock lock{mutex_};
  std::erase_if(pairings_, [account](const Pairing& pairing) {
    return pairing.account == account;
  });
  return {};
}

void Console::Core::leave() {
  platform::Lock changes{changes_};
  platform::Lock lock{mutex_};
  forgotten_ = true;
  pairings_.clear();
  presence_ = Presence::unknown;
  running_title_.reset();
}

Console::Console(std::unique_ptr<Core> core)
    : hardware_id_(core->hardware_id()),
      generation_(core->generation()),
      core_(std::move(core)) {}

Console::~Console() = default;

auto Console::firmware() const -> Firmware { return core_->firmware(); }

auto Console::name() const -> std::string { return core_->name(); }

auto Console::name_source() const -> ConsoleName::Source {
  return core_->name_source();
}

auto Console::last_address() const -> std::optional<Host> {
  return core_->last_address();
}

auto Console::presence() const -> Presence { return core_->presence(); }

auto Console::running_title() const -> std::optional<RunningTitle> {
  return core_->running_title();
}

auto Console::is_saved() const noexcept -> bool { return core_->is_saved(); }

auto Console::is_forgotten() const noexcept -> bool {
  return core_->is_forgotten();
}

auto Console::is_paired() const -> bool { return core_->is_paired(); }

auto Console::is_paired_with(const users::PsUser& user) const -> bool {
  return core_->holds(user.account_id());
}

auto Console::paired_accounts() const -> std::vector<AccountId> {
  return core_->paired_accounts();
}

auto Console::starting() const noexcept -> Starting* {
  return core_->home().starting();
}

auto Console::check(const platform::StopToken& stop, const HomeNetwork& over)
    -> Expected<Presence, Unreachable> {
  return core_->home().find().check(*this, stop, over);
}

auto Console::wake(const users::PsUser& as, const HomeNetwork& over)
    -> Expected<void, WakeRefusal> {
  return core_->home().wake().now(*this, as, over);
}

auto Console::wake_and_wait(const users::PsUser& as,
                            const platform::StopToken& stop,
                            const HomeNetwork& over)
    -> Expected<void, WakeRefusal> {
  return core_->home().wake().and_wait(*this, as, stop, over);
}

auto Console::titles(users::PsUser& of, const platform::StopToken& stop)
    -> Expected<std::vector<InstalledTitle>, TitlesUnavailable> {
  if (core_->is_forgotten())
    return unexpected(TitlesUnavailable{TitlesUnavailable::Reason::forgotten});
  auto shelf = core_->home().library().shelf_of(*this, of, stop);
  if (!shelf) return unexpected(shelf.error());
  return std::move(shelf.value().titles);
}

auto Console::start(const InstalledTitle& title, users::PsUser& as,
                    StartWatcher* watcher, const platform::StopToken& stop)
    -> Expected<StartState, StartFailure> {
  if (core_->is_forgotten())
    return unexpected(StartFailure{StartFailure::Reason::forgotten});
  auto& home = core_->home();
  const auto shelf = home.library().shelf_of(*this, as, stop);
  if (!shelf) return unexpected(TitleStart::not_started(shelf.error()));
  TitleStart::Pause pause{home.threading(), home.clock()};
  return TitleStart{home.http(), pause, home.clock(), home.log()}.run(
      as, shelf.value().duid, title, watcher, stop);
}

auto Console::rename(std::string name) -> Expected<void, Unchanged> {
  return core_->rename(std::move(name));
}

auto Console::moved_to(Host host) -> Expected<void, Unchanged> {
  return core_->moved_to(std::move(host));
}

}  // namespace libchiaking::consoles
