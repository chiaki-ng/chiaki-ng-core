#include "src/core/catalog/adapters/cache/cache.hpp"
#include "src/core/catalog/adapters/cache/refusal_memory.hpp"
#include "src/core/catalog/adapters/cache/refusal_record.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/rule_cases.hpp"

#include <libchiaking/detail/catalog/library.hpp>
#include <libchiaking/detail/catalog/my_games.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/testing/in_memory_record_store.hpp>
#include <libchiaking/testing/manual_clock.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace catalog_test {
namespace {

namespace platform = libchiaking::platform;
using libchiaking::AccountId;
using libchiaking::catalog::ProductId;
using libchiaking::catalog::RefusalMemory;
using libchiaking::catalog::StreamRefusal;
using libchiaking::catalog::Unanswered;
using libchiaking::users::Tier;
using Circumstances = StreamRefusal::Circumstances;
using Kind = StreamRefusal::Kind;
using Holding = StreamRefusal::Holding;

class ReadRefusingStore final : public platform::RecordStore {
 public:
  auto read(const platform::RecordName& name)
      -> libchiaking::Expected<platform::StoredRecord,
                               platform::StorageError> override {
    if (unavailable)
      return libchiaking::unexpected(platform::StorageError::read_failed);
    return inner.read(name);
  }
  auto write_atomic(const platform::RecordName& name,
                    std::span<const std::byte> data)
      -> libchiaking::Expected<void, platform::StorageError> override {
    return inner.write_atomic(name, data);
  }
  auto erase(const platform::RecordName& name)
      -> libchiaking::Expected<void, platform::StorageError> override {
    return inner.erase(name);
  }
  auto erase_prefix(const platform::RecordName& prefix)
      -> libchiaking::Expected<void, platform::StorageError> override {
    return inner.erase_prefix(prefix);
  }
  auto list(const platform::RecordName& prefix)
      -> libchiaking::Expected<std::vector<platform::RecordListing>,
                               platform::StorageError> override {
    return inner.list(prefix);
  }
  auto erase_legacy_file(std::string_view file_name)
      -> libchiaking::Expected<void, platform::StorageError> override {
    return inner.erase_legacy_file(file_name);
  }

  platform::DefaultThreadingProvider threading;
  libchiaking::testing::InMemoryRecordStore inner{threading};
  bool unavailable{};
};

class RefusalMemoryCases : public ::testing::Test, protected RuleCases {
 protected:
  static constexpr std::string_view rivet =
      "EP0109-PPSA07028_00-FAKE0110TITLE000";
  static constexpr std::string_view show =
      "UP0114-PPSA37004_00-FAKE0071TITLE000";
  static constexpr AccountId first_account{1111111111111111111ULL};
  static constexpr AccountId second_account{2222222222222222222ULL};

