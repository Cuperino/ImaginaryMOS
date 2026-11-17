// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// File-backed IMessageStore (plan §6): durability across restarts, torn-tail
// truncate-and-continue recovery, CRC verification, compaction, payload
// compression, and the persisted inbound dedupe window — the M1 "store
// crash-recovery tests" exit gate.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <imos/store/file_store.h>

namespace fs = std::filesystem;
using imos::Channel;
using imos::OutboundRecord;
using imos::PendingCursor;
using imos::PendingRecord;
using imos::Seq;
using imos::StreamHandle;
using imos::Timestamp;
using imos::store::FileMessageStore;
using imos::store::FileStoreOptions;
using imos::store::SyncPolicy;

namespace {

class FileStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::path{::testing::TempDir()} /
                ("imos-store-" +
                 std::string{::testing::UnitTest::GetInstance()->current_test_info()->name()});
        fs::remove_all(root_);
        fs::create_directories(root_);
    }
    void TearDown() override { fs::remove_all(root_); }

    FileStoreOptions options() {
        FileStoreOptions opts;
        opts.root = root_;
        opts.syncPolicy = SyncPolicy::EachAppend;
        return opts;
    }

    static OutboundRecord record(std::string_view messageId, const std::string& payload) {
        OutboundRecord r;
        r.messageId = messageId;
        r.payload = std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()};
        r.enqueuedAt = Timestamp{1'750'000'000'000};
        return r;
    }

    static std::vector<PendingRecord> drain(FileMessageStore& store, StreamHandle handle) {
        auto cursor = store.pending(handle);
        EXPECT_TRUE(cursor.ok());
        std::vector<PendingRecord> out;
        while (auto next = cursor.value().next()) {
            out.push_back(std::move(*next));
        }
        return out;
    }

    fs::path journalPath() const {
        for (const auto& dir : fs::directory_iterator(root_)) {
            const fs::path journal = dir.path() / "mom.journal";
            if (fs::exists(journal)) {
                return journal;
            }
        }
        return {};
    }

    fs::path root_;
};

std::string payloadText(const PendingRecord& record) {
    return std::string{reinterpret_cast<const char*>(record.payload.data()), record.payload.size()};
}

}  // namespace

TEST_F(FileStoreTest, AppendPendingAckSurviveRestart) {
    Seq firstSeq = 0;
    {
        FileMessageStore store{options()};
        auto handle = store.open("peer.a|ncs.b", Channel::Mom);
        ASSERT_TRUE(handle.ok());
        auto s1 = store.append(handle.value(), record("10", "<mos>one</mos>"));
        auto s2 = store.append(handle.value(), record("11", "<mos>two</mos>"));
        auto s3 = store.append(handle.value(), record("12", "<mos>three</mos>"));
        ASSERT_TRUE(s1.ok() && s2.ok() && s3.ok());
        firstSeq = s1.value();
        ASSERT_TRUE(store.markAcked(handle.value(), s2.value()).ok());
    }
    // Restart: pending = {one, three}, oldest first; sequences keep counting.
    FileMessageStore store{options()};
    auto handle = store.open("peer.a|ncs.b", Channel::Mom);
    ASSERT_TRUE(handle.ok());
    EXPECT_EQ(store.recoveryStats(handle.value()).truncatedBytes, 0U);
    const auto pending = drain(store, handle.value());
    ASSERT_EQ(pending.size(), 2U);
    EXPECT_EQ(pending[0].seq, firstSeq);
    EXPECT_EQ(pending[0].messageId, "10");
    EXPECT_EQ(payloadText(pending[0]), "<mos>one</mos>");
    EXPECT_EQ(pending[1].messageId, "12");

    auto s4 = store.append(handle.value(), record("13", "<mos>four</mos>"));
    ASSERT_TRUE(s4.ok());
    EXPECT_GT(s4.value(), firstSeq + 2);
}

