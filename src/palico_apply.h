#pragma once
#include "palico_memory.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace palico {
struct Options { int deploy = -1; int level = 0; int gadget = -1; int gadgetLevel = 0; int loadout = 0; };
struct Result {
    bool ok = false;
    bool changed = false;
    std::string error;
    std::filesystem::path backupPath;
};
constexpr std::size_t kPalicoSlotSize = 0x26cc00;
// Context must positively identify Idle in the base, including loading == 0.
// Capture identity pointers as well as state, so pointer replacement is rejected.
struct ApplyContext {
    std::uintptr_t slot = 0, quest = 0, ui = 0, loading = 0;
    int slotIndex = -1;
};
// All callbacks must be nonthrowing. Except backup, callbacks must not allocate
// or perform file I/O: they also execute while the game's save lock is held.
struct ApplyBackend {
    Reader reader;
    std::uintptr_t exe = 0;
    void* user = nullptr;
    bool (*context)(void*, ApplyContext&) = nullptr;
    bool (*write)(void*, std::uintptr_t, const void*, std::size_t) = nullptr;
    bool (*tryLock)(void*, std::uintptr_t) = nullptr;
    void (*unlock)(void*, std::uintptr_t) = nullptr;
    bool (*backup)(void*, const ApplyContext&, const void*, std::size_t,
                   const std::filesystem::path&, std::filesystem::path&) = nullptr;
};
// Portable transaction core. No memory writes unless a durable full-slot backup
// succeeded; validates all save bytes except the CRITICAL_SECTION at slot+8.
Result ApplyWithBackend(const Options&, const std::filesystem::path&, const ApplyBackend&);
// Windows only production adapter; other platforms return an explicit error.
// Worker must call only before an explicit restart, after observing base Idle.
Result Apply(const Options&, const std::filesystem::path& backupDirectory);
// Save-slot words the plugin writes: deploy flag, palico XP, six gadget XP.
// Captured once before the first apply, restored when the player returns to base.
struct SaveOriginal {
    std::uint32_t deploy = 0, xp = 0, gadgetXp[6] = {};
    std::uintptr_t slot = 0;
    int slotIndex = -1;
    bool valid = false;
};
bool CaptureSaveFields(const ApplyBackend&, const ApplyContext&, SaveOriginal& out);
Result RestoreWithBackend(const SaveOriginal&, const std::filesystem::path&, const ApplyBackend&);
// Process-wide native capture/restore (idempotent capture; restore clears it).
bool CaptureOriginal(std::string& error);
bool RestoreOriginal(const std::filesystem::path& backupDirectory, std::string& error);
// A title-screen boundary ends the character session, even if a new character
// later occupies the same slot address. Never replay an old-session snapshot.
bool EndCharacterSession();
// Base-and-idle check plus the game's own palico loadout apply, without
// touching save data. Used after a restart, because the restart reloads the
// save and would otherwise discard an in-memory equipment change.
// Resolve the exact original-DLL signature before any diagnostic hook patches it.
bool ResolveLoadoutApply(std::uintptr_t exe, std::string& error);
bool AtBaseIdle();
bool ApplyLoadoutOnly(int loadout, std::string& error);
} // namespace palico
