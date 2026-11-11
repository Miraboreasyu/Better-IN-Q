#pragma once
#include "palico_rules.h"
#include <cstddef>
#include <cstdint>
namespace palico {
// Explicit success is required: a failed read must never masquerade as player 0,
// rank 0 or an expired cooldown. Injectable for mapped-memory regression tests.
using ReadMemory = bool (*)(std::uintptr_t, void*, std::size_t, void*);
struct Reader {
    ReadMemory read = nullptr;
    void* context = nullptr;
    template<class T> bool Get(std::uintptr_t a, T& v) const {
        return read && a >= 0x10000 && read(a,&v,sizeof(v),context);
    }
    bool Bytes(std::uintptr_t a, void* p, std::size_t n) const {
        return read && a >= 0x10000 && read(a,p,n,context);
    }
};
bool ReadOwnedCoral(const Reader&, std::uintptr_t exe, std::uintptr_t object, Snapshot&);
bool ReadSaveSlot(const Reader&, std::uintptr_t exe, std::uintptr_t& slot, int& index);
bool ReadExperienceTable(const Reader&, std::uintptr_t exe, int gadget, int* values, int& count);
} // namespace palico
