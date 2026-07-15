// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// imos_store_file — the default file-backed IMessageStore (plan §6). One
// directory per peer, one append-only journal per channel plus one for the
// inbound dedupe window. Records are length-prefixed with a fixed header and
// a zlib crc32 over header+payload; payloads above a threshold are
// deflate-compressed. Recovery tolerates a torn final record
// (truncate-and-continue) and verifies CRCs; journals are rewritten via
// write-temp-then-atomic-rename on compaction.
//
// This module is deliberately OUTSIDE the sans-I/O core: it owns real file
// descriptors. Platform specifics live in a tiny file abstraction
// (fdatasync/O_CLOEXEC on POSIX, FlushFileBuffers on Windows).

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <imos/session/message_store.h>
#include <imos/store_file_export.h>

namespace imos::store {

/// Durability policy per append (plan §6: embedded flash wear is real).
enum class SyncPolicy : std::uint8_t {
    EachAppend,  ///< fsync inside every append (safest)
    OnDemand,    ///< default: fsync when the session calls sync() before send
    Batched,     ///< fsync at most every batchWindow; sync() still forces
};

struct FileStoreOptions {
    std::filesystem::path root;
    SyncPolicy syncPolicy = SyncPolicy::OnDemand;
    std::int64_t batchWindowMillis = 50;
    std::size_t compressThresholdBytes = 512;
    std::size_t dedupeWindow = 256;
    /// Compact automatically once a journal carries this many dead records.
    std::size_t compactAfterDeadRecords = 256;
    /// Recovery refuses records claiming to be larger than this.
    std::size_t maxRecordBytes = 64U * 1024U * 1024U;
};

struct RecoveryStats {
    std::size_t liveRecords = 0;
    std::size_t truncatedBytes = 0;  ///< torn/corrupt tail dropped at open
};

class IMOS_STORE_FILE_API FileMessageStore final : public IMessageStore {
public:
    explicit FileMessageStore(FileStoreOptions options);
    ~FileMessageStore() override;

    FileMessageStore(const FileMessageStore&) = delete;
    FileMessageStore& operator=(const FileMessageStore&) = delete;

    Result<StreamHandle> open(const PeerId& peer, Channel channel) override;
    Result<Seq> append(StreamHandle stream, const OutboundRecord& record) override;
    Result<void> markAcked(StreamHandle stream, Seq seq) override;
    Result<PendingCursor> pending(StreamHandle stream) override;
    Result<void> noteInbound(const PeerId& peer, MessageIdView messageId,
                             Timestamp receivedAt) override;
    Result<bool> seenInbound(const PeerId& peer, MessageIdView messageId) override;
    Result<void> compact(StreamHandle stream) override;
    Result<void> sync(StreamHandle stream) override;

    /// Stats of the most recent open()/recovery for the given stream.
    [[nodiscard]] RecoveryStats recoveryStats(StreamHandle stream) const;

private:
    struct Journal;
    struct InboundJournal;

    Journal* find(StreamHandle stream);
    const Journal* find(StreamHandle stream) const;
    Result<InboundJournal*> ensureInbound(const PeerId& peer);

    FileStoreOptions options_;
    std::vector<std::unique_ptr<Journal>> journals_;
    std::map<PeerId, std::unique_ptr<InboundJournal>> inbound_;
};

}  // namespace imos::store