  RefusalMemoryCases() { clock.now_us = 1'800'000'000ULL * 1'000'000ULL; }

  auto now() const -> Time { return cache.now(); }
  auto premium() const -> Circumstances { return {Tier::premium, holding}; }

  auto refuses(std::string_view id, Kind kind) -> bool {
    return memory.remember(first_account, product(id), kind, premium())
        .has_value();
  }

  platform::DefaultThreadingProvider threading;
  libchiaking::testing::InMemoryRecordStore store{threading};
  libchiaking::testing::ManualClock clock;
  libchiaking::catalog::cache::Cache cache{store, clock};
  Holding holding{Holding::not_held()};
  RefusalMemory memory{cache, threading};
};

class RefusalMemoryRecall : public RefusalMemoryCases {};
class RefusalMemoryRevision : public RefusalMemoryCases {};
class RefusalMemoryLifetime : public RefusalMemoryCases {};
class RefusalMemoryCircumstances : public RefusalMemoryCases {};
class RefusalMemoryAccounts : public RefusalMemoryCases {};
class RefusalMemoryStorage : public RefusalMemoryCases {};

TEST_F(RefusalMemoryRecall, HoldsNothingAtFirst) {
  const auto none = memory.standing(first_account, Tier::premium);
  ASSERT_TRUE(none);
  EXPECT_TRUE(none.value().refusals.empty());
  EXPECT_EQ(memory.revision(first_account), none.value().revision);
}

TEST_F(RefusalMemoryRecall, RemembersARefusalForItsUserAndTitle) {
  const auto quiet =
      memory.standing(first_account, Tier::premium).value().revision;
  const auto remembered = memory.remember(
      first_account, product(rivet), Kind::no_game_for_entitlement, premium());
  ASSERT_TRUE(remembered);
  EXPECT_EQ(remembered.value().account(), first_account);
  EXPECT_EQ(remembered.value().product_id(), product(rivet));
  EXPECT_EQ(remembered.value().kind(), Kind::no_game_for_entitlement);
  EXPECT_TRUE(remembered.value().questions_ownership());
  EXPECT_EQ(remembered.value().refused_at(), now());
  EXPECT_EQ(remembered.value().circumstances(), premium());
  const auto one = memory.refusal_of(first_account, product(rivet), premium());
  ASSERT_TRUE(one);
  ASSERT_TRUE(one.value());
  EXPECT_EQ(*one.value(), remembered.value());
  const auto other_title =
      memory.refusal_of(first_account, product(show), premium());
  ASSERT_TRUE(other_title);
  EXPECT_FALSE(other_title.value());
  const auto standing = memory.standing(first_account, Tier::premium);
  ASSERT_TRUE(standing);
  ASSERT_EQ(standing.value().refusals.size(), 1U);
  EXPECT_EQ(standing.value().refusals[0], remembered.value());
  EXPECT_NE(standing.value().revision, quiet);
  EXPECT_EQ(memory.revision(first_account), standing.value().revision);
}

TEST_F(RefusalMemoryRecall, KeepsARefusalAcrossARestart) {
  const auto remembered = memory.remember(
      first_account, product(rivet), Kind::no_game_for_entitlement, premium());
  ASSERT_TRUE(remembered);
  RefusalMemory reopened{cache, threading};
  const auto kept = reopened.standing(first_account, Tier::premium);
  ASSERT_TRUE(kept);
  ASSERT_EQ(kept.value().refusals.size(), 1U);
  EXPECT_EQ(kept.value().refusals[0], remembered.value());
}

TEST_F(RefusalMemoryRecall, ReplacesAnOlderRefusalForTheSameTitle) {
  EXPECT_TRUE(refuses(rivet, Kind::refused_by_stream_server));
  clock.advance(std::chrono::hours{23});
  const auto again = memory.remember(first_account, product(rivet),
                                     Kind::no_game_for_entitlement, premium());
  ASSERT_TRUE(again);
  clock.advance(std::chrono::hours{2});
  const auto standing = memory.standing(first_account, Tier::premium);
  ASSERT_TRUE(standing);
  ASSERT_EQ(standing.value().refusals.size(), 1U);
  EXPECT_EQ(standing.value().refusals[0], again.value());
}

TEST_F(RefusalMemoryRevision, StaysPutWhileNothingChanges) {
  const auto start =
      memory.standing(first_account, Tier::premium).value().revision;
  EXPECT_EQ(memory.standing(first_account, Tier::premium).value().revision,
            start);
  EXPECT_TRUE(refuses(rivet, Kind::refused_by_stream_server));
  const auto after_first = memory.revision(first_account);
  EXPECT_EQ(memory.standing(first_account, Tier::premium).value().revision,
            after_first);
  EXPECT_TRUE(
      memory.refusal_of(first_account, product(rivet), premium()).value());
  EXPECT_EQ(memory.revision(first_account), after_first);
}

TEST_F(RefusalMemoryRevision, ChangesWithEveryChange) {
  const auto start =
      memory.standing(first_account, Tier::premium).value().revision;
  EXPECT_TRUE(refuses(rivet, Kind::refused_by_stream_server));
  const auto after_first = memory.revision(first_account);
  EXPECT_NE(after_first, start);
  EXPECT_TRUE(refuses(show, Kind::refused_by_stream_server));
  const auto after_second = memory.revision(first_account);
  EXPECT_NE(after_second, after_first);
  EXPECT_NE(after_second, start);

  clock.advance(std::chrono::hours{24});
  const auto expired = memory.standing(first_account, Tier::premium);
  ASSERT_TRUE(expired);
  EXPECT_TRUE(expired.value().refusals.empty());
  EXPECT_NE(expired.value().revision, after_second);
  EXPECT_NE(expired.value().revision, after_first);
  EXPECT_EQ(memory.standing(first_account, Tier::premium).value().revision,
            expired.value().revision);

  libchiaking::catalog::MyGames::Inputs before{};
  before.refusals_revision = after_second;
  libchiaking::catalog::MyGames::Inputs after{};
  after.refusals_revision = expired.value().revision;
  EXPECT_NE(before, after);

  EXPECT_TRUE(memory.forget(first_account));
  EXPECT_TRUE(refuses(show, Kind::refused_by_stream_server));
  const auto after_return = memory.revision(first_account);
  EXPECT_NE(after_return, expired.value().revision);
  EXPECT_NE(after_return, after_second);
  EXPECT_NE(after_return, start);
}

TEST_F(RefusalMemoryLifetime, IsADay) {
  const auto remembered = memory.remember(
      first_account, product(rivet), Kind::refused_by_stream_server, premium());
  ASSERT_TRUE(remembered);
  EXPECT_EQ(remembered.value().expires_at(), now() + std::chrono::hours{24});
  clock.advance(std::chrono::hours{24} - std::chrono::milliseconds{1});
  EXPECT_TRUE(refuses(show, Kind::refused_by_stream_server));
  auto standing = memory.standing(first_account, Tier::premium);
  ASSERT_TRUE(standing);
  EXPECT_EQ(standing.value().refusals.size(), 2U);

  clock.advance(std::chrono::milliseconds{1});
  standing = memory.standing(first_account, Tier::premium);
  ASSERT_TRUE(standing);
  ASSERT_EQ(standing.value().refusals.size(), 1U);
  EXPECT_EQ(standing.value().refusals[0].product_id(), product(show));
  const auto gone = memory.refusal_of(first_account, product(rivet), premium());
  ASSERT_TRUE(gone);
  EXPECT_FALSE(gone.value());

  RefusalMemory reopened{cache, threading};
  const auto saved = reopened.standing(first_account, Tier::premium);
  ASSERT_TRUE(saved);
  ASSERT_EQ(saved.value().refusals.size(), 1U);
  EXPECT_EQ(saved.value().refusals[0].product_id(), product(show));

  clock.advance(std::chrono::hours{24} - std::chrono::milliseconds{1});
  standing = memory.standing(first_account, Tier::premium);
  ASSERT_TRUE(standing);
  EXPECT_TRUE(standing.value().refusals.empty());
  EXPECT_TRUE(store.names().empty());
}

TEST_F(RefusalMemoryLifetime, EndsWhenTheClockGoesBackwards) {
  EXPECT_TRUE(refuses(rivet, Kind::refused_by_stream_server));
  clock.now_us -= 1'000;
  const auto gone = memory.refusal_of(first_account, product(rivet), premium());
  ASSERT_TRUE(gone);
  EXPECT_FALSE(gone.value());
}

TEST_F(RefusalMemoryCircumstances, ClearARefusalEarlyWhenTheTierChanges) {
  EXPECT_TRUE(memory.remember(first_account, product(rivet),
                              Kind::refused_by_stream_server,
                              {Tier::extra, holding}));
  EXPECT_TRUE(memory.remember(first_account, product(show),
                              Kind::refused_by_stream_server,
                              {Tier::premium, holding}));
  const auto before = memory.revision(first_account);
  clock.advance(std::chrono::minutes{1});

  auto as_extra =
      memory.refusal_of(first_account, product(rivet), {Tier::extra, holding});
  ASSERT_TRUE(as_extra);
  EXPECT_TRUE(as_extra.value());
  EXPECT_NE(memory.revision(first_account), before);
  const auto after_extra = memory.revision(first_account);
  const auto show_gone =
      memory.refusal_of(first_account, product(show), {Tier::extra, holding});
  ASSERT_TRUE(show_gone);
  EXPECT_FALSE(show_gone.value());

  const auto upgraded = memory.standing(first_account, Tier::premium);
  ASSERT_TRUE(upgraded);
  EXPECT_TRUE(upgraded.value().refusals.empty());
  EXPECT_NE(upgraded.value().revision, after_extra);
  as_extra =
      memory.refusal_of(first_account, product(rivet), {Tier::extra, holding});
  ASSERT_TRUE(as_extra);
  EXPECT_FALSE(as_extra.value());
  EXPECT_TRUE(store.names().empty());
}

TEST_F(RefusalMemoryCircumstances, ClearARefusalMadeAtAnUnknownTierOnceKnown) {
  EXPECT_TRUE(memory.remember(first_account, product(rivet),
                              Kind::refused_by_stream_server,
                              {std::nullopt, holding}));
  auto unknown =
      memory.refusal_of(first_account, product(rivet), {std::nullopt, holding});
  ASSERT_TRUE(unknown);
  EXPECT_TRUE(unknown.value());
  unknown =
      memory.refusal_of(first_account, product(rivet), {Tier::none, holding});
  ASSERT_TRUE(unknown);
  EXPECT_FALSE(unknown.value());
}

TEST_F(RefusalMemoryCircumstances, LeaveRefusalsStandingWhileTheTierIsUnknown) {
  EXPECT_TRUE(refuses(rivet, Kind::refused_by_stream_server));
  EXPECT_TRUE(memory.remember(first_account, product(show),
                              Kind::no_game_for_entitlement,
                              {Tier::extra, holding}));
  const auto before = memory.revision(first_account);
  clock.advance(std::chrono::minutes{1});

  const auto unknown =
      memory.standing_while_membership_is_unknown(first_account);
  ASSERT_TRUE(unknown);
  EXPECT_EQ(unknown.value().refusals.size(), 2U);
  EXPECT_EQ(unknown.value().revision, before);
  EXPECT_EQ(memory.revision(first_account), before);
  const auto known =
      memory.refusal_of(first_account, product(rivet), premium());
  ASSERT_TRUE(known);
  ASSERT_TRUE(known.value());
  EXPECT_EQ(known.value()->circumstances().tier, Tier::premium);

  EXPECT_TRUE(memory.remember(first_account, product(show),
                              Kind::no_game_for_entitlement,
                              {Tier::extra, holding}));
  const auto after_purchase = memory.forget_where_ownership_changed(
      first_account, [](const ProductId& refused) {
        return Holding::held_as(
            *libchiaking::catalog::OwnershipId::parse(refused.text()));
      });
  ASSERT_TRUE(after_purchase);
  EXPECT_NE(after_purchase.value(), before);
  const auto after_library =
      memory.standing_while_membership_is_unknown(first_account);
  ASSERT_TRUE(after_library);
  EXPECT_TRUE(after_library.value().refusals.empty());
  EXPECT_NE(after_library.value().revision, before);
  EXPECT_TRUE(store.names().empty());
}

TEST_F(RefusalMemoryCircumstances, StillExpireWhileTheTierIsUnknown) {
  EXPECT_TRUE(refuses(rivet, Kind::refused_by_stream_server));
  clock.advance(StreamRefusal::lifetime);
  const auto expired =
      memory.standing_while_membership_is_unknown(first_account);
  ASSERT_TRUE(expired);
  EXPECT_TRUE(expired.value().refusals.empty());
  const auto nobody =
      memory.standing_while_membership_is_unknown(second_account);
  ASSERT_TRUE(nobody);
  EXPECT_TRUE(nobody.value().refusals.empty());
}

TEST_F(RefusalMemoryCircumstances, SurviveARefetchedLibraryThatHoldsTheSame) {
  const auto held =
      Holding::held_as(*libchiaking::catalog::OwnershipId::parse(rivet));
  EXPECT_TRUE(memory.remember(first_account, product(rivet),
                              Kind::no_game_for_entitlement,
                              {Tier::premium, held}));
  const auto remembered = memory.revision(first_account);
  clock.advance(std::chrono::minutes{2});
  auto standing =
      memory.refusal_of(first_account, product(rivet), {Tier::premium, held});
  ASSERT_TRUE(standing);
  EXPECT_TRUE(standing.value());
  EXPECT_EQ(memory.revision(first_account), remembered);

  const auto refetched = memory.forget_where_ownership_changed(
      first_account, [&](const ProductId&) { return held; });
  ASSERT_TRUE(refetched);
  EXPECT_EQ(refetched.value(), remembered);
  const auto unchecked = memory.forget_where_ownership_changed(
      first_account, [](const ProductId&) { return Holding::unknown(); });
  ASSERT_TRUE(unchecked);
  EXPECT_EQ(unchecked.value(), remembered);
  standing = memory.refusal_of(first_account, product(rivet),
                               {Tier::premium, Holding::unknown()});
  ASSERT_TRUE(standing);
  ASSERT_TRUE(standing.value());
  EXPECT_EQ(standing.value()->circumstances().holding, held);
  RefusalMemory after_refetch{cache, threading};
  const auto kept = after_refetch.standing(first_account, Tier::premium);
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept.value().refusals.size(), 1U);
  EXPECT_EQ(store.names().size(), 1U);
}

TEST_F(RefusalMemoryCircumstances, ClearARefusalEarlyWhenOwnershipChanges) {
  const auto held =
      Holding::held_as(*libchiaking::catalog::OwnershipId::parse(rivet));
  const auto held_otherwise =
      Holding::held_as(*libchiaking::catalog::OwnershipId::parse(show));
  const Circumstances at_refusal{Tier::premium, held};
  EXPECT_TRUE(memory.remember(first_account, product(rivet),
                              Kind::no_game_for_entitlement, at_refusal));
  const auto remembered = memory.revision(first_account);
  clock.advance(std::chrono::minutes{2});
  auto standing = memory.refusal_of(first_account, product(rivet),
                                    {Tier::premium, Holding::not_held()});
  ASSERT_TRUE(standing);
  EXPECT_FALSE(standing.value());
  EXPECT_NE(memory.revision(first_account), remembered);
  RefusalMemory reopened{cache, threading};
  const auto saved = reopened.standing(first_account, Tier::premium);
  ASSERT_TRUE(saved);
  EXPECT_TRUE(saved.value().refusals.empty());
  EXPECT_TRUE(store.names().empty());

  EXPECT_TRUE(memory.remember(first_account, product(rivet),
                              Kind::no_game_for_entitlement, at_refusal));
  standing = memory.refusal_of(first_account, product(rivet),
                               {Tier::premium, held_otherwise});
  ASSERT_TRUE(standing);
  EXPECT_FALSE(standing.value());
}

TEST_F(RefusalMemoryCircumstances, ClearOnlyRefusalsForTitlesNowClaimed) {
  const auto held =
      Holding::held_as(*libchiaking::catalog::OwnershipId::parse(rivet));
  EXPECT_TRUE(memory.remember(first_account, product(rivet),
                              Kind::refused_by_stream_server,
                              {Tier::premium, Holding::not_held()}));
  EXPECT_TRUE(memory.remember(first_account, product(show),
                              Kind::refused_by_stream_server,
                              {Tier::premium, Holding::not_held()}));
  const auto both = memory.revision(first_account);
  const auto claimed = memory.forget_where_ownership_changed(
      first_account, [&](const ProductId& refused) {
        return refused == product(rivet) ? held : Holding::not_held();
      });
  ASSERT_TRUE(claimed);
  EXPECT_NE(claimed.value(), both);
  const auto left = memory.standing(first_account, Tier::premium);
  ASSERT_TRUE(left);
  ASSERT_EQ(left.value().refusals.size(), 1U);
  EXPECT_EQ(left.value().refusals[0].product_id(), product(show));
  const auto nobody = memory.forget_where_ownership_changed(
      second_account, [&](const ProductId&) { return held; });
  EXPECT_TRUE(nobody);
  EXPECT_TRUE(
      memory.standing(second_account, Tier::premium).value().refusals.empty());
}

TEST_F(RefusalMemoryAccounts, NeverShowARefusalToAnotherAccount) {
  EXPECT_TRUE(refuses(rivet, Kind::refused_by_stream_server));
  const auto theirs =
      memory.refusal_of(second_account, product(rivet), premium());
  ASSERT_TRUE(theirs);
  EXPECT_FALSE(theirs.value());
  auto all_theirs = memory.standing(second_account, Tier::premium);
  ASSERT_TRUE(all_theirs);
  EXPECT_TRUE(all_theirs.value().refusals.empty());

  EXPECT_TRUE(memory.remember(second_account, product(show),
                              Kind::no_game_for_entitlement, premium()));
  const auto mine = memory.standing(first_account, Tier::premium);
  all_theirs = memory.standing(second_account, Tier::premium);
  ASSERT_TRUE(mine);
  ASSERT_EQ(mine.value().refusals.size(), 1U);
  EXPECT_EQ(mine.value().refusals[0].product_id(), product(rivet));
  EXPECT_EQ(mine.value().refusals[0].account(), first_account);
  ASSERT_TRUE(all_theirs);
  ASSERT_EQ(all_theirs.value().refusals.size(), 1U);
  EXPECT_EQ(all_theirs.value().refusals[0].product_id(), product(show));
  EXPECT_EQ(all_theirs.value().refusals[0].account(), second_account);
  EXPECT_NE(mine.value().revision, all_theirs.value().revision);
}

TEST_F(RefusalMemoryAccounts, DropARecordCopiedUnderAnotherAccount) {
  EXPECT_TRUE(refuses(rivet, Kind::refused_by_stream_server));
  EXPECT_TRUE(memory.remember(second_account, product(show),
                              Kind::no_game_for_entitlement, premium()));
  const auto first_name =
      *platform::RecordName::parse("user/1111111111111111111/refusals");
  const auto second_name =
      *platform::RecordName::parse("user/2222222222222222222/refusals");
  const auto first_record = store.read(first_name);
  ASSERT_TRUE(first_record);
  EXPECT_TRUE(store.read(second_name));
  EXPECT_TRUE(store.write_atomic(second_name, first_record.value().data));
  RefusalMemory reopened{cache, threading};
  const auto theirs =
      reopened.refusal_of(second_account, product(rivet), premium());
  ASSERT_TRUE(theirs);
  EXPECT_FALSE(theirs.value());
  EXPECT_FALSE(store.read(second_name));
  EXPECT_TRUE(store.read(first_name));
  const auto still_mine =
      reopened.refusal_of(first_account, product(rivet), premium());
  ASSERT_TRUE(still_mine);
  ASSERT_TRUE(still_mine.value());
  EXPECT_EQ(still_mine.value()->account(), first_account);
}

TEST_F(RefusalMemoryAccounts, ForgetOneAccountAndLeaveTheOtherAlone) {
  EXPECT_TRUE(refuses(rivet, Kind::refused_by_stream_server));
  EXPECT_TRUE(memory.remember(second_account, product(rivet),
                              Kind::refused_by_stream_server, premium()));
  EXPECT_EQ(store.names().size(), 2U);
  EXPECT_TRUE(memory.forget(first_account));
  const auto mine = memory.standing(first_account, Tier::premium);
  const auto theirs = memory.standing(second_account, Tier::premium);
  ASSERT_TRUE(mine);
  EXPECT_TRUE(mine.value().refusals.empty());
  ASSERT_TRUE(theirs);
  EXPECT_EQ(theirs.value().refusals.size(), 1U);
  ASSERT_EQ(store.names().size(), 1U);
  EXPECT_EQ(store.names().front().text(), "user/2222222222222222222/refusals");
  EXPECT_TRUE(memory.forget(first_account));
  RefusalMemory reopened{cache, threading};
  const auto gone = reopened.standing(first_account, Tier::premium);
  ASSERT_TRUE(gone);
  EXPECT_TRUE(gone.value().refusals.empty());
}

TEST_F(RefusalMemoryAccounts, ForgetEveryoneForGoodBeforeALaterRefusal) {
  EXPECT_TRUE(refuses(rivet, Kind::refused_by_stream_server));
  EXPECT_TRUE(memory.remember(second_account, product(rivet),
                              Kind::refused_by_stream_server, premium()));
  EXPECT_EQ(store.names().size(), 2U);
  EXPECT_TRUE(memory.forget_everyone());
  EXPECT_TRUE(store.names().empty());

  const auto later = memory.remember(first_account, product(show),
                                     Kind::no_game_for_entitlement, premium());
  ASSERT_TRUE(later);
  const auto mine = memory.standing(first_account, Tier::premium);
  ASSERT_TRUE(mine);
  ASSERT_EQ(mine.value().refusals.size(), 1U);
  EXPECT_EQ(mine.value().refusals[0], later.value());
  const auto theirs = memory.standing(second_account, Tier::premium);
  ASSERT_TRUE(theirs);
  EXPECT_TRUE(theirs.value().refusals.empty());
  ASSERT_EQ(store.names().size(), 1U);
  EXPECT_EQ(store.names().front().text(), "user/1111111111111111111/refusals");

  RefusalMemory reopened{cache, threading};
  const auto kept = reopened.standing(first_account, Tier::premium);
  ASSERT_TRUE(kept);
  ASSERT_EQ(kept.value().refusals.size(), 1U);
  EXPECT_EQ(kept.value().refusals[0].product_id(), product(show));
}

TEST_F(RefusalMemoryStorage, LeavesAnUnreadableStoreUnansweredButRemembers) {
  ReadRefusingStore refusing;
  libchiaking::catalog::cache::Cache refusing_cache{refusing, clock};
  RefusalMemory remembering{refusing_cache, refusing.threading};
  const Circumstances any_premium{Tier::premium, {}};
  refusing.unavailable = true;
  const auto asked = remembering.standing(first_account, Tier::premium);
  ASSERT_FALSE(asked);
  EXPECT_EQ(asked.error().reason(), Unanswered::Reason::storage);
  const auto one =
      remembering.refusal_of(first_account, product(rivet), any_premium);
  ASSERT_FALSE(one);
  EXPECT_EQ(one.error().reason(), Unanswered::Reason::storage);
  const auto unsaved =
      remembering.remember(first_account, product(rivet),
                           Kind::refused_by_stream_server, any_premium);
  ASSERT_FALSE(unsaved);
  EXPECT_EQ(unsaved.error().reason(), Unanswered::Reason::storage);
  EXPECT_TRUE(refusing.inner.names().empty());

  refusing.unavailable = false;
  const auto standing = remembering.standing(first_account, Tier::premium);
  ASSERT_TRUE(standing);
  ASSERT_EQ(standing.value().refusals.size(), 1U);
  EXPECT_EQ(standing.value().refusals[0].product_id(), product(rivet));
  EXPECT_EQ(refusing.inner.names().size(), 1U);
  RefusalMemory reopened{refusing_cache, refusing.threading};
  const auto saved = reopened.standing(first_account, Tier::premium);
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved.value().refusals, standing.value().refusals);
}