// Whether a record is a reply (kOutboundReplyFlag) survives a restart, so a
// resuming session can tell its requests from the last session's answers.
TEST_F(FileStoreTest, TheReplyFlagSurvivesARestart) {
    {
        FileMessageStore store{options()};
        auto handle = store.open("peer", Channel::Ro);
        ASSERT_TRUE(handle.ok());
        // The records point into these, which must outlive them.
        const std::string requestPayload = "<mos>request</mos>";
        const std::string replyPayload = "<mos>reply</mos>";
        OutboundRecord request = record("20", requestPayload);
        OutboundRecord reply = record("21", replyPayload);
        reply.flags = imos::kOutboundReplyFlag;
        ASSERT_TRUE(store.append(handle.value(), request).ok());
        ASSERT_TRUE(store.append(handle.value(), reply).ok());
    }
    FileMessageStore store{options()};
    auto handle = store.open("peer", Channel::Ro);
    ASSERT_TRUE(handle.ok());
    const auto pending = drain(store, handle.value());
    ASSERT_EQ(pending.size(), 2U);
    EXPECT_EQ(pending[0].flags, 0U);
    EXPECT_EQ(pending[1].flags, imos::kOutboundReplyFlag);
}

TEST_F(FileStoreTest, TornFinalRecordIsTruncatedAndEarlierRecordsSurvive) {
    {
        FileMessageStore store{options()};
        auto handle = store.open("peer", Channel::Mom);
        ASSERT_TRUE(handle.ok());
        ASSERT_TRUE(store.append(handle.value(), record("1", "<mos>alpha</mos>")).ok());
        ASSERT_TRUE(store.append(handle.value(), record("2", "<mos>beta</mos>")).ok());
    }
    // Crash mid-write: chop bytes off the last record.
    const fs::path journal = journalPath();
    ASSERT_FALSE(journal.empty());
    const auto size = fs::file_size(journal);
    fs::resize_file(journal, size - 7);

    FileMessageStore store{options()};
    auto handle = store.open("peer", Channel::Mom);
    ASSERT_TRUE(handle.ok());
    EXPECT_GT(store.recoveryStats(handle.value()).truncatedBytes, 0U);
    const auto pending = drain(store, handle.value());
    ASSERT_EQ(pending.size(), 1U);
    EXPECT_EQ(pending[0].messageId, "1");

    // The journal keeps working after truncate-and-continue.
    ASSERT_TRUE(store.append(handle.value(), record("3", "<mos>gamma</mos>")).ok());
    const auto after = drain(store, handle.value());
    EXPECT_EQ(after.size(), 2U);
}

TEST_F(FileStoreTest, CrcCorruptionStopsReplayAtTheDamage) {
    {
        FileMessageStore store{options()};
        auto handle = store.open("peer", Channel::Mom);
        ASSERT_TRUE(handle.ok());
        ASSERT_TRUE(store.append(handle.value(), record("1", "<mos>alpha</mos>")).ok());
        ASSERT_TRUE(store.append(handle.value(), record("2", "<mos>beta</mos>")).ok());
    }
    const fs::path journal = journalPath();
    const auto size = static_cast<std::size_t>(fs::file_size(journal));
    {
        // Flip one payload byte inside the SECOND record.
        std::fstream file{journal, std::ios::in | std::ios::out | std::ios::binary};
        file.seekp(static_cast<std::streamoff>(size - 12));
        char byte = 0;
        file.seekg(static_cast<std::streamoff>(size - 12));
        file.read(&byte, 1);
        byte = static_cast<char>(byte ^ 0x5A);
        file.seekp(static_cast<std::streamoff>(size - 12));
        file.write(&byte, 1);
    }
    FileMessageStore store{options()};
    auto handle = store.open("peer", Channel::Mom);
    ASSERT_TRUE(handle.ok());
    EXPECT_GT(store.recoveryStats(handle.value()).truncatedBytes, 0U);
    const auto pending = drain(store, handle.value());
    ASSERT_EQ(pending.size(), 1U);
    EXPECT_EQ(pending[0].messageId, "1");
}

TEST_F(FileStoreTest, LargePayloadsCompressAndRoundTrip) {
    std::string big = "<mos><roStorySend><storyBody>";
    for (int i = 0; i < 200; ++i) {
        big += "<p>The quick brown fox jumps over the lazy dog again and again.</p>";
    }
    big += "</storyBody></roStorySend></mos>";

    {
        FileMessageStore store{options()};
        auto handle = store.open("peer", Channel::Ro);
        ASSERT_TRUE(handle.ok());
        ASSERT_TRUE(store.append(handle.value(), record("big", big)).ok());
    }
    fs::path journal;
    for (const auto& dir : fs::directory_iterator(root_)) {
        if (fs::exists(dir.path() / "ro.journal")) {
            journal = dir.path() / "ro.journal";
        }
    }
    ASSERT_FALSE(journal.empty());
    EXPECT_LT(fs::file_size(journal), big.size() / 2) << "payload did not compress";

    FileMessageStore store{options()};
    auto handle = store.open("peer", Channel::Ro);
    ASSERT_TRUE(handle.ok());
    const auto pending = drain(store, handle.value());
    ASSERT_EQ(pending.size(), 1U);
    EXPECT_EQ(payloadText(pending[0]), big);
}

