#pragma once
#include "config.h"
#include <array>
#include <string>

namespace settings_transfer {
constexpr size_t MaxPartBytes = 1024 * 1024;
constexpr size_t MaxBackupBytes = 4 * MaxPartBytes + 256;
struct Snapshot { Config global; std::array<Config, 3> slots; };
struct Storage {
    std::wstring globalPath;
    std::array<std::wstring, 3> slotPaths;
    int currentSlot = 1;
};
// Pure data API; outputs are published only on success. Format is versioned,
// bounded, length-delimited INI; no account identifiers or filesystem paths.
bool Encode(const Snapshot&, std::string& bytes, std::string& err);
bool Decode(const std::string& bytes, Snapshot&, std::string& err);
// File API. Caller serializes access with ordinary config saving/slot switching.
// Capture reads ONLY other slots; missing slots are empty, read errors fail.
Storage CurrentStorage();
bool Capture(const Storage&, const Config& live, Snapshot&, std::string& err);
// CREATE_NEW: refuses an existing destination. Does not persist/change live config.
bool ExportFile(const Storage&, const Config& live, const std::wstring& destination, std::string& err);
// Preflight -> flushed independent timestamp backup -> stage all -> replace all.
// On failure rolls back exact prior bytes/existence; failed rollback is explicitly
// reported and recovery files retained. No crash-level multi-file atomicity claim.
// Imported current selections/quest are cleared in all slots before persistence.
// live is untouched on any failure; backupPath is set once recovery backup exists.
bool ImportFile(const Storage&, const std::wstring& source, const std::wstring& backupDirectory,
                Config& live, std::wstring& backupPath, std::string& err);
namespace testing {
// Thread-local fault injection, 1-based replacement index; 0 disables.
void FailCommitAt(int index);
void FailRollbackAt(int index);
}
} // namespace settings_transfer
