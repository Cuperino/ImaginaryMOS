// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/store/file_store.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <string_view>
#include <utility>

#include <zlib.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace imos::store {

namespace {

// ---- journal record format (little-endian) --------------------------------
//   u32 totalLen        bytes following this field (header + blobs + crc)
//   u16 version         1
//   u8  kind            1 outbound · 2 ack · 3 inbound-note
//   u8  flags           bit0: payload deflate-compressed
//   u64 seq
//   i64 timestampMs
//   u32 messageIdLen
//   u32 rawPayloadLen   uncompressed payload size
//   ... messageId, stored payload ...
//   u32 crc32           zlib crc over everything above it (incl. totalLen)

constexpr std::uint16_t kRecordVersion = 1;
constexpr std::uint8_t kKindOutbound = 1;
constexpr std::uint8_t kKindAck = 2;
constexpr std::uint8_t kKindInboundNote = 3;
constexpr std::uint8_t kFlagDeflate = 0x01;
constexpr std::size_t kFixedHeaderAfterLen = 28;  // version..rawPayloadLen
constexpr std::size_t kCrcBytes = 4;

void put16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

void put64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

std::uint16_t get16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t get64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | p[i];
    }
    return v;
}

std::uint32_t recordCrc(const std::uint8_t* data, std::size_t size) {
    return static_cast<std::uint32_t>(
        ::crc32(0L, reinterpret_cast<const Bytef*>(data), static_cast<uInt>(size)));
}

// ---- tiny platform file abstraction ---------------------------------------

class RawFile {
public:
    RawFile() = default;
    ~RawFile() { close(); }
    RawFile(const RawFile&) = delete;
    RawFile& operator=(const RawFile&) = delete;

    bool openAppend(const std::filesystem::path& path) {
        close();
#ifdef _WIN32
        fd_ = ::_wopen(path.c_str(), _O_WRONLY | _O_APPEND | _O_CREAT | _O_BINARY,
                       _S_IREAD | _S_IWRITE);
#else
        fd_ = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
#endif
        return fd_ >= 0;
    }

    [[nodiscard]] bool isOpen() const { return fd_ >= 0; }

    bool writeAll(const std::uint8_t* data, std::size_t size) {
        while (size > 0) {
#ifdef _WIN32
            const int n = ::_write(fd_, data, static_cast<unsigned>(size));
#else
            const auto n = ::write(fd_, data, size);
#endif
            if (n <= 0) {
                return false;
            }
            data += n;
            size -= static_cast<std::size_t>(n);
        }
        return true;
    }

    bool flushToDisk() {
#ifdef _WIN32
        return ::_commit(fd_) == 0;
#elif defined(__APPLE__)
        return ::fsync(fd_) == 0;
#else
        return ::fdatasync(fd_) == 0;
#endif
    }