TEST_F(FileStoreTest, CompactionShrinksTheJournalAndPreservesPending) {
    FileStoreOptions opts = options();
    opts.compactAfterDeadRecords = 1000;  // manual compaction only
    Seq keepSeq = 0;
    {
        FileMessageStore store{opts};
        auto handle = store.open("peer", Channel::Mom);
        ASSERT_TRUE(handle.ok());
        for (int i = 0; i < 50; ++i) {
            auto seq =
                store.append(handle.value(), record(std::to_string(i), "<mos>disposable</mos>"));
            ASSERT_TRUE(seq.ok());
            ASSERT_TRUE(store.markAcked(handle.value(), seq.value()).ok());
        }
        auto kept = store.append(handle.value(), record("keeper", "<mos>keep</mos>"));
        ASSERT_TRUE(kept.ok());
        keepSeq = kept.value();

        const auto before = fs::file_size(journalPath());
        ASSERT_TRUE(store.compact(handle.value()).ok());
        EXPECT_LT(fs::file_size(journalPath()), before / 4);
    }
    FileMessageStore store{opts};
    auto handle = store.open("peer", Channel::Mom);
    ASSERT_TRUE(handle.ok());
    const auto pending = drain(store, handle.value());
    ASSERT_EQ(pending.size(), 1U);
    EXPECT_EQ(pending[0].seq, keepSeq);
    EXPECT_EQ(pending[0].messageId, "keeper");
}

TEST_F(FileStoreTest, AutoCompactionTriggersOnDeadRecords) {
    FileStoreOptions opts = options();
    opts.compactAfterDeadRecords = 8;
    FileMessageStore store{opts};
    auto handle = store.open("peer", Channel::Mom);
    ASSERT_TRUE(handle.ok());
    for (int i = 0; i < 8; ++i) {
        auto seq = store.append(handle.value(), record(std::to_string(i), "<mos>x</mos>"));
        ASSERT_TRUE(seq.ok());
        ASSERT_TRUE(store.markAcked(handle.value(), seq.value()).ok());
    }
    // The eighth ack crosses the dead-record threshold; auto-compaction
    // rewrites the journal, which holds nothing (everything acked).
    EXPECT_EQ(fs::file_size(journalPath()), 0U);
    EXPECT_TRUE(drain(store, handle.value()).empty());
}

TEST_F(FileStoreTest, InboundDedupeWindowPersistsAndEvicts) {
    FileStoreOptions opts = options();
    opts.dedupeWindow = 3;
    {
        FileMessageStore store{opts};
        ASSERT_TRUE(store.noteInbound("peer", "A", Timestamp{1}).ok());
        ASSERT_TRUE(store.noteInbound("peer", "B", Timestamp{2}).ok());
        auto seenA = store.seenInbound("peer", "A");
        ASSERT_TRUE(seenA.ok());
        EXPECT_TRUE(seenA.value());
    }
    FileMessageStore store{opts};
    EXPECT_TRUE(store.seenInbound("peer", "A").value());
    EXPECT_TRUE(store.seenInbound("peer", "B").value());
    EXPECT_FALSE(store.seenInbound("peer", "Z").value());

    // Window of 3: C, D push A out.
    ASSERT_TRUE(store.noteInbound("peer", "C", Timestamp{3}).ok());
    ASSERT_TRUE(store.noteInbound("peer", "D", Timestamp{4}).ok());
    EXPECT_FALSE(store.seenInbound("peer", "A").value());
    EXPECT_TRUE(store.seenInbound("peer", "D").value());
}

TEST_F(FileStoreTest, EmptyAndMissingJournalsRecoverCleanly) {
    FileMessageStore store{options()};
    auto handle = store.open("brand.new.peer", Channel::Aux);
    ASSERT_TRUE(handle.ok());
    EXPECT_TRUE(drain(store, handle.value()).empty());
    EXPECT_EQ(store.recoveryStats(handle.value()).truncatedBytes, 0U);
}
