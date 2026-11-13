#include "palico_apply.h"
#include "palico_rules.h"
#include "hook.h"
#include <array>
#include <cstring>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace palico {
namespace {
constexpr std::size_t kLockBegin = 8, kLockEnd = 48; // Win64 CRITICAL_SECTION, 40 bytes.
constexpr std::size_t kXp = 0x10265c, kGadgetXp = 0x102660;
constexpr std::size_t kGadget = 0x2d4, kDeploy = 0x1026f8;
// DLL 9f40 reads slot+2d4 as the equipped gadget; 22239 passes that
// same value to 28350. This is NOT proof of a safe gadget setter.
struct Patch { std::size_t offset; std::uint32_t before, after; };
bool Same(const ApplyContext& a, const ApplyContext& b) {
    return a.slot == b.slot && a.slotIndex == b.slotIndex && a.quest == b.quest &&
           a.ui == b.ui && a.loading == b.loading;
}
std::uint32_t Word(const std::vector<unsigned char>& s, std::size_t o) {
    std::uint32_t v; std::memcpy(&v, s.data()+o, 4); return v;
}
bool Unchanged(const ApplyBackend& b, const ApplyContext& c, const std::vector<unsigned char>& s) {
    std::array<unsigned char,4096> block{};
    // Exclude only the lock itself: acquisition necessarily mutates it.
    for (std::size_t begin : {std::size_t(0), kLockEnd}) {
        const std::size_t end = begin == 0 ? kLockBegin : s.size();
        for (std::size_t at=begin; at<end;) {
            const auto n = (end-at < block.size()) ? end-at : block.size();
            if (!b.reader.Bytes(c.slot+at,block.data(),n) || std::memcmp(block.data(),s.data()+at,n)) return false;
            at += n;
        }
    }
    return true;
}
// Runs with save lock held. Fixed storage only; returns literal diagnostics.
const char* Commit(const ApplyBackend& b, const ApplyContext& c,
                   const std::vector<unsigned char>& snapshot, const Patch* p, int count, bool& changed) {
    ApplyContext now;
    if (!b.context(b.user,now) || !Same(c,now) || !Unchanged(b,c,snapshot))
        return "Save slot, original data, or base/loading context changed after backup; nothing written";
    int attempted=0;
    const char* failure=nullptr;
    for (int i=0;i<count;++i) {
        if (!b.context(b.user,now) || !Same(c,now)) { failure="Base/loading context changed during transaction"; break; }
        attempted=i+1; // A failing writer can have partially modified this int.
        std::uint32_t verify=0;
        if (!b.write(b.user,c.slot+p[i].offset,&p[i].after,4) ||
            !b.reader.Get(c.slot+p[i].offset,verify) || verify!=p[i].after) {
            failure="Palico write or read-back failed"; break;
        }
    }
    if (!failure && (!b.context(b.user,now) || !Same(c,now))) failure="Base/loading context changed during transaction";
    if (!failure) { changed=true; return nullptr; }
    bool restored=true;
    for (int i=attempted-1;i>=0;--i) {
        std::uint32_t verify=0;
        const bool wrote=b.write(b.user,c.slot+p[i].offset,&p[i].before,4);
        const bool read=b.reader.Get(c.slot+p[i].offset,verify);
        if (!wrote || !read || verify!=p[i].before) restored=false;
    }
    if (!restored) return "Palico transaction failed AND rollback failed; retain backup for manual recovery";
    return failure;
}
}
namespace {
std::uintptr_t g_loadoutApply = 0;
SaveOriginal g_saveOriginal;
}
bool ResolveLoadoutApply(std::uintptr_t exe, std::string& error) {
    if (g_loadoutApply) return true;
    std::uintptr_t text=0; std::size_t size=0;
    if (!hook::TextRange(exe,text,size)) { error="Palico loadout: game text range is unavailable"; return false; }
    const auto hits=hook::FindAll(text,size,"48 8B C4 48 89 48 08 53 48 83 EC 60");
    if (hits.size()!=1) { error="Palico loadout: the original apply signature has " + std::to_string(hits.size()) + " matches; no call made"; return false; }
    g_loadoutApply=hits[0];
    error.clear(); return true;
}
bool ApplyLoadoutGame(const Reader& r,std::uintptr_t exe,int loadout,std::string& error) {
    // Verbatim port of speedrun_util's apply (module rva 0x22430):
    //   obj = registry[0x62]                      ; the equip menu object
    //   old = *(obj+0x2958)->+0x274, *(obj+0x2960)->+0x240
    //   *(obj+0x2958)->+0x274 = (n-1)%8 ; *(obj+0x2960)->+0x240 = (n-1)/8
    //   fn = AOB '48 8B C4 48 89 48 08 53 48 83 EC 60'   ; NOT from a vtable
    //   fn(&obj, 0, 0)                            ; callee does mov rcx,[rcx]
    //   restore the two cursor ints
    if (loadout < 1 || loadout > 24) { error="Palico loadout: set number must be 1..24"; return false; }
    if (!ResolveLoadoutApply(exe,error)) return false;
    const auto fn=g_loadoutApply;
    std::uintptr_t base=0,array=0,obj=0,idxPtr=0,pagePtr=0;
    if (!r.Get(exe+0x51C4640,base)||!base||!r.Get(base+0x278,array)||!array||
        !r.Get(array+0x62*8,obj)||!obj) {
        error="Palico loadout: the game's palico loadout menu object is unavailable; nothing applied";
        return false;
    }
    // Original DLL 0x2249f..0x224c7: require uGUIEquipMySet's exact vtable.
    std::uintptr_t vtable=0,slot=0; int slotIndex=-1; std::uint32_t weapon=0;
    if (!r.Get(obj,vtable) || vtable!=exe+0x346cfe0) {
        error="Palico loadout: registry entry is not uGUIEquipMySet; no call made"; return false;
    }
    // Original DLL 0x9e00: empty record is identified by the saved weapon -1.
    if (!ReadSaveSlot(r,exe,slot,slotIndex) ||
        !r.Get(slot+0x13b674+std::uintptr_t(loadout-1)*0x130,weapon)) {
        error="Palico loadout: saved loadout record is unreadable; no call made"; return false;
    }
    if (weapon==0xffffffffu) { error="Palico loadout: the selected saved loadout is empty; no call made"; return false; }
    if (!r.Get(obj+0x2958,idxPtr)||!idxPtr||!r.Get(obj+0x2960,pagePtr)||!pagePtr) {
        error="Palico loadout: the menu object has no cursor fields; nothing applied";
        return false;
    }
    std::uint32_t oldIdx=0,oldPage=0;
    if (!r.Get(idxPtr+0x274,oldIdx)||!r.Get(pagePtr+0x240,oldPage)) {
        error="Palico loadout: cannot read the current menu cursor; nothing applied";
        return false;
    }
    const int n=loadout-1;
    const std::uint32_t idx=std::uint32_t(n%8),page=std::uint32_t(n/8);
    if (!hook::WriteMem(idxPtr+0x274,&idx,sizeof idx)||!hook::WriteMem(pagePtr+0x240,&page,sizeof page)) {
        hook::WriteMem(idxPtr+0x274,&oldIdx,sizeof oldIdx);
        hook::WriteMem(pagePtr+0x240,&oldPage,sizeof oldPage);
        error="Palico loadout: cannot set the menu cursor; nothing applied"; return false;
    }
    using Fn=std::uintptr_t(*)(void*,std::uintptr_t,std::uintptr_t);
    reinterpret_cast<Fn>(fn)(&obj,0,0);
    const bool restoredIdx=hook::WriteMem(idxPtr+0x274,&oldIdx,sizeof oldIdx);
    const bool restoredPage=hook::WriteMem(pagePtr+0x240,&oldPage,sizeof oldPage);
    std::uint32_t verifyIdx=0,verifyPage=0;
    if (!restoredIdx || !restoredPage || !r.Get(idxPtr+0x274,verifyIdx) ||
        !r.Get(pagePtr+0x240,verifyPage) || verifyIdx!=oldIdx || verifyPage!=oldPage) {
        error="Palico loadout: game function was called but cursor restoration failed"; return false;
    }
    error.clear(); return true;
}
bool CaptureSaveFields(const ApplyBackend& b, const ApplyContext& c, SaveOriginal& out) {
    const std::size_t off[8] = {kDeploy, kXp, kGadgetXp, kGadgetXp+4, kGadgetXp+8,
                                kGadgetXp+12, kGadgetXp+16, kGadgetXp+20};
    std::uint32_t v[8]{};
    for (int i=0;i<8;++i) if (!b.reader.Get(c.slot+off[i], v[i])) return false;
    out.slot=c.slot; out.slotIndex=c.slotIndex;
    out.deploy=v[0]; out.xp=v[1];
    for (int i=0;i<6;++i) out.gadgetXp[i]=v[2+i];
    out.valid=true; return true;
}
Result RestoreWithBackend(const SaveOriginal& o, const std::filesystem::path& directory, const ApplyBackend& b) {
    Result result;
    auto fail=[&](const char* e){ result.error=e; return result; };
    if (!o.valid) return fail("Palico restore has no captured original values");
    if (!b.reader.read || !b.context || !b.write || !b.tryLock || !b.unlock || !b.backup)
        return fail("Incomplete Palico transaction backend");
    ApplyContext c,after;
    if (!b.context(b.user,c) || c.slot < 0x10000 || c.slotIndex<0 || c.slotIndex>2)
        return fail("Palico restore requires base Idle and a valid save slot");
    if (o.slot != c.slot || o.slotIndex != c.slotIndex)
        return fail("Palico restore belongs to another save slot; nothing written");
    std::vector<unsigned char> snapshot;
    try { snapshot.resize(kPalicoSlotSize); }
    catch (...) { return fail("Unable to allocate full-slot backup snapshot"); }
    if (!b.reader.Bytes(c.slot,snapshot.data(),snapshot.size()) || !b.context(b.user,after) || !Same(c,after))
        return fail("Cannot read a stable full save slot before restore");
    Patch patches[8]{}; int count=0;
    auto add=[&](std::size_t offset, std::uint32_t value) {
        auto before=Word(snapshot,offset);
        if (before!=value) patches[count++]={offset,before,value};
    };
    add(kDeploy,(Word(snapshot,kDeploy) & ~0x10u) | (o.deploy & 0x10u)); add(kXp,o.xp);
    for (int i=0;i<6;++i) add(kGadgetXp+std::size_t(i)*4, o.gadgetXp[i]);
    if (!count) { result.ok=true; return result; }
    if (!b.backup(b.user,c,snapshot.data(),snapshot.size(),directory,result.backupPath))
        return fail("Full-slot durable backup failed; no Palico data restored");
    if (!b.tryLock(b.user,c.slot+8)) return fail("Save slot lock is busy or unavailable; no Palico data restored");
    const char* error=Commit(b,c,snapshot,patches,count,result.changed);
    b.unlock(b.user,c.slot+8);
    if (error) return fail(error);
    result.ok=true; return result;
}
Result ApplyWithBackend(const Options& o, const std::filesystem::path& directory, const ApplyBackend& b) {
    Result result;
    auto fail=[&](const char* e) { result.error=e; return result; };
    if (o.deploy < -1 || o.deploy > 1 || o.level < 0 || o.level > 50 ||
        o.gadget < -1 || o.gadget > 5 || o.gadgetLevel < 0 || o.gadgetLevel > 15 ||
        o.loadout < 0 || o.loadout > 24)
        return fail("Palico options are outside their supported ranges");
    if (o.deploy==-1 && !o.level && o.gadget==-1 && !o.gadgetLevel && !o.loadout) { result.ok=true; return result; }
    if (!b.reader.read || !b.context || !b.write || !b.tryLock || !b.unlock || !b.backup)
        return fail("Incomplete Palico transaction backend");
    ApplyContext c,after;
    if (!b.context(b.user,c) || c.slot < 0x10000 || c.slotIndex<0 || c.slotIndex>2)
        return fail("Palico apply requires positively observed base Idle and a valid save slot");
    std::vector<unsigned char> snapshot;
    try { snapshot.resize(kPalicoSlotSize); }
    catch (...) { return fail("Unable to allocate full-slot backup snapshot"); }
    if (!b.reader.Bytes(c.slot,snapshot.data(),snapshot.size()) || !b.context(b.user,after) || !Same(c,after))
        return fail("Cannot read a stable full save slot before backup");
    Patch patches[3]{}; int count=0;
    auto add=[&](std::size_t offset, std::uint32_t value) {
        auto before=Word(snapshot,offset);
        if (before!=value) patches[count++]={offset,before,value};
    };
    // The set's gadget level must land on the gadget the selected loadout
    // equips, not the currently equipped one. The saved loadout record stores
    // its gadget at +0x13b680 (DLL 9e00), one 0x130-byte record per loadout.
    int gadget = static_cast<std::int32_t>(Word(snapshot, kGadget));
    if (o.loadout > 0)
        gadget = static_cast<std::int32_t>(Word(snapshot, 0x13b680 + std::size_t(o.loadout - 1) * 0x130));
    if (o.gadget!=-1 && o.gadget!=gadget)
        return fail("Gadget selection blocked: slot+0x2d4 getter confirmed, but game setter/cache synchronization is not verified");
    if (o.deploy!=-1) {
        auto flags=Word(snapshot,kDeploy);
        add(kDeploy,o.deploy ? flags & ~0x10u : flags | 0x10u);
    }
    for (int which=0;which<2;++which) {
        const int level=which ? o.gadgetLevel : o.level;
        if (!level) continue;
        if (which && (gadget<0 || gadget>5)) return fail("Current equipped gadget is invalid; refusing gadget XP write");
        int costs[200]{}, n=200, xp=0;
        if (!ReadExperienceTable(b.reader,b.exe,which ? gadget : -1,costs,n) ||
            n < (which ? 15 : 50) || !ExperienceForLevel(costs,n,level,xp))
            return fail("Palico per-level XP resource table is unavailable or invalid");
        add(which ? kGadgetXp+std::size_t(gadget)*4 : kXp,static_cast<std::uint32_t>(xp));
    }
    if (!count) { result.ok=true; return result; }
    if (!b.backup(b.user,c,snapshot.data(),snapshot.size(),directory,result.backupPath))
        return fail("Full-slot durable backup failed; no Palico data written");
    if (!b.tryLock(b.user,c.slot+8)) return fail("Save slot lock is busy or unavailable; no Palico data written");
    const char* error=Commit(b,c,snapshot,patches,count,result.changed);
    b.unlock(b.user,c.slot+8);
    if (error) return fail(error); // Construct diagnostics only AFTER releasing lock.
    result.ok=true; return result;
}