    void close() {
        if (fd_ >= 0) {
#ifdef _WIN32
            ::_close(fd_);
#else
            ::close(fd_);
#endif
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

std::string sanitizePeerId(const PeerId& peer) {
    std::string out;
    out.reserve(peer.size() + 9);
    std::uint64_t hash = 1469598103934665603ULL;  // FNV-1a: collision guard
    for (const char c : peer) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        out += safe ? c : '_';
    }
    char suffix[12];
    std::snprintf(suffix, sizeof(suffix), "-%08x", static_cast<unsigned>(hash & 0xFFFFFFFFU));
    return out + suffix;
}

struct ParsedRecord {
    std::uint8_t kind = 0;
    Seq seq = 0;
    std::int64_t timestampMs = 0;
    std::string messageId;
    std::vector<std::uint8_t> payload;  // decompressed
};

std::vector<std::uint8_t> buildRecord(std::uint8_t kind, std::uint8_t flags, Seq seq,
                                      std::int64_t timestampMs, std::string_view messageId,
                                      std::span<const std::uint8_t> storedPayload,
                                      std::uint32_t rawPayloadLen) {
    std::vector<std::uint8_t> out;
    const std::size_t total =
        kFixedHeaderAfterLen + messageId.size() + storedPayload.size() + kCrcBytes;
    out.reserve(4 + total);
    put32(out, static_cast<std::uint32_t>(total));
    put16(out, kRecordVersion);
    out.push_back(kind);
    out.push_back(flags);
    put64(out, seq);
    put64(out, static_cast<std::uint64_t>(timestampMs));
    put32(out, static_cast<std::uint32_t>(messageId.size()));
    put32(out, rawPayloadLen);
    out.insert(out.end(), messageId.begin(), messageId.end());
    out.insert(out.end(), storedPayload.begin(), storedPayload.end());
    put32(out, recordCrc(out.data(), out.size()));
    return out;
}

std::vector<std::uint8_t> encodePayload(std::span<const std::uint8_t> payload,
                                        std::size_t compressThreshold, std::uint8_t& flags) {
    flags = 0;
    if (payload.size() < compressThreshold) {
        return {payload.begin(), payload.end()};
    }
    uLongf bound = ::compressBound(static_cast<uLong>(payload.size()));
    std::vector<std::uint8_t> compressed(bound);
    if (::compress2(compressed.data(), &bound, payload.data(), static_cast<uLong>(payload.size()),
                    Z_DEFAULT_COMPRESSION) != Z_OK ||
        bound >= payload.size()) {
        return {payload.begin(), payload.end()};  // incompressible: store raw
    }
    compressed.resize(bound);
    flags = kFlagDeflate;
    return compressed;
}

}  // namespace

// ---- journal state ---------------------------------------------------------

struct FileMessageStore::Journal {
    std::filesystem::path path;
    RawFile file;
    Seq nextSeq = 1;
    std::map<Seq, PendingRecord> live;  // ordered: replay oldest first
    std::size_t deadRecords = 0;
    bool dirty = false;
    std::chrono::steady_clock::time_point lastFlush{};
    RecoveryStats stats;
    std::string key;  // peer + channel identity for reopen checks
};

struct FileMessageStore::InboundJournal {
    std::filesystem::path path;
    RawFile file;
    std::deque<std::string> order;
    std::set<std::string, std::less<>> seen;
    std::size_t fileRecords = 0;
    bool dirty = false;
};

namespace {

/// Scan a journal file, applying good records through `apply(kind, record)`.
/// Returns the number of bytes of torn/corrupt tail that were truncated.
template <typename Apply>
std::size_t recoverJournal(const std::filesystem::path& path, std::size_t maxRecordBytes,
                           Apply&& apply) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return 0;
    }
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    in.close();

    std::size_t offset = 0;
    std::size_t lastGood = 0;
    while (bytes.size() - offset >= 4) {
        const std::uint32_t totalLen = get32(bytes.data() + offset);
        const std::size_t recordEnd = offset + 4 + totalLen;
        if (totalLen < kFixedHeaderAfterLen + kCrcBytes || totalLen > maxRecordBytes ||
            recordEnd > bytes.size()) {
            break;  // torn or corrupt tail
        }
        const std::uint32_t storedCrc = get32(bytes.data() + recordEnd - kCrcBytes);
        if (recordCrc(bytes.data() + offset, 4 + totalLen - kCrcBytes) != storedCrc) {
            break;
        }
        const std::uint8_t* p = bytes.data() + offset + 4;
        ParsedRecord record;
        const std::uint16_t version = get16(p);
        record.kind = p[2];
        const std::uint8_t flags = p[3];
        record.seq = get64(p + 4);
        record.timestampMs = static_cast<std::int64_t>(get64(p + 12));
        const std::uint32_t messageIdLen = get32(p + 20);
        const std::uint32_t rawPayloadLen = get32(p + 24);
        const std::size_t variable = totalLen - kFixedHeaderAfterLen - kCrcBytes;
        if (version != kRecordVersion || messageIdLen > variable ||
            rawPayloadLen > maxRecordBytes) {
            break;
        }
        const std::size_t storedPayloadLen = variable - messageIdLen;
        const std::uint8_t* messageId = p + kFixedHeaderAfterLen;
        const std::uint8_t* payload = messageId + messageIdLen;
        record.messageId.assign(reinterpret_cast<const char*>(messageId), messageIdLen);
        if ((flags & kFlagDeflate) != 0) {
            record.payload.resize(rawPayloadLen);
            uLongf outLen = rawPayloadLen;
            if (::uncompress(record.payload.data(), &outLen, payload,
                             static_cast<uLong>(storedPayloadLen)) != Z_OK ||
                outLen != rawPayloadLen) {
                break;  // corrupt compressed body: treat as tail damage
            }
        } else {
            record.payload.assign(payload, payload + storedPayloadLen);
        }
        apply(record);
        offset = recordEnd;
        lastGood = recordEnd;
    }