TEST_F(RefusalMemoryStorage, HoldsARefusalItCouldNotWrite) {
  const Circumstances any_premium{Tier::premium, {}};
  EXPECT_TRUE(memory.standing(first_account, Tier::premium));
  store.fail_writes = true;
  const auto failed =
      memory.remember(first_account, product(show),
                      Kind::refused_by_stream_server, any_premium);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().reason(), Unanswered::Reason::storage);
  const auto held =
      memory.refusal_of(first_account, product(show), any_premium);
  ASSERT_TRUE(held);
  EXPECT_TRUE(held.value());
}

TEST_F(RefusalMemoryStorage, ForgetsACorruptedRecordRatherThanFailing) {
  const auto name =
      *platform::RecordName::parse("user/1111111111111111111/refusals");
  const std::string_view garbage =
      R"({"record":"refusals","schema":1,"writtenAtMs":"x")";
  EXPECT_TRUE(store.write_atomic(
      name, std::as_bytes(std::span{garbage.data(), garbage.size()})));
  RefusalMemory reopened{cache, threading};
  const auto standing = reopened.standing(first_account, Tier::premium);
  ASSERT_TRUE(standing);
  EXPECT_TRUE(standing.value().refusals.empty());
  EXPECT_TRUE(store.names().empty());
  EXPECT_TRUE(reopened.remember(first_account, product(rivet),
                                Kind::refused_by_stream_server, premium()));
  EXPECT_EQ(store.names().size(), 1U);
}

}  // namespace
}  // namespace catalog_test