#ifdef _WIN32
namespace {
constexpr std::uintptr_t kExe=0x140000000;
bool NativeRead(std::uintptr_t at, void* to, std::size_t n, void*) {
    SIZE_T got=0;
    return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<LPCVOID>(at),to,n,&got) && got==n;
}
bool NativeWrite(void*,std::uintptr_t at,const void* from,std::size_t n) {
    // Do not change page protections or write code. Only confirmed writable ints.
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(at),&m,sizeof(m)) || m.State!=MEM_COMMIT ||
        (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
        !(m.Protect&(PAGE_READWRITE|PAGE_WRITECOPY)) ||
        at+n>reinterpret_cast<std::uintptr_t>(m.BaseAddress)+m.RegionSize) return false;
    SIZE_T wrote=0;
    return WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<LPVOID>(at),from,n,&wrote) && wrote==n;
}
bool NativeContext(void*,ApplyContext& c) {
    Reader r{NativeRead,nullptr}; std::uintptr_t base=0; int state=0; unsigned loading=1;
    return ReadSaveSlot(r,kExe,c.slot,c.slotIndex) &&
        r.Get(kExe+0x500ed30,c.quest) && c.quest && r.Get(c.quest+0x38,state) && state==1 &&
        r.Get(kExe+0x51c4640,base) && base && r.Get(base+0x278,c.ui) && c.ui &&
        r.Get(c.ui+0x20,c.loading) && c.loading && r.Get(c.loading+0x1d04,loading) && loading==0;
}
bool NativeLock(void*,std::uintptr_t address) {
    // DLL IAT (own PE parser, not guessed): ce050 EnterCriticalSection,
    // ce058 LeaveCriticalSection; a5da/a6b8 use slot+8.
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(address),&m,sizeof(m)) || m.State!=MEM_COMMIT ||
        (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) || !(m.Protect&PAGE_READWRITE) ||
        address+sizeof(CRITICAL_SECTION)>reinterpret_cast<std::uintptr_t>(m.BaseAddress)+m.RegionSize) return false;