    const std::size_t truncated = bytes.size() - lastGood;
    if (truncated > 0) {
        std::filesystem::resize_file(path, lastGood, ec);
    }
    return truncated;
}

}  // namespace

// ---- FileMessageStore ------------------------------------------------------

FileMessageStore::FileMessageStore(FileStoreOptions options) : options_(std::move(options)) {}

FileMessageStore::~FileMessageStore() = default;

FileMessageStore::Journal* FileMessageStore::find(StreamHandle stream) {
    const std::size_t index = static_cast<std::size_t>(stream.id) - 1;
    return index < journals_.size() ? journals_[index].get() : nullptr;
}

const FileMessageStore::Journal* FileMessageStore::find(StreamHandle stream) const {
    const std::size_t index = static_cast<std::size_t>(stream.id) - 1;
    return index < journals_.size() ? journals_[index].get() : nullptr;
}

Result<StreamHandle> FileMessageStore::open(const PeerId& peer, Channel channel) {
    const std::string key = peer + "\x1f" + std::string{channelName(channel)};
    for (std::size_t i = 0; i < journals_.size(); ++i) {
        if (journals_[i]->key == key) {
            return StreamHandle{i + 1};
        }
    }

    auto journal = std::make_unique<Journal>();
    journal->key = key;
    const std::filesystem::path dir = options_.root / sanitizePeerId(peer);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return makeError(ErrorCode::IoError,
                         "cannot create store directory " + dir.string() + ": " + ec.message());
    }
    journal->path = dir / (std::string{channelName(channel)} + ".journal");

    journal->stats.truncatedBytes =
        recoverJournal(journal->path, options_.maxRecordBytes, [&](const ParsedRecord& r) {
            if (r.kind == kKindOutbound) {
                PendingRecord pending;
                pending.seq = r.seq;
                pending.messageId = r.messageId;
                pending.payload = r.payload;
                pending.enqueuedAt = Timestamp{r.timestampMs};
                journal->live.emplace(r.seq, std::move(pending));
                journal->nextSeq = std::max(journal->nextSeq, r.seq + 1);
            } else if (r.kind == kKindAck) {
                journal->live.erase(r.seq);
                ++journal->deadRecords;
                journal->nextSeq = std::max(journal->nextSeq, r.seq + 1);
            }
        });
    journal->stats.liveRecords = journal->live.size();

    if (!journal->file.openAppend(journal->path)) {
        return makeError(ErrorCode::IoError, "cannot open journal " + journal->path.string());
    }
    journals_.push_back(std::move(journal));
    return StreamHandle{journals_.size()};
}

