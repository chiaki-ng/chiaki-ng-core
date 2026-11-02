#include "libchiaking/play_types.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_case.hpp"
#include "tests/unit/core/play/adapters/session/session_engine_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace play_test {

using test_support::play::SessionRig;

using test_support::LiveProviders;
namespace {

using libchiaking::diagnostics::SessionJournal;
using libchiaking::platform::LogSeverity;

TEST(SessionJournal, LabelsItsOwnLinesSessionByDefault) {
  SessionJournal plain{LiveProviders::threading(), LiveProviders::steady(), 1};
  plain.write({LogSeverity::info, "hello"});
  const auto plain_entry = plain.read();
  ASSERT_TRUE(plain_entry);
  EXPECT_EQ(plain_entry->label(), "session");
  EXPECT_TRUE(plain_entry->line().starts_with("session attempt="));
}

TEST(SessionJournal, LabelsCoreAndSessionLinesAsConfigured) {
  libchiaking::diagnostics::LogQueue core{LiveProviders::steady()};
  SessionJournal labelled{
      LiveProviders::threading(), LiveProviders::steady(), 1,
      SessionJournal::Labels{.core = "libchiaki", .session = "bridge"}};
  labelled.write({LogSeverity::info, "hello"});
  labelled.follow(core);
  core.write({LogSeverity::info, "from the core"});
  const auto first = labelled.read();
  const auto second = labelled.read();
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  const auto& from_core =
      first->origin() == SessionJournal::Origin::core ? *first : *second;
  const auto& from_session =
      first->origin() == SessionJournal::Origin::core ? *second : *first;
  EXPECT_EQ(from_core.label(), "libchiaki");
  EXPECT_TRUE(from_core.line().starts_with("libchiaki attempt="));
  EXPECT_TRUE(from_core.line().ends_with(" from the core"));
  EXPECT_EQ(from_session.label(), "bridge");
  EXPECT_TRUE(from_session.line().starts_with("bridge attempt="));
  EXPECT_TRUE(from_session.line().ends_with(" hello"));
  EXPECT_EQ(SessionJournal::Labels{}.core, "core");
  EXPECT_EQ(SessionJournal::Labels{}.session, "session");
}

TEST(SessionJournal, KeepsBoundedRoomForLogsAndCaptures) {
  EXPECT_EQ(SessionJournal::capacity, 256U);
  EXPECT_EQ(SessionJournal::routine_capacity, 224U);
  EXPECT_EQ(SessionJournal::retained_logs, 257U);
  EXPECT_EQ(SessionJournal::retained_captures, 33U);
  EXPECT_EQ(SessionJournal::message_limit, 899U);
}

TEST(SessionJournal, OutlivesTheSessionAndItsPlatform) {
  std::shared_ptr<SessionJournal> journal;
  {
    libchiaking::diagnostics::LogQueue core{LiveProviders::steady()};
    libchiaking::diagnostics::ApiCapture capture;
    capture.enable(true);
    SessionEngineRig h;
    journal = h.journal;
    journal->follow(core, &capture);
    EXPECT_TRUE(journal->following());
    core.write({LogSeverity::warning, "core line"});
    const auto live = journal->read();
    ASSERT_TRUE(live);
    EXPECT_EQ(live->origin(), SessionJournal::Origin::core);
    EXPECT_EQ(live->message(), "core line");
    core.write({LogSeverity::info, "kept line"});
    libchiaking::platform::HttpRequest request;
    request.url = libchiaking::SecretString{"https://example.test/path"};
    capture.finish(
        1, capture.begin(request, 1),
        libchiaking::unexpected(libchiaking::platform::HttpError::disconnected),
        std::chrono::microseconds{1});
    const auto outcome = h.run();
    EXPECT_TRUE(outcome.succeeded());
    EXPECT_FALSE(journal->following());
    core.write({LogSeverity::info, "after the run"});
  }
  const auto first = journal->read();
  ASSERT_TRUE(first);
  EXPECT_EQ(first->origin(), SessionJournal::Origin::core);
  EXPECT_EQ(first->message(), "kept line");
  EXPECT_TRUE(first->line().starts_with("core attempt="));
  EXPECT_TRUE(first->line().ends_with(" kept line"));
  EXPECT_NE(first->line().find(" dropped=0 "), std::string::npos);
  bool stopped = false;
  std::uint64_t sequence = 0;
  while (const auto entry = journal->read()) {
    EXPECT_EQ(entry->origin(), SessionJournal::Origin::session);
    EXPECT_EQ(entry->sequence(), ++sequence);
    EXPECT_TRUE(entry->line().starts_with(
        "session attempt=" + std::to_string(entry->attempt()) + " sequence="));
    EXPECT_NE(entry->message(), "after the run");
    stopped = stopped || entry->message() == "session stop succeeded";
  }
  EXPECT_TRUE(stopped);
  EXPECT_GE(sequence, 4U);
  EXPECT_TRUE(journal->take_capture().has_value());
  while (journal->take_capture()) {
  }
  EXPECT_FALSE(journal->take_capture());
}

TEST(SessionJournal, DropsRoutineLinesBeforeUrgentOnesAndSaysSo) {
  SessionJournal bounded{LiveProviders::threading(), LiveProviders::steady(),
                         1};
  for (int index = 0; index < 300; ++index)
    bounded.write({LogSeverity::info, "routine"});
  for (int index = 0; index < 40; ++index)
    bounded.write(
        {LogSeverity::error, "urgent\nline\r" + std::string(1000, 'x')});
  std::size_t routine = 0;
  std::size_t urgent = 0;
  std::optional<SessionJournal::Entry> notice;
  while (auto entry = bounded.read()) {
    if (entry->message() == "routine")
      ++routine;
    else if (entry->severity() == LogSeverity::error)
      ++urgent;
    else
      notice = std::move(entry);
    if (entry && entry->severity() == LogSeverity::error) {
      EXPECT_EQ(entry->message().size(), 899U);
      EXPECT_TRUE(entry->message().starts_with("urgent line xxx"));
    }
  }
  EXPECT_EQ(routine, 224U);
  EXPECT_EQ(urgent, 32U);
  ASSERT_TRUE(notice);
  EXPECT_EQ(notice->severity(), LogSeverity::warning);
  EXPECT_EQ(notice->dropped(), 84U);
  EXPECT_TRUE(notice->message().starts_with(
      "the session journal dropped 84 diagnostic records"));
  EXPECT_FALSE(bounded.read());
  EXPECT_FALSE(bounded.take_capture());
}

using SessionJournalReading = SessionCase;

TEST_F(SessionJournalReading, ShowsWhatTheSessionLogged) {
  SessionRig rig;
  const auto journal = rig.session->journal();
  ASSERT_TRUE(journal);
  EXPECT_EQ(journal, rig.core->journal());
  EXPECT_FALSE(journal->following());
  rig.start_playing();
  rig.link.describe(description());
  SessionRig::eventually([&] { return rig.media.descriptions().size() == 1; });
  EXPECT_TRUE(rig.session->leave());
  EXPECT_TRUE(rig.session->wait_until_ended().was_left());
  std::vector<std::string> lines;
  while (const auto entry = journal->read()) {
    EXPECT_EQ(entry->origin(), SessionJournal::Origin::session);
    EXPECT_TRUE(entry->line().starts_with("session attempt="));
    lines.emplace_back(entry->message());
  }
  EXPECT_EQ(std::ranges::count(lines,
                               "stream negotiated: 1 video profiles "
                               "(1280x720); audio 2 ch 48000 Hz"),
            1);
  EXPECT_FALSE(journal->read());
  EXPECT_FALSE(journal->take_capture());
}

}  // namespace
}  // namespace play_test