#if defined(_MSC_VER)
    __try { return TryEnterCriticalSection(reinterpret_cast<LPCRITICAL_SECTION>(address))!=FALSE; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return TryEnterCriticalSection(reinterpret_cast<LPCRITICAL_SECTION>(address))!=FALSE;
#endif
}
void NativeUnlock(void*,std::uintptr_t a) { LeaveCriticalSection(reinterpret_cast<LPCRITICAL_SECTION>(a)); }
bool WriteAll(HANDLE file,const void* data,std::size_t n) {
    DWORD wrote=0;
    return n<=MAXDWORD && WriteFile(file,data,static_cast<DWORD>(n),&wrote,nullptr) && wrote==n;
}
bool NativeBackup(void*,const ApplyContext& c,const void* bytes,std::size_t n,
                  const std::filesystem::path& directory,std::filesystem::path& out) {
    // Explicit little-endian format (Win64): magic[16], version u32,
    // headerBytes u32, slotIndex u32, slotBytes u32, slotAddress u64,
    // FILETIME u64, followed by exactly 0x26cc00 raw real-slot bytes.
    struct Header {
        char magic[16]; std::uint32_t version,headerBytes,slotIndex,slotBytes;
        std::uint64_t address,time;
    };
    static_assert(sizeof(Header)==48,"Backup format layout");
    if (n!=kPalicoSlotSize || directory.empty()) return false;
    try {
        std::error_code ec;
        std::filesystem::create_directories(directory,ec);
        if (ec) return false;
        FILETIME ft{}; GetSystemTimeAsFileTime(&ft);
        const std::uint64_t time=(std::uint64_t(ft.dwHighDateTime)<<32)|ft.dwLowDateTime;
        Header h{}; std::memcpy(h.magic,"BIQ-PALICO-SLOT",15);
        h.version=1; h.headerBytes=sizeof(h); h.slotIndex=c.slotIndex;
        h.slotBytes=static_cast<std::uint32_t>(n); h.address=c.slot; h.time=time;
        static volatile LONG serial=0;
        for (int attempt=0;attempt<32;++attempt) {
            auto path=directory/(L"palico-slot-"+std::to_wstring(c.slotIndex)+L"-"+
                std::to_wstring(time)+L"-"+std::to_wstring(GetCurrentProcessId())+L"-"+
                std::to_wstring(InterlockedIncrement(&serial))+L".biqbak");
            // Allocate the result path before opening the handle; no throwing
            // operations are permitted between CreateFile and CloseHandle.
            out=path;
            HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if (f==INVALID_HANDLE_VALUE) {
                const DWORD error=GetLastError(); out.clear();
                if (error==ERROR_FILE_EXISTS) continue;
                return false;
            }
            // Retain even a partial failed backup for diagnosis; never overwrite.
            const bool saved=WriteAll(f,&h,sizeof(h)) && WriteAll(f,bytes,n) && FlushFileBuffers(f);
            const bool closed=CloseHandle(f)!=FALSE;
            return saved && closed;
        }
    } catch (...) { return false; }
    return false;
}
}
#endif
Result Apply(const Options& o,const std::filesystem::path& directory) {
#ifdef _WIN32
    static_assert(sizeof(CRITICAL_SECTION)==40,"Only Win64 game layout is supported");
    ApplyBackend b; b.reader={NativeRead,nullptr}; b.exe=kExe;
    b.context=NativeContext; b.write=NativeWrite; b.tryLock=NativeLock;
    b.unlock=NativeUnlock; b.backup=NativeBackup;
    return ApplyWithBackend(o,directory,b);
#else
    (void)o; (void)directory;
    Result r; r.error="Palico apply is supported only on Win64"; return r;
#endif
}
bool EndCharacterSession() {
    const bool pending = g_saveOriginal.valid;
    g_saveOriginal = {};
    return pending;
}
bool AtBaseIdle() {
#ifdef _WIN32
    ApplyContext c;
    return NativeContext(nullptr,c) && c.slot >= 0x10000;
#else
    return false;
#endif
}
bool CaptureOriginal(std::string& error) {
#ifdef _WIN32
    if (g_saveOriginal.valid) {
        ApplyContext current;
        if (!NativeContext(nullptr,current) || current.slot!=g_saveOriginal.slot ||
            current.slotIndex!=g_saveOriginal.slotIndex) {
            error="Palico snapshot belongs to another save slot; restore it on its original character first";
            return false;
        }
        error.clear(); return true;
    }
    ApplyBackend b; b.reader={NativeRead,nullptr}; b.exe=kExe;
    b.context=NativeContext; b.write=NativeWrite; b.tryLock=NativeLock;
    b.unlock=NativeUnlock; b.backup=NativeBackup;
    ApplyContext c;
    if (!NativeContext(nullptr,c) || c.slot < 0x10000 || c.slotIndex<0 || c.slotIndex>2) {
        error="Palico capture requires base Idle and a valid save slot"; return false;
    }
    SaveOriginal o;
    if (!CaptureSaveFields(b,c,o)) { error="Palico capture could not read the save-slot fields"; return false; }
    g_saveOriginal=o; error.clear(); return true;
#else
    error="Palico capture is supported only on Win64"; return false;
#endif
}
bool RestoreOriginal(const std::filesystem::path& backupDirectory, std::string& error) {
#ifdef _WIN32
    if (!g_saveOriginal.valid) { error.clear(); return true; }
    ApplyBackend b; b.reader={NativeRead,nullptr}; b.exe=kExe;
    b.context=NativeContext; b.write=NativeWrite; b.tryLock=NativeLock;
    b.unlock=NativeUnlock; b.backup=NativeBackup;
    const auto r=RestoreWithBackend(g_saveOriginal, backupDirectory, b);
    if (!r.ok) { error=r.error; return false; }
    g_saveOriginal.valid=false; error.clear(); return true;
#else
    error="Palico restore is supported only on Win64"; return false;
#endif
}
bool ApplyLoadoutOnly(int loadout,std::string& error) {
#ifdef _WIN32
    if (loadout<1 || loadout>24) { error="Palico loadout: the set number must be 1..24"; return false; }
    if (!AtBaseIdle()) { error="Palico loadout: base and idle are required; nothing applied"; return false; }
    Reader reader{NativeRead,nullptr};
    return ApplyLoadoutGame(reader,kExe,loadout,error);
#else
    (void)loadout; (void)error; return false;
#endif
}
} // namespace palico