Result<Seq> FileMessageStore::append(StreamHandle stream, const OutboundRecord& record) {
    Journal* journal = find(stream);
    if (journal == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "unknown stream handle");
    }
    const Seq seq = journal->nextSeq++;
    std::uint8_t flags = 0;
    const std::vector<std::uint8_t> stored =
        encodePayload(record.payload, options_.compressThresholdBytes, flags);
    const std::vector<std::uint8_t> bytes =
        buildRecord(kKindOutbound, flags, seq, record.enqueuedAt.millisSinceEpoch, record.messageId,
                    stored, static_cast<std::uint32_t>(record.payload.size()));
    if (!journal->file.writeAll(bytes.data(), bytes.size())) {
        return makeError(ErrorCode::IoError, "journal append failed");
    }
    journal->dirty = true;

    PendingRecord pending;
    pending.seq = seq;
    pending.messageId = std::string{record.messageId};
    pending.payload.assign(record.payload.begin(), record.payload.end());
    pending.enqueuedAt = record.enqueuedAt;
    pending.flags = record.flags;
    journal->live.emplace(seq, std::move(pending));

    if (options_.syncPolicy == SyncPolicy::EachAppend) {
        if (!journal->file.flushToDisk()) {
            return makeError(ErrorCode::IoError, "journal fsync failed");
        }
        journal->dirty = false;
    } else if (options_.syncPolicy == SyncPolicy::Batched) {
        const auto now = std::chrono::steady_clock::now();
        if (now - journal->lastFlush >= std::chrono::milliseconds{options_.batchWindowMillis}) {
            if (journal->file.flushToDisk()) {
                journal->dirty = false;
                journal->lastFlush = now;
            }
        }
    }
    return seq;
}

Result<void> FileMessageStore::markAcked(StreamHandle stream, Seq seq) {
    Journal* journal = find(stream);
    if (journal == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "unknown stream handle");
    }
    const std::vector<std::uint8_t> bytes = buildRecord(kKindAck, 0, seq, 0, {}, {}, 0);
    if (!journal->file.writeAll(bytes.data(), bytes.size())) {
        return makeError(ErrorCode::IoError, "journal append failed");
    }
    journal->dirty = true;
    journal->live.erase(seq);
    ++journal->deadRecords;
    if (options_.compactAfterDeadRecords != 0 &&
        journal->deadRecords >= options_.compactAfterDeadRecords) {
        return compact(stream);
    }
    return {};
}

Result<PendingCursor> FileMessageStore::pending(StreamHandle stream) {
    Journal* journal = find(stream);
    if (journal == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "unknown stream handle");
    }
    std::vector<PendingRecord> records;
    records.reserve(journal->live.size());
    for (const auto& [seq, record] : journal->live) {
        records.push_back(record);
    }
    return PendingCursor{std::move(records)};
}

Result<void> FileMessageStore::compact(StreamHandle stream) {
    Journal* journal = find(stream);
    if (journal == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "unknown stream handle");
    }
    const std::filesystem::path tmp = journal->path.string() + ".tmp";
    {
        RawFile out;
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        if (!out.openAppend(tmp)) {
            return makeError(ErrorCode::IoError, "cannot create " + tmp.string());
        }
        for (const auto& [seq, record] : journal->live) {
            std::uint8_t flags = 0;
            const std::vector<std::uint8_t> stored = encodePayload(
                std::span<const std::uint8_t>{record.payload.data(), record.payload.size()},
                options_.compressThresholdBytes, flags);
            const std::vector<std::uint8_t> bytes = buildRecord(
                kKindOutbound, flags, seq, record.enqueuedAt.millisSinceEpoch, record.messageId,
                stored, static_cast<std::uint32_t>(record.payload.size()));
            if (!out.writeAll(bytes.data(), bytes.size())) {
                return makeError(ErrorCode::IoError, "compaction write failed");
            }
        }
        if (!out.flushToDisk()) {
            return makeError(ErrorCode::IoError, "compaction fsync failed");
        }
    }
    journal->file.close();
    std::error_code ec;
    std::filesystem::rename(tmp, journal->path, ec);  // atomic replace
    if (ec) {
        return makeError(ErrorCode::IoError, "compaction rename failed: " + ec.message());
    }
    if (!journal->file.openAppend(journal->path)) {
        return makeError(ErrorCode::IoError, "cannot reopen compacted journal");
    }
    journal->deadRecords = 0;
    journal->dirty = false;
    return {};
}

Result<void> FileMessageStore::sync(StreamHandle stream) {
    Journal* journal = find(stream);
    if (journal == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "unknown stream handle");
    }
    if (journal->dirty) {
        if (!journal->file.flushToDisk()) {
            return makeError(ErrorCode::IoError, "journal fsync failed");
        }
        journal->dirty = false;
        journal->lastFlush = std::chrono::steady_clock::now();
    }
    return {};
}

