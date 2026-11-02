#include "src/core/consoles/adapters/search/search_datagram.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace consoles_test {
namespace {

using libchiaking::consoles::Firmware;
using libchiaking::consoles::Generation;
using libchiaking::consoles::RunningTitle;
using libchiaking::consoles::SearchDatagram;
using libchiaking::consoles::SearchReply;
using libchiaking::platform::IpAddress;
using Status = SearchReply::Status;

static_assert(SearchDatagram::id_length == 12);
static_assert(SearchDatagram::console_name_limit == 255 &&
              SearchDatagram::console_system_version_limit == 63);
static_assert(!std::is_default_constructible_v<SearchReply>);

class SearchReplyIdentity : public ::testing::Test {
 protected:
  static auto host(Status status, std::string id, std::string type,
                   std::string name = "Console") -> SearchDatagram::Fields {
    SearchDatagram::Fields value;
    value.status = status;
    value.address = IpAddress::ipv4({192, 0, 2, 20});
    value.request_port = 997;
    value.system_version = "08050001";
    value.protocol_version = type == "PS5" ? "00030010" : "00020020";
    value.id = std::move(id);
    value.type = std::move(type);
    value.name = std::move(name);
    return value;
  }

  static auto firmware_of(std::string_view type, std::string_view protocol,
                          std::string_view system) -> Firmware::Family {
    SearchDatagram::Fields fields;
    fields.protocol_version = protocol;
    fields.system_version = system;
    fields.type = type;
    fields.id = "AABBCCDDEEFF";
    return SearchDatagram::from(fields).value().firmware().family();
  }
};

TEST_F(SearchReplyIdentity, AdmitsAPs5InStandbyWithItsFacts) {
  const auto ps5 = SearchDatagram::from(
      host(Status::standby, "AABBCCDDEEFF", "PS5", "Living room"));
  ASSERT_TRUE(ps5);
  EXPECT_EQ(ps5->hardware_id().text(), "aabbccddeeff");
  ASSERT_TRUE(ps5->name());
  EXPECT_EQ(ps5->name()->text(), "Living room");
  EXPECT_EQ(ps5->generation(), Generation::ps5);
  EXPECT_TRUE(ps5->in_standby());
  EXPECT_EQ(ps5->request_port(), 997);
  EXPECT_EQ(ps5->status(), Status::standby);
  EXPECT_EQ(ps5->address(), IpAddress::ipv4({192, 0, 2, 20}));
  EXPECT_EQ(ps5->system_version(), "08050001");
  EXPECT_EQ(ps5->firmware(), Firmware{Firmware::Family::ps5_1});
}

TEST_F(SearchReplyIdentity, MakesASightingOfAConsoleInRestMode) {
  const auto ps5 = SearchDatagram::from(
      host(Status::standby, "AABBCCDDEEFF", "PS5", "Living room"));
  ASSERT_TRUE(ps5);
  const auto sighting = ps5->sighting();
  ASSERT_TRUE(sighting);
  EXPECT_TRUE(sighting->in_rest_mode);
  EXPECT_EQ(sighting->address.text(), "192.0.2.20");
  EXPECT_EQ(sighting->hardware_id, ps5->hardware_id());
  EXPECT_EQ(sighting->firmware, ps5->firmware());
  EXPECT_EQ(sighting->name, ps5->name());
  EXPECT_FALSE(sighting->running_title);
}

TEST_F(SearchReplyIdentity, AdmitsAReadyPs4WithoutAName) {
  const auto ps4 =
      SearchDatagram::from(host(Status::ready, "001122334455", "PS4", ""));
  ASSERT_TRUE(ps4);
  EXPECT_EQ(ps4->generation(), Generation::ps4);
  EXPECT_FALSE(ps4->in_standby());
  EXPECT_FALSE(ps4->name());
  EXPECT_EQ(ps4->status(), Status::ready);
  EXPECT_EQ(ps4->firmware(), Firmware{Firmware::Family::ps4_10});
}

TEST_F(SearchReplyIdentity, EqualsOnlyTheSameFacts) {
  const auto ps4 =
      SearchDatagram::from(host(Status::ready, "001122334455", "PS4", ""));
  const auto same =
      SearchDatagram::from(host(Status::ready, "001122334455", "PS4", ""));
  const auto named =
      SearchDatagram::from(host(Status::ready, "001122334455", "PS4", "PS4"));
  const auto ps5 = SearchDatagram::from(
      host(Status::standby, "AABBCCDDEEFF", "PS5", "Living room"));
  ASSERT_TRUE(ps4);
  ASSERT_TRUE(same);
  ASSERT_TRUE(named);
  ASSERT_TRUE(ps5);
  EXPECT_TRUE(*ps4 == *same);
  EXPECT_FALSE(*ps4 == *named);
  EXPECT_FALSE(*ps4 == *ps5);
}

TEST_F(SearchReplyIdentity, KeepsAnUnknownStatusOutOfRestMode) {
  const auto unknown =
      SearchDatagram::from(host(Status::unknown, "AABBCCDDEEFF", "PS5"));
  ASSERT_TRUE(unknown);
  EXPECT_EQ(unknown->status(), Status::unknown);
  EXPECT_FALSE(unknown->in_standby());
  ASSERT_TRUE(unknown->sighting());
  EXPECT_FALSE(unknown->sighting()->in_rest_mode);
  const auto ready =
      SearchDatagram::from(host(Status::ready, "AABBCCDDEEFF", "PS5"));
  ASSERT_TRUE(ready);
  EXPECT_FALSE(*unknown == *ready);
}

TEST_F(SearchReplyIdentity, RefusesAHardwareIdThatIsNotTwelveHexDigits) {
  for (const std::string_view id :
       {"AABBCCDDEEF", "AABBCCDDEEFFF", "AABBCCDDEEFG", "",
        "AA:BB:CC:DD:", "000000000000"})
    EXPECT_FALSE(
        SearchDatagram::from(host(Status::ready, std::string{id}, "PS5")))
        << id;
}

TEST_F(SearchReplyIdentity, RefusesAnUnknownConsoleType) {
  EXPECT_FALSE(
      SearchDatagram::from(host(Status::ready, "AABBCCDDEEFF", "PS3")));
  EXPECT_FALSE(
      SearchDatagram::from(host(Status::ready, "AABBCCDDEEFF", "ps5")));
}

TEST_F(SearchReplyIdentity, RefusesAnUnprintableOrOverlongName) {
  EXPECT_FALSE(SearchDatagram::from(
      host(Status::ready, "AABBCCDDEEFF", "PS5", "bell\a")));
  EXPECT_FALSE(SearchDatagram::from(
      host(Status::ready, "AABBCCDDEEFF", "PS5", std::string(256, 'n'))));
  EXPECT_TRUE(SearchDatagram::from(
      host(Status::ready, "AABBCCDDEEFF", "PS5", std::string(255, 'n'))));
}

TEST_F(SearchReplyIdentity, RefusesAnOverlongSystemVersion) {
  auto versioned = host(Status::ready, "AABBCCDDEEFF", "PS5");
  versioned.system_version = std::string(64, '1');
  EXPECT_FALSE(SearchDatagram::from(versioned));
}

TEST_F(SearchReplyIdentity, KeepsTheRunningTitle) {
  auto playing = host(Status::ready, "AABBCCDDEEFF", "PS5");
  playing.running_app_title_id = "PPSA01234_00";
  playing.running_app_name = "A Game";
  const auto busy = SearchDatagram::from(playing);
  ASSERT_TRUE(busy);
  EXPECT_EQ(busy->running_title(), (RunningTitle{"PPSA01234_00", "A Game"}));
}

TEST_F(SearchReplyIdentity, DropsARunningTitleWithAnUnprintableName) {
  auto playing = host(Status::ready, "AABBCCDDEEFF", "PS5");
  playing.running_app_title_id = "PPSA01234_00";
  playing.running_app_name = "bell\a";
  const auto busy = SearchDatagram::from(playing);
  ASSERT_TRUE(busy);
  EXPECT_FALSE(busy->running_title());
}

TEST_F(SearchReplyIdentity, NamesTheFirmwareFamilyFromTypeAndVersion) {
  EXPECT_EQ(firmware_of("PS5", "00030010", "08050001"),
            Firmware::Family::ps5_1);
  EXPECT_EQ(firmware_of("PS5", "00030010", "8050000"), Firmware::Family::ps5_1);
  EXPECT_EQ(firmware_of("PS5", "00020020", ""), Firmware::Family::ps5_1);
  EXPECT_EQ(firmware_of("PS4", "00020020", "12345678"),
            Firmware::Family::ps4_10);
  EXPECT_EQ(firmware_of("PS4", "00020020", "7000000"), Firmware::Family::ps4_9);
  EXPECT_EQ(firmware_of("PS4", "00020020", " 07000000 "),
            Firmware::Family::ps4_9);
  EXPECT_EQ(firmware_of("PS4", "00020020", " +7000000x"),
            Firmware::Family::ps4_10);
  EXPECT_EQ(firmware_of("PS4", "00020020", "1"), Firmware::Family::ps4_8);
  EXPECT_EQ(firmware_of("PS4", "00020020", "-5"), Firmware::Family::ps4_10);
  EXPECT_EQ(firmware_of("PS4", "00030010", "08050001"),
            Firmware::Family::ps4_10);
}

}  // namespace
}  // namespace consoles_test
