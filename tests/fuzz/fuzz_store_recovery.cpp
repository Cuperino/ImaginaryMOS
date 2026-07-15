// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Fuzz surface 3 (plan §10): store journal recovery. The input bytes are
// written verbatim as a journal file; opening the store must never crash,
// and every replayed pending record must be internally consistent.

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <imos/store/file_store.h>

namespace {

std::filesystem::path fuzzRoot() {
    static const std::filesystem::path root = [] {
        std::filesystem::path dir =
            std::filesystem::temp_directory_path() /
            ("imos-fuzz-store-" + std::to_string(static_cast<unsigned>(::getpid())));
        std::filesystem::create_directories(dir);
        return dir;
    }();
    return root;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::filesystem::path root = fuzzRoot();

    imos::store::FileStoreOptions options;
    options.root = root;
    options.maxRecordBytes = 1U << 20;

    {
        // The sanitized directory name for peer id "peer" is deterministic;
        // create it by opening once, then locate the journal and overwrite it
        // with the fuzz bytes.
        imos::store::FileMessageStore probe{options};
        (void)probe.open("peer", imos::Channel::Mom);
    }
    std::filesystem::path journal;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (std::filesystem::exists(entry.path() / "mom.journal")) {
            journal = entry.path() / "mom.journal";
            break;
        }
    }
    if (journal.empty()) {
        return 0;
    }
    {
        std::ofstream out{journal, std::ios::binary | std::ios::trunc};
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }

    imos::store::FileMessageStore store{options};
    auto handle = store.open("peer", imos::Channel::Mom);
    if (!handle.ok()) {
        return 0;
    }
    auto cursor = store.pending(handle.value());
    if (!cursor.ok()) {
        return 0;
    }
    imos::Seq last = 0;
    while (auto record = cursor.value().next()) {
        if (record->seq <= last) {
            std::abort();  // replay must be strictly ordered
        }
        last = record->seq;
        if (record->payload.size() > options.maxRecordBytes) {
            std::abort();  // inflation cap must hold
        }
    }
    // The journal must accept appends after any recovery.
    const std::uint8_t probe[] = {'<', 'm', 'o', 's', '/', '>'};
    imos::OutboundRecord record;
    record.messageId = "fuzz";
    record.payload = probe;
    if (!store.append(handle.value(), record).ok()) {
        std::abort();
    }
    return 0;
}
