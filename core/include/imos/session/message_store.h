// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// IMessageStore (plan §6): durable buffering of unacknowledged/queued
// messages across restarts — the spec's durability recommendation — plus the
// inbound messageID window that makes retries idempotent. The interface is
// small and transport-agnostic; the default file-backed implementation lives
// in store/ (imos_store_file), and applications may substitute their own.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imos/error.h>
#include <imos/protocol.h>

namespace imos {

/// Stable identity of one peer relationship (one mosID↔ncsID pairing);
/// used to key store directories and dedupe tables.
using PeerId = std::string;
using MessageIdView = std::string_view;
using Seq = std::uint64_t;

/// Wall-clock milliseconds since the Unix epoch (store records outlive any
/// monotonic timeline).
struct Timestamp {
    std::int64_t millisSinceEpoch = 0;
};

/// Opaque handle to one (peer, channel) outbound stream.
struct StreamHandle {
    std::uint64_t id = 0;
};

/// One outbound message as handed to the store, durable before send.
struct OutboundRecord {
    MessageIdView messageId;
    std::span<const std::uint8_t> payload;  ///< serialized UTF-8 document
    Timestamp enqueuedAt;
    std::uint32_t flags = 0;
};

/// A pending (unacknowledged) record replayed after restart.
struct PendingRecord {
    Seq seq = 0;
    std::string messageId;
    std::vector<std::uint8_t> payload;
    Timestamp enqueuedAt;
    std::uint32_t flags = 0;
};

/// Snapshot cursor over the pending set, oldest first.
class PendingCursor {
public:
    PendingCursor() = default;
    explicit PendingCursor(std::vector<PendingRecord> records) : records_(std::move(records)) {}

    std::optional<PendingRecord> next() {
        if (index_ >= records_.size()) {
            return std::nullopt;
        }
        return std::move(records_[index_++]);
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return records_.size() - index_; }

private:
    std::vector<PendingRecord> records_;
    std::size_t index_ = 0;
};

class IMessageStore {
public:
    virtual Result<StreamHandle> open(const PeerId& peer, Channel channel) = 0;
    /// Assigns and returns the record's sequence number; the record must be
    /// durable (per the backend's sync policy) before the caller transmits.
    virtual Result<Seq> append(StreamHandle stream, const OutboundRecord& record) = 0;
    virtual Result<void> markAcked(StreamHandle stream, Seq seq) = 0;
    /// Replay the unacknowledged set after restart, oldest first.
    virtual Result<PendingCursor> pending(StreamHandle stream) = 0;
    /// Record an inbound messageID for duplicate suppression.
    virtual Result<void> noteInbound(const PeerId& peer, MessageIdView messageId,
                                     Timestamp receivedAt) = 0;
    virtual Result<bool> seenInbound(const PeerId& peer, MessageIdView messageId) = 0;
    /// Drop acknowledged prefixes / prune the dedupe window.
    virtual Result<void> compact(StreamHandle stream) = 0;
    /// Flush buffered records to durable storage.
    virtual Result<void> sync(StreamHandle stream) = 0;
    virtual ~IMessageStore() = default;
};

}  // namespace imos