RecoveryStats FileMessageStore::recoveryStats(StreamHandle stream) const {
    const Journal* journal = find(stream);
    return journal != nullptr ? journal->stats : RecoveryStats{};
}

Result<FileMessageStore::InboundJournal*> FileMessageStore::ensureInbound(const PeerId& peer) {
    const auto existing = inbound_.find(peer);
    if (existing != inbound_.end()) {
        return existing->second.get();
    }
    auto journal = std::make_unique<InboundJournal>();
    const std::filesystem::path dir = options_.root / sanitizePeerId(peer);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return makeError(ErrorCode::IoError,
                         "cannot create store directory " + dir.string() + ": " + ec.message());
    }
    journal->path = dir / "inbound.journal";
    recoverJournal(journal->path, options_.maxRecordBytes, [&](const ParsedRecord& r) {
        if (r.kind != kKindInboundNote || journal->seen.contains(r.messageId)) {
            return;
        }
        journal->order.push_back(r.messageId);
        journal->seen.insert(r.messageId);
        ++journal->fileRecords;
        while (options_.dedupeWindow != 0 && journal->order.size() > options_.dedupeWindow) {
            journal->seen.erase(journal->order.front());
            journal->order.pop_front();
        }
    });
    if (!journal->file.openAppend(journal->path)) {
        return makeError(ErrorCode::IoError, "cannot open " + journal->path.string());
    }
    InboundJournal* raw = journal.get();
    inbound_.emplace(peer, std::move(journal));
    return raw;
}

Result<void> FileMessageStore::noteInbound(const PeerId& peer, MessageIdView messageId,
                                           Timestamp receivedAt) {
    Result<InboundJournal*> journal = ensureInbound(peer);
    if (!journal.ok()) {
        return std::move(journal).error();
    }
    InboundJournal& inbound = *journal.value();
    if (inbound.seen.contains(messageId)) {
        return {};
    }
    const std::vector<std::uint8_t> bytes =
        buildRecord(kKindInboundNote, 0, 0, receivedAt.millisSinceEpoch, messageId, {}, 0);
    if (!inbound.file.writeAll(bytes.data(), bytes.size())) {
        return makeError(ErrorCode::IoError, "inbound journal append failed");
    }
    ++inbound.fileRecords;
    inbound.order.emplace_back(messageId);
    inbound.seen.insert(std::string{messageId});
    while (options_.dedupeWindow != 0 && inbound.order.size() > options_.dedupeWindow) {
        inbound.seen.erase(inbound.order.front());
        inbound.order.pop_front();
    }

    // Rewrite once the file drags far more history than the window keeps.
    if (options_.dedupeWindow != 0 && inbound.fileRecords > options_.dedupeWindow * 4) {
        const std::filesystem::path tmp = inbound.path.string() + ".tmp";
        RawFile out;
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        if (out.openAppend(tmp)) {
            bool ok = true;
            for (const std::string& id : inbound.order) {
                const std::vector<std::uint8_t> record =
                    buildRecord(kKindInboundNote, 0, 0, receivedAt.millisSinceEpoch, id, {}, 0);
                ok = ok && out.writeAll(record.data(), record.size());
            }
            if (ok && out.flushToDisk()) {
                out.close();
                inbound.file.close();
                std::filesystem::rename(tmp, inbound.path, ec);
                if (!ec && inbound.file.openAppend(inbound.path)) {
                    inbound.fileRecords = inbound.order.size();
                }
            }
        }
    }
    if (options_.syncPolicy == SyncPolicy::EachAppend && !inbound.file.flushToDisk()) {
        return makeError(ErrorCode::IoError, "inbound journal fsync failed");
    }
    return {};
}

Result<bool> FileMessageStore::seenInbound(const PeerId& peer, MessageIdView messageId) {
    Result<InboundJournal*> journal = ensureInbound(peer);
    if (!journal.ok()) {
        return std::move(journal).error();
    }
    return journal.value()->seen.contains(messageId);
}

}  // namespace imos::store
