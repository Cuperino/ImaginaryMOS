// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Normalization of the thirteen messages removed in MOS v4.0 (plan §3.2).
// A v4.0 endpoint never initiates them, but real 2.8-era peers still emit
// them, so the codec parses all thirteen and this pass rewrites them onto
// roElementAction / roElementStat semantics before they reach application
// handlers. The original message is left untouched (retention rules apply);
// the returned message is a canonical rebuild carrying the same envelope and
// the source subtrees (story/item and their metadata) verbatim.

#pragma once

#include <imos/codec/message.h>
#include <imos/core_export.h>
#include <imos/error.h>

namespace imos::codec {

/// True for the thirteen removed legacy message types.
[[nodiscard]] IMOS_CORE_API bool isLegacyRemoved(MessageType type) noexcept;

/// Rewrite a legacy message onto its 4.0 equivalent:
///   roStory*/roItem* sequence messages -> roElementAction (INSERT / REPLACE
///   / MOVE / SWAP / DELETE with the fixture-documented target/source shapes)
///   roStat -> roElementStat element="RO"; roItemStat -> element="ITEM".
/// Fails with InvalidArgument for non-legacy input and ProtocolViolation for
/// structurally unusable payloads (e.g. a move without a target).
[[nodiscard]] IMOS_CORE_API Result<Message> normalizeLegacy(const Message& legacy);

}  // namespace imos::codec
