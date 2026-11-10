// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/session/in_memory_store.h>

#include <algorithm>
#include <string>

namespace imos {

namespace {

std::string streamKey(const PeerId& peer, Channel channel) {
    return peer + "\x1f" + std::string{channelName(channel)};
}

}  // namespace

InMemoryMessageStore::Stream* InMemoryMessageStore::find(StreamHandle stream) {
    const std::size_t index = static_cast<std::size_t>(stream.id) - 1;
    return index < streams_.size() ? &streams_[index] : nullptr;
}

Result<StreamHandle> InMemoryMessageStore::open(const PeerId& peer, Channel channel) {
    const std::string key = streamKey(peer, channel);
    for (std::size_t i = 0; i < streams_.size(); ++i) {
        if (streams_[i].key == key) {
            return StreamHandle{i + 1};
        }
    }
    Stream stream;
    stream.key = key;
    streams_.push_back(std::move(stream));
    return StreamHandle{streams_.size()};
}

Result<Seq> InMemoryMessageStore::append(StreamHandle handle, const OutboundRecord& record) {
    Stream* stream = find(handle);
    if (stream == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "unknown stream handle");
    }
    PendingRecord pending;
    pending.seq = stream->nextSeq++;
    pending.messageId = std::string{record.messageId};
    pending.payload.assign(record.payload.begin(), record.payload.end());
    pending.enqueuedAt = record.enqueuedAt;
    pending.flags = record.flags;
    stream->pending.push_back(std::move(pending));
    return stream->pending.back().seq;
}

Result<void> InMemoryMessageStore::markAcked(StreamHandle handle, Seq seq) {
    Stream* stream = find(handle);
    if (stream == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "unknown stream handle");
    }
    std::erase_if(stream->pending, [seq](const PendingRecord& r) { return r.seq == seq; });
    return {};
}

Result<PendingCursor> InMemoryMessageStore::pending(StreamHandle handle) {
    Stream* stream = find(handle);
    if (stream == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "unknown stream handle");
    }
    return PendingCursor{stream->pending};
}

Result<void> InMemoryMessageStore::noteInbound(const PeerId& peer, MessageIdView messageId,
                                               Timestamp /*receivedAt*/) {
    Inbound& inbound = inbound_[peer];
    if (inbound.seen.contains(messageId)) {
        return {};
    }
    inbound.order.emplace_back(messageId);
    inbound.seen.insert(std::string{messageId});
    while (dedupeWindow_ != 0 && inbound.order.size() > dedupeWindow_) {
        inbound.seen.erase(inbound.order.front());
        inbound.order.pop_front();
    }
    return {};
}

Result<bool> InMemoryMessageStore::seenInbound(const PeerId& peer, MessageIdView messageId) {
    const auto it = inbound_.find(peer);
    return it != inbound_.end() && it->second.seen.contains(messageId);
}

Result<std::uint64_t> InMemoryMessageStore::lastMessageId(const PeerId& peer) {
    const auto it = messageIds_.find(peer);
    return it == messageIds_.end() ? std::uint64_t{0} : it->second;
}

Result<void> InMemoryMessageStore::noteMessageId(const PeerId& peer, std::uint64_t messageId) {
    std::uint64_t& stored = messageIds_[peer];
    stored = messageId;
    return {};
}

Result<void> InMemoryMessageStore::compact(StreamHandle handle) {
    return find(handle) != nullptr
               ? Result<void>{}
               : Result<void>{makeError(ErrorCode::InvalidArgument, "unknown stream handle")};
}

Result<void> InMemoryMessageStore::sync(StreamHandle handle) {
    return find(handle) != nullptr
               ? Result<void>{}
               : Result<void>{makeError(ErrorCode::InvalidArgument, "unknown stream handle")};
}

}  // namespace imos
