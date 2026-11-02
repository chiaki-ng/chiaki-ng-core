#include "src/core/play/adapters/links/cloud/cloud_messaging.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace play_test {
namespace {

namespace play = libchiaking::play;
using play::CloudCaptureUpload;

class CloudCaptures : public ::testing::Test {
 protected:
  static void put_word(std::vector<std::byte>& body, std::size_t at,
                       std::uint32_t value) {
    for (std::size_t shift = 0; shift < 4; ++shift)
      body[at + shift] = static_cast<std::byte>((value >> (shift * 8)) & 0xffU);
  }

  static auto capture_report(std::uint32_t kind, std::string_view json)
      -> std::vector<std::byte> {
    std::vector<std::byte> body(1048);
    put_word(body, 8, 0x138);
    put_word(body, 12, kind);
    put_word(body, 16, static_cast<std::uint32_t>(json.size()));
    for (std::size_t index = 0; index < json.size(); ++index)
      body[20 + index] = static_cast<std::byte>(json[index]);
    return body;
  }

  static auto thumbnail_chunk(std::uint32_t index, std::uint32_t chunks,
                              const std::vector<std::byte>& data)
      -> std::vector<std::byte> {
    const std::string_view json =
        R"({"fileName":"cmg_thumb_1.jpg","createdDate":"2026-10-18T10:31:08Z","localUgcId":"local1","mimeType":"image\/jpeg","duration":0,"captureOperationType":1})";
    std::vector<std::byte> body(2336);
    put_word(body, 8, 0x138);
    put_word(body, 12, play::CloudCaptures::thumbnail_kind);
    put_word(body, 16, chunks);
    put_word(body, 20, index);
    for (std::size_t at = 0; at < json.size(); ++at)
      body[24 + at] = static_cast<std::byte>(json[at]);
    put_word(body, play::CloudCaptures::chunk_length_at,
             static_cast<std::uint32_t>(data.size()));
    std::ranges::copy(data, body.begin() + play::CloudCaptures::chunk_data_at);
    return body;
  }

  static auto event(std::string_view json) -> std::vector<std::byte> {
    return capture_report(play::CloudCaptures::event_kind, json);
  }

  const std::vector<std::byte> first{std::byte{0xff}, std::byte{0xd8},
                                     std::byte{1}};
  const std::vector<std::byte> second{std::byte{2}, std::byte{0xff},
                                      std::byte{0xd9}};
  play::CloudCaptures captures;
};

TEST_F(CloudCaptures, AssembleAThumbnailFromItsChunks) {
  EXPECT_FALSE(
      captures
          .receive(event(
              R"({"eventType":2,"type":"saveUgcId","localUgcId":"local1"})"))
          .thumbnail);
  EXPECT_FALSE(captures.receive(thumbnail_chunk(2, 2, second)).thumbnail);
  const auto whole = captures.receive(thumbnail_chunk(1, 2, first)).thumbnail;
  ASSERT_TRUE(whole);
  EXPECT_EQ(whole->id, "local1");
  EXPECT_FALSE(whole->video);
  EXPECT_EQ(whole->created, "2026-10-18T10:31:08Z");
  EXPECT_EQ(
      whole->thumbnail,
      (std::vector<std::byte>{std::byte{0xff}, std::byte{0xd8}, std::byte{1},
                              std::byte{2}, std::byte{0xff}, std::byte{0xd9}}));
}

TEST_F(CloudCaptures, ReportAFinishedUploadOnce) {
  static_cast<void>(captures.receive(
      event(R"({"eventType":2,"type":"saveUgcId","localUgcId":"local1"})")));
  static_cast<void>(captures.receive(thumbnail_chunk(2, 2, second)));
  static_cast<void>(captures.receive(thumbnail_chunk(1, 2, first)));
  EXPECT_FALSE(
      captures
          .receive(capture_report(
              play::CloudCaptures::upload_kind,
              R"({"contentExt":".jpg","contentSize":132722,"contentType":1,"createdDate":"2026-10-18T10:31:08Z","localUgcId":"local1","uploadUgcId":"psn1"})"))
          .uploaded);
  const auto done = captures.receive(
      event(R"({"eventType":2,"type":"complete","localUgcId":"local1"})"));
  ASSERT_TRUE(done.uploaded);
  EXPECT_EQ(*done.uploaded,
            (CloudCaptureUpload{"local1", "psn1", ".jpg", 132722, false,
                                "2026-10-18T10:31:08Z"}));
  EXPECT_FALSE(
      captures
          .receive(event(
              R"({"eventType":2,"type":"complete","localUgcId":"local1"})"))
          .uploaded);
}

TEST_F(CloudCaptures, IgnoreAStrayChunkAndAShortReport) {
  EXPECT_FALSE(captures.receive(thumbnail_chunk(3, 2, first)).thumbnail);
  EXPECT_FALSE(captures.receive(std::vector<std::byte>(10)).thumbnail);
}

}  // namespace
}  // namespace play_test
