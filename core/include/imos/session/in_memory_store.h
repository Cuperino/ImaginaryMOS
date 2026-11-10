// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Non-durable IMessageStore for tests and hosts that opt out of durability.
// Pure memory, so it may live in the sans-I/O core; it doubles as the
// reference semantics the file backend's tests compare against.

#pragma once

#include <cstddef>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <imos/core_export.h>
#include <imos/session/message_store.h>

namespace imos {

class IMOS_CORE_API InMemoryMessageStore final : public IMessageStore {
public:
    explicit InMemoryMessageStore(std::size_t dedupeWindow = 256) : dedupeWindow_(dedupeWindow) {}

    Result<StreamHandle> open(const PeerId& peer, Channel channel) override;
    Result<Seq> append(StreamHandle stream, const OutboundRecord& record) override;
    Result<void> markAcked(StreamHandle stream, Seq seq) override;
    Result<PendingCursor> pending(StreamHandle stream) override;
    Result<void> noteInbound(const PeerId& peer, MessageIdView messageId,
                             Timestamp receivedAt) override;
    Result<bool> seenInbound(const PeerId& peer, MessageIdView messageId) override;
    Result<std::uint64_t> lastMessageId(const PeerId& peer) override;
    Result<void> noteMessageId(const PeerId& peer, std::uint64_t messageId) override;
    Result<void> compact(StreamHandle stream) override;
    Result<void> sync(StreamHandle stream) override;

private:
    struct Stream {
        std::string key;
        Seq nextSeq = 1;
        std::vector<PendingRecord> pending;  // oldest first
    };
    struct Inbound {
        std::deque<std::string> order;
        std::set<std::string, std::less<>> seen;
    };

    Stream* find(StreamHandle stream);

    std::size_t dedupeWindow_;
    std::vector<Stream> streams_;
    std::map<PeerId, Inbound> inbound_;
    std::map<PeerId, std::uint64_t> messageIds_;
};

}  // namespace imos
