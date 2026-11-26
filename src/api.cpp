#include "api.h"

#include "gamedata.h"
#include "log.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace api {
namespace {

using NTSTATUS = LONG;
using BCRYPT_ALG_HANDLE = PVOID;
using BCRYPT_HASH_HANDLE = PVOID;

using FnBCryptOpenAlgorithmProvider = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE*, LPCWSTR, LPCWSTR, ULONG);
using FnBCryptCloseAlgorithmProvider = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, ULONG);
using FnBCryptCreateHash = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
using FnBCryptHashData = NTSTATUS(WINAPI*)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
using FnBCryptFinishHash = NTSTATUS(WINAPI*)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
using FnBCryptDestroyHash = NTSTATUS(WINAPI*)(BCRYPT_HASH_HANDLE);
using FnBCryptGenRandom = NTSTATUS(WINAPI*)(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, ULONG);

struct BcryptApi {
    bool                           attempted = false;
    bool                           ready = false;
    FnBCryptOpenAlgorithmProvider  openAlg = nullptr;
    FnBCryptCloseAlgorithmProvider closeAlg = nullptr;
    FnBCryptCreateHash             createHash = nullptr;
    FnBCryptHashData               hashData = nullptr;
    FnBCryptFinishHash             finishHash = nullptr;
    FnBCryptDestroyHash            destroyHash = nullptr;
    FnBCryptGenRandom              genRandom = nullptr;
};

BcryptApi g_bcrypt{};

bool EnsureBcryptLoaded()
{
    if (g_bcrypt.attempted) return g_bcrypt.ready;
    g_bcrypt.attempted = true;
    HMODULE mod = ::LoadLibraryW(L"bcrypt.dll");
    if (!mod) return false;
    g_bcrypt.openAlg     = reinterpret_cast<FnBCryptOpenAlgorithmProvider>(::GetProcAddress(mod, "BCryptOpenAlgorithmProvider"));
    g_bcrypt.closeAlg    = reinterpret_cast<FnBCryptCloseAlgorithmProvider>(::GetProcAddress(mod, "BCryptCloseAlgorithmProvider"));
    g_bcrypt.createHash  = reinterpret_cast<FnBCryptCreateHash>(::GetProcAddress(mod, "BCryptCreateHash"));
    g_bcrypt.hashData    = reinterpret_cast<FnBCryptHashData>(::GetProcAddress(mod, "BCryptHashData"));
    g_bcrypt.finishHash  = reinterpret_cast<FnBCryptFinishHash>(::GetProcAddress(mod, "BCryptFinishHash"));
    g_bcrypt.destroyHash = reinterpret_cast<FnBCryptDestroyHash>(::GetProcAddress(mod, "BCryptDestroyHash"));
    g_bcrypt.genRandom   = reinterpret_cast<FnBCryptGenRandom>(::GetProcAddress(mod, "BCryptGenRandom"));
    g_bcrypt.ready = g_bcrypt.openAlg && g_bcrypt.closeAlg && g_bcrypt.createHash &&
                     g_bcrypt.hashData && g_bcrypt.finishHash && g_bcrypt.destroyHash;
    return g_bcrypt.ready;
}

std::string LowerAscii(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
}

std::string TrimAscii(const std::string& s)
{
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

bool IsHex64(const std::string& s)
{
    if (s.size() != 64U) return false;
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}

std::string BaseNameUtf8(const std::wstring& fullPath)
{
    const std::size_t slash = fullPath.find_last_of(L"\\/");
    const std::wstring base = (slash == std::wstring::npos) ? fullPath : fullPath.substr(slash + 1);
    return ToUtf8(base);
}

bool WildcardMatchLower(const std::string& pat, const std::string& str)
{
    std::size_t pi = 0, si = 0, star = std::string::npos, match = 0;
    while (si < str.size()) {
        if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == str[si])) {
            ++pi;
            ++si;
        } else if (pi < pat.size() && pat[pi] == '*') {
            star = pi++;
            match = si;
        } else if (star != std::string::npos) {
            pi = star + 1;
            si = ++match;
        } else {
            return false;
        }
    }
    while (pi < pat.size() && pat[pi] == '*') ++pi;
    return pi == pat.size();
}

bool HasSafeUnpinnedWildcardPrefix(const std::string& patLower)
{
    const std::size_t star = patLower.find('*');
    if (star == std::string::npos) return patLower.find('?') == std::string::npos;
    if (patLower.find('?') != std::string::npos) return false;
    std::size_t literalPrefix = 0;
    for (std::size_t i = 0; i < star; ++i) {
        const unsigned char c = static_cast<unsigned char>(patLower[i]);
        if (std::isalnum(c)) ++literalPrefix;
    }
    return literalPrefix >= 2U;
}

struct HashCacheEntry {
    std::wstring pathLower;
    std::uint64_t fileSize = 0;
    std::uint64_t writeTime = 0;
    std::string sha256;
};

struct SessionSlot {
    bool              active = false;
    BetterINQ_Session token = 0;
    HMODULE           callerMod = nullptr;
    std::wstring      fullPath;
    std::string       fileName;
    std::string       sha256;
    std::string       callerTag;
    ULONGLONG         lastUsedTick = 0;
    ULONGLONG         rateWindowStart = 0;
    std::uint32_t     rateCount = 0;
};

constexpr std::size_t   kMaxSessions = 32U;
constexpr std::size_t   kMaxPendingCommands = 32U;
constexpr std::size_t   kCommandRingSize = 64U;
constexpr std::uint32_t kRateLimitMaxControlsPerSec = 20U;

std::mutex                  g_sessionMx;
HMODULE                     g_selfModule = nullptr;
bool                        g_apiEnabled = false;
std::string                 g_whitelistSpec;
SessionSlot                 g_sessions[kMaxSessions]{};
std::vector<HashCacheEntry> g_hashCache;
std::atomic<std::uint64_t>  g_rngCounter{1ULL};

struct PublishedState {
    BetterINQ_Status            status{};
    std::vector<BetterINQ_SetInfo>  sets;
    std::vector<BetterINQ_FoodInfo> foods;
    std::int32_t                itemStoreToDisplay[81]{};
};

std::mutex     g_stateMx;
PublishedState g_pubState;

struct CommandRecord {
    std::uint64_t     requestId = 0;
    BetterINQ_Session session = 0;
    std::uint32_t     state = BETTERINQ_CMD_UNKNOWN;
    std::int32_t      result = BETTERINQ_OK;
};

std::mutex                  g_cmdMx;
std::uint64_t               g_nextReqId = 1ULL;
std::vector<PendingCommand> g_cmdQueue;
CommandRecord               g_cmdRing[kCommandRingSize]{};

std::uint64_t GenerateSessionTokenLocked()
{
    std::uint64_t tok = 0;
    if (EnsureBcryptLoaded() && g_bcrypt.genRandom) {
        (void)g_bcrypt.genRandom(nullptr, reinterpret_cast<PUCHAR>(&tok), sizeof(tok), 0x00000002U);
    }
    if (tok == 0) {
        LARGE_INTEGER qpc{};
        ::QueryPerformanceCounter(&qpc);
        std::uint64_t x = static_cast<std::uint64_t>(qpc.QuadPart) ^
                          (static_cast<std::uint64_t>(::GetCurrentProcessId()) << 32) ^
                          (static_cast<std::uint64_t>(::GetCurrentThreadId()) << 16) ^
                          (g_rngCounter.fetch_add(1ULL, std::memory_order_relaxed) * 0x9E3779B97F4A7C15ULL);
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        x = x ^ (x >> 31);
        tok = x ? x : 1ULL;
    }
    for (const auto& s : g_sessions) {
        if (s.active && s.token == tok) {
            tok ^= 0x55AA55AA55AA55AAULL;
            if (tok == 0) tok = 1ULL;
        }
    }
    return tok;
}

HMODULE ModuleFromReturnAddress(const void* addr) noexcept
{
    if (!addr) return nullptr;
    HMODULE mod = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(addr), &mod)) {
        return nullptr;
    }
    return mod;
}

std::wstring ModulePathOf(HMODULE mod)
{
    if (!mod) return {};
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = ::GetModuleFileNameW(mod, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    if (!n || n >= sizeof(buf) / sizeof(buf[0])) return {};
    return std::wstring(buf, n);
}

bool CachedSha256Locked(const std::wstring& fullPath, std::string& outHex)
{
    outHex.clear();
    if (fullPath.empty()) return false;

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    std::uint64_t fileSize = 0;
    std::uint64_t writeTime = 0;
    const bool haveAttr = ::GetFileAttributesExW(fullPath.c_str(), GetFileExInfoStandard, &fad) != FALSE;
    if (haveAttr) {
        fileSize = (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        writeTime = (static_cast<std::uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                    fad.ftLastWriteTime.dwLowDateTime;
    }

    std::wstring pathLower = fullPath;
    for (wchar_t& wc : pathLower) wc = static_cast<wchar_t>(::towlower(wc));

    if (haveAttr) {
        for (const auto& e : g_hashCache) {
            if (e.pathLower == pathLower && e.fileSize == fileSize && e.writeTime == writeTime) {
                outHex = e.sha256;
                return true;
            }
        }
    }

    std::string computed;
    if (!ComputeFileSha256Hex(fullPath, computed)) return false;

    if (haveAttr) {
        if (g_hashCache.size() >= 64U) g_hashCache.erase(g_hashCache.begin());
        g_hashCache.push_back(HashCacheEntry{pathLower, fileSize, writeTime, computed});
    }
    outHex = computed;
    return true;
}

std::string SanitizeTag(const char* tag)
{
    if (!tag) return {};
    std::string out;
    for (std::size_t i = 0; i < 64 && tag[i] != '\0'; ++i) {
        const unsigned char c = static_cast<unsigned char>(tag[i]);
        if (c >= 0x20U && c != 0x7FU && c != '\r' && c != '\n') out.push_back(static_cast<char>(c));
    }
    return out;
}

template <std::size_t N>
void CopyTrunc(char (&dst)[N], const std::string& src) noexcept
{
    const std::size_t n = (src.size() < N - 1U) ? src.size() : (N - 1U);
    if (n > 0) std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

std::int32_t ValidateSessionLocked(BetterINQ_Session session,
                                   HMODULE callerMod,
                                   bool isControl,
                                   SessionSlot** outSlot)
{
    if (outSlot) *outSlot = nullptr;
    if (!session || !callerMod) return BETTERINQ_ERR_INVALID_SESSION;
    if (!g_apiEnabled) return BETTERINQ_ERR_DISABLED;

    const ULONGLONG now = ::GetTickCount64();
    for (auto& s : g_sessions) {
        if (!s.active || s.token != session) continue;
        if (s.callerMod != callerMod) return BETTERINQ_ERR_INVALID_SESSION;
        s.lastUsedTick = now;
        if (isControl) {
            if (now - s.rateWindowStart >= 1000ULL) {
                s.rateWindowStart = now;
                s.rateCount = 0;
            }
            if (s.rateCount >= kRateLimitMaxControlsPerSec) {
                return BETTERINQ_ERR_RATE_LIMITED;
            }
            ++s.rateCount;
        }
        if (outSlot) *outSlot = &s;
        return BETTERINQ_OK;
    }
    return BETTERINQ_ERR_INVALID_SESSION;
}

std::int32_t EnqueueCommand(BetterINQ_Session session,
                            const void* returnAddress,
                            PendingCommand cmd,
                            std::uint64_t* outRequestId)
{
    if (!outRequestId) return BETTERINQ_ERR_INVALID_ARG;
    *outRequestId = 0;

    const HMODULE callerMod = ModuleFromReturnAddress(returnAddress);
    {
        std::lock_guard<std::mutex> lk(g_sessionMx);
        SessionSlot* slot = nullptr;
        const std::int32_t vr = ValidateSessionLocked(session, callerMod, true, &slot);
        if (vr != BETTERINQ_OK) return vr;
    }

    std::lock_guard<std::mutex> clk(g_cmdMx);
    if (g_cmdQueue.size() >= kMaxPendingCommands) {
        return BETTERINQ_ERR_QUEUE_FULL;
    }
    const std::uint64_t reqId = g_nextReqId++;
    cmd.requestId = reqId;
    cmd.session = session;
    g_cmdQueue.push_back(std::move(cmd));

    CommandRecord& rec = g_cmdRing[reqId % kCommandRingSize];
    rec.requestId = reqId;
    rec.session = session;
    rec.state = BETTERINQ_CMD_QUEUED;
    rec.result = BETTERINQ_PENDING;

    *outRequestId = reqId;
    return BETTERINQ_OK;
}

// ---- ApiV1 thunks (all __declspec(noinline) so _ReturnAddress() identifies the external caller) ----

__declspec(noinline) int32_t Api_Release(BetterINQ_Session session)
{
    const HMODULE callerMod = ModuleFromReturnAddress(_ReturnAddress());
    if (!session || !callerMod) return BETTERINQ_ERR_INVALID_SESSION;

    std::lock_guard<std::mutex> lk(g_sessionMx);
    for (auto& s : g_sessions) {
        if (s.active && s.token == session) {
            if (s.callerMod != callerMod) return BETTERINQ_ERR_INVALID_SESSION;
            Log("api: session released 0x%llX file=\"%s\"",
                static_cast<unsigned long long>(session), s.fileName.c_str());
            s = SessionSlot{};
            return BETTERINQ_OK;
        }
    }
    return BETTERINQ_ERR_INVALID_SESSION;
}

__declspec(noinline) int32_t Api_GetStatus(BetterINQ_Session session, BetterINQ_Status* outStatus)
{
    if (!outStatus || (outStatus->structSize != 0U && outStatus->structSize != sizeof(BetterINQ_Status))) {
        return BETTERINQ_ERR_INVALID_ARG;
    }
    const HMODULE callerMod = ModuleFromReturnAddress(_ReturnAddress());
    {
        std::lock_guard<std::mutex> lk(g_sessionMx);
        const std::int32_t vr = ValidateSessionLocked(session, callerMod, false, nullptr);
        if (vr != BETTERINQ_OK) return vr;
    }
    std::lock_guard<std::mutex> slk(g_stateMx);
    *outStatus = g_pubState.status;
    outStatus->structSize = sizeof(BetterINQ_Status);
    outStatus->apiVersion = BETTERINQ_API_VERSION_1;
    return BETTERINQ_OK;
}

__declspec(noinline) int32_t Api_ListSets(BetterINQ_Session session,
                                          BetterINQ_SetInfo* outSets,
                                          uint32_t capacity,
                                          uint32_t* outCount)
{
    if (!outCount) return BETTERINQ_ERR_INVALID_ARG;
    *outCount = 0;
    if (capacity > 0U && !outSets) return BETTERINQ_ERR_INVALID_ARG;
    const HMODULE callerMod = ModuleFromReturnAddress(_ReturnAddress());
    {
        std::lock_guard<std::mutex> lk(g_sessionMx);
        const std::int32_t vr = ValidateSessionLocked(session, callerMod, false, nullptr);
        if (vr != BETTERINQ_OK) return vr;
    }
    std::lock_guard<std::mutex> slk(g_stateMx);
    const std::uint32_t total = static_cast<std::uint32_t>(g_pubState.sets.size());
    *outCount = total;
    const std::uint32_t toCopy = (capacity < total) ? capacity : total;
    for (std::uint32_t i = 0; i < toCopy; ++i) {
        outSets[i] = g_pubState.sets[i];
        outSets[i].structSize = sizeof(BetterINQ_SetInfo);
    }
    return BETTERINQ_OK;
}

__declspec(noinline) int32_t Api_ListFoods(BetterINQ_Session session,
                                           BetterINQ_FoodInfo* outFoods,
                                           uint32_t capacity,
                                           uint32_t* outCount)
{
    if (!outCount) return BETTERINQ_ERR_INVALID_ARG;
    *outCount = 0;
    if (capacity > 0U && !outFoods) return BETTERINQ_ERR_INVALID_ARG;
    const HMODULE callerMod = ModuleFromReturnAddress(_ReturnAddress());
    {
        std::lock_guard<std::mutex> lk(g_sessionMx);
        const std::int32_t vr = ValidateSessionLocked(session, callerMod, false, nullptr);
        if (vr != BETTERINQ_OK) return vr;
    }
    std::lock_guard<std::mutex> slk(g_stateMx);
    const std::uint32_t total = static_cast<std::uint32_t>(g_pubState.foods.size());
    *outCount = total;
    const std::uint32_t toCopy = (capacity < total) ? capacity : total;
    for (std::uint32_t i = 0; i < toCopy; ++i) {
        outFoods[i] = g_pubState.foods[i];
        outFoods[i].structSize = sizeof(BetterINQ_FoodInfo);
    }
    return BETTERINQ_OK;
}

__declspec(noinline) int32_t Api_ResolveItemStore(BetterINQ_Session session,
                                                  int32_t store,
                                                  int32_t* outDisplay)
{
    if (!outDisplay) return BETTERINQ_ERR_INVALID_ARG;
    *outDisplay = 0;
    if (store < 1 || store > 80) return BETTERINQ_ERR_INVALID_ARG;

    const HMODULE callerMod = ModuleFromReturnAddress(_ReturnAddress());
    {
        std::lock_guard<std::mutex> lk(g_sessionMx);
        const std::int32_t vr = ValidateSessionLocked(session, callerMod, false, nullptr);
        if (vr != BETTERINQ_OK) return vr;
    }

    std::lock_guard<std::mutex> slk(g_stateMx);
    if (!g_pubState.status.characterLinked) return BETTERINQ_ERR_NOT_LINKED;
    const std::int32_t disp = g_pubState.itemStoreToDisplay[store];
    if (disp <= 0) return BETTERINQ_ERR_NOT_FOUND;
    *outDisplay = disp;
    return BETTERINQ_OK;
}

__declspec(noinline) int32_t Api_GetCommandStatus(BetterINQ_Session session,
                                                  uint64_t requestId,
                                                  BetterINQ_CommandStatus* outStatus)
{
    if (!outStatus || (outStatus->structSize != 0U && outStatus->structSize != sizeof(BetterINQ_CommandStatus))) {
        return BETTERINQ_ERR_INVALID_ARG;
    }
    const HMODULE callerMod = ModuleFromReturnAddress(_ReturnAddress());
    {
        std::lock_guard<std::mutex> lk(g_sessionMx);
        const std::int32_t vr = ValidateSessionLocked(session, callerMod, false, nullptr);
        if (vr != BETTERINQ_OK) return vr;
    }

    outStatus->structSize = sizeof(BetterINQ_CommandStatus);
    outStatus->state = BETTERINQ_CMD_UNKNOWN;
    outStatus->result = BETTERINQ_ERR_NOT_FOUND;
    outStatus->reserved = 0;
    if (requestId == 0ULL) return BETTERINQ_ERR_INVALID_ARG;

    std::lock_guard<std::mutex> clk(g_cmdMx);
    const CommandRecord& rec = g_cmdRing[requestId % kCommandRingSize];
    if (rec.requestId == requestId && rec.session == session) {
        outStatus->state = rec.state;
        outStatus->result = rec.result;
        return BETTERINQ_OK;
    }
    return BETTERINQ_ERR_NOT_FOUND;
}

__declspec(noinline) int32_t Api_PatchSessionConfig(BetterINQ_Session session,
                                                    const BetterINQ_ConfigPatch* patch,
                                                    uint64_t* outRequestId)
{
    if (!patch || patch->structSize != sizeof(BetterINQ_ConfigPatch)) return BETTERINQ_ERR_INVALID_ARG;
    PendingCommand cmd{};
    cmd.kind = CommandKind::PatchSessionConfig;
    cmd.patch = *patch;
    return EnqueueCommand(session, _ReturnAddress(), std::move(cmd), outRequestId);
}

__declspec(noinline) int32_t Api_ResetSessionOverrides(BetterINQ_Session session,
                                                       uint64_t* outRequestId)
{
    PendingCommand cmd{};
    cmd.kind = CommandKind::ResetSessionOverrides;
    return EnqueueCommand(session, _ReturnAddress(), std::move(cmd), outRequestId);
}

__declspec(noinline) int32_t Api_PersistConfig(BetterINQ_Session session,
                                               uint64_t* outRequestId)
{
    PendingCommand cmd{};
    cmd.kind = CommandKind::PersistConfig;
    return EnqueueCommand(session, _ReturnAddress(), std::move(cmd), outRequestId);
}

__declspec(noinline) int32_t Api_RequestRestart(BetterINQ_Session session,
                                                const BetterINQ_RestartParams* paramsOrNull,
                                                uint64_t* outRequestId)
{
    PendingCommand cmd{};
    cmd.kind = CommandKind::RequestRestart;
    if (paramsOrNull) {
        if (paramsOrNull->structSize != sizeof(BetterINQ_RestartParams)) return BETTERINQ_ERR_INVALID_ARG;
        cmd.hasRestartParams = true;
        cmd.restart = *paramsOrNull;
    }
    return EnqueueCommand(session, _ReturnAddress(), std::move(cmd), outRequestId);
}

__declspec(noinline) int32_t Api_RequestResetOnly(BetterINQ_Session session,
                                                  uint64_t* outRequestId)
{
    PendingCommand cmd{};
    cmd.kind = CommandKind::RequestResetOnly;
    return EnqueueCommand(session, _ReturnAddress(), std::move(cmd), outRequestId);
}

__declspec(noinline) int32_t Api_RequestApplyActiveSet(BetterINQ_Session session,
                                                       uint64_t* outRequestId)
{
    PendingCommand cmd{};
    cmd.kind = CommandKind::RequestApplyActiveSet;
    return EnqueueCommand(session, _ReturnAddress(), std::move(cmd), outRequestId);
}

__declspec(noinline) int32_t Api_RequestApplyLoadout(BetterINQ_Session session,
                                                     const BetterINQ_LoadoutParams* params,
                                                     uint64_t* outRequestId)
{
    if (!params || params->structSize != sizeof(BetterINQ_LoadoutParams)) return BETTERINQ_ERR_INVALID_ARG;
    PendingCommand cmd{};
    cmd.kind = CommandKind::RequestApplyLoadout;
    cmd.loadout = *params;
    return EnqueueCommand(session, _ReturnAddress(), std::move(cmd), outRequestId);
}

__declspec(noinline) int32_t Api_RequestQueueSet(BetterINQ_Session session,
                                                 int32_t setId,
                                                 uint64_t* outRequestId)
{
    if (setId <= 0) return BETTERINQ_ERR_INVALID_ARG;
    PendingCommand cmd{};
    cmd.kind = CommandKind::RequestQueueSet;
    cmd.setId = setId;
    return EnqueueCommand(session, _ReturnAddress(), std::move(cmd), outRequestId);
}

__declspec(noinline) int32_t Api_RequestPouchRefill(BetterINQ_Session session,
                                                    uint64_t* outRequestId)
{
    PendingCommand cmd{};
    cmd.kind = CommandKind::RequestPouchRefill;
    return EnqueueCommand(session, _ReturnAddress(), std::move(cmd), outRequestId);
}

__declspec(noinline) int32_t Api_CancelPending(BetterINQ_Session session,
                                               uint64_t* outRequestId)
{
    PendingCommand cmd{};
    cmd.kind = CommandKind::CancelPending;
    return EnqueueCommand(session, _ReturnAddress(), std::move(cmd), outRequestId);
}

void FillApiTableV1(BetterINQ_ApiV1* out) noexcept
{
    out->structSize            = sizeof(BetterINQ_ApiV1);
    out->version               = BETTERINQ_API_VERSION_1;
    out->Release               = &Api_Release;
    out->GetStatus             = &Api_GetStatus;
    out->ListSets              = &Api_ListSets;
    out->ListFoods             = &Api_ListFoods;
    out->ResolveItemStore      = &Api_ResolveItemStore;
    out->GetCommandStatus      = &Api_GetCommandStatus;
    out->PatchSessionConfig    = &Api_PatchSessionConfig;
    out->ResetSessionOverrides = &Api_ResetSessionOverrides;
    out->PersistConfig         = &Api_PersistConfig;
    out->RequestRestart        = &Api_RequestRestart;
    out->RequestResetOnly      = &Api_RequestResetOnly;
    out->RequestApplyActiveSet = &Api_RequestApplyActiveSet;
    out->RequestApplyLoadout   = &Api_RequestApplyLoadout;
    out->RequestQueueSet       = &Api_RequestQueueSet;
    out->RequestPouchRefill    = &Api_RequestPouchRefill;
    out->CancelPending         = &Api_CancelPending;
}

std::int32_t AcquireInternal(HMODULE callerMod,
                             const std::wstring& fullPath,
                             std::uint32_t apiVersion,
                             const char* callerTag,
                             BetterINQ_Session* outSession,
                             BetterINQ_ApiV1* outApi)
{
    if (!outSession || !outApi) return BETTERINQ_ERR_INVALID_ARG;
    *outSession = 0;
    std::memset(outApi, 0, sizeof(*outApi));

    if (apiVersion != BETTERINQ_API_VERSION_1) return BETTERINQ_ERR_BAD_VERSION;
    if (!callerMod || fullPath.empty()) return BETTERINQ_ERR_INVALID_ARG;

    const std::string tag = SanitizeTag(callerTag);
    const std::string fileName = BaseNameUtf8(fullPath);
    const std::string pathUtf8 = ToUtf8(fullPath);

    std::lock_guard<std::mutex> lk(g_sessionMx);
    if (g_selfModule && callerMod == g_selfModule) {
        return BETTERINQ_ERR_NOT_WHITELISTED;
    }

    std::string sha256;
    const bool hashed = CachedSha256Locked(fullPath, sha256);
    const char* shaLog = hashed ? sha256.c_str() : "(hash-unavailable)";

    if (!g_apiEnabled) {
        Log("api: acquire DENIED (disabled) tag=\"%s\" file=\"%s\" sha256=%s path=\"%s\" "
            "-> set [Api] Enabled=1 and add \"%s:%s\" (or \"%s\") to Whitelist",
            tag.c_str(), fileName.c_str(), shaLog, pathUtf8.c_str(),
            fileName.c_str(), shaLog, fileName.c_str());
        return BETTERINQ_ERR_DISABLED;
    }

    std::string matchedRule;
    if (!MatchWhitelist(g_whitelistSpec, fileName, sha256, &matchedRule)) {
        Log("api: acquire DENIED (not whitelisted) tag=\"%s\" file=\"%s\" sha256=%s path=\"%s\" "
            "-> add \"%s:%s\" (or \"%s\") to [Api] Whitelist",
            tag.c_str(), fileName.c_str(), shaLog, pathUtf8.c_str(),
            fileName.c_str(), shaLog, fileName.c_str());
        return BETTERINQ_ERR_NOT_WHITELISTED;
    }

    const ULONGLONG now = ::GetTickCount64();
    int freeIdx = -1;
    int deadIdx = -1;
    int oldestIdx = 0;
    ULONGLONG oldestTick = ~0ULL;

    for (int i = 0; i < static_cast<int>(kMaxSessions); ++i) {
        if (!g_sessions[i].active) {
            if (freeIdx < 0) freeIdx = i;
            continue;
        }
        wchar_t probe[4]{};
        if (::GetModuleFileNameW(g_sessions[i].callerMod, probe, 4) == 0 && deadIdx < 0) {
            deadIdx = i;
        }
        if (g_sessions[i].lastUsedTick <= oldestTick) {
            oldestTick = g_sessions[i].lastUsedTick;
            oldestIdx = i;
        }
    }

    const int chosen = (freeIdx >= 0) ? freeIdx : ((deadIdx >= 0) ? deadIdx : oldestIdx);
    const BetterINQ_Session token = GenerateSessionTokenLocked();
    SessionSlot& slot = g_sessions[chosen];
    slot = SessionSlot{};
    slot.active          = true;
    slot.token           = token;
    slot.callerMod       = callerMod;
    slot.fullPath        = fullPath;
    slot.fileName        = fileName;
    slot.sha256          = sha256;
    slot.callerTag       = tag;
    slot.lastUsedTick    = now;
    slot.rateWindowStart = now;
    slot.rateCount       = 0;

    *outSession = token;
    FillApiTableV1(outApi);
    Log("api: acquire OK session=0x%llX rule=\"%s\" tag=\"%s\" file=\"%s\" sha256=%s path=\"%s\"",
        static_cast<unsigned long long>(token), matchedRule.c_str(),
        tag.c_str(), fileName.c_str(), shaLog, pathUtf8.c_str());
    return BETTERINQ_OK;
}

} // namespace

bool ComputeFileSha256Hex(const std::wstring& path, std::string& outHex)
{
    outHex.clear();
    if (path.empty() || !EnsureBcryptLoaded()) return false;

    HANDLE hFile = ::CreateFileW(path.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    auto cleanup = [&]() noexcept {
        if (hHash) { g_bcrypt.destroyHash(hHash); hHash = nullptr; }
        if (hAlg)  { g_bcrypt.closeAlg(hAlg, 0);  hAlg = nullptr; }
        if (hFile != INVALID_HANDLE_VALUE) { ::CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; }
    };

    if (g_bcrypt.openAlg(&hAlg, L"SHA256", nullptr, 0) < 0 ||
        g_bcrypt.createHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) < 0) {
        cleanup();
        return false;
    }

    std::array<std::uint8_t, 65536> buf{};
    for (;;) {
        DWORD rd = 0;
        if (!::ReadFile(hFile, buf.data(), static_cast<DWORD>(buf.size()), &rd, nullptr)) {
            cleanup();
            return false;
        }
        if (rd == 0) break;
        if (g_bcrypt.hashData(hHash, buf.data(), rd, 0) < 0) {
            cleanup();
            return false;
        }
    }

    std::array<std::uint8_t, 32> digest{};
    if (g_bcrypt.finishHash(hHash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        cleanup();
        return false;
    }
    cleanup();

    static constexpr char kHex[] = "0123456789abcdef";
    outHex.resize(64U);
    for (std::size_t i = 0; i < digest.size(); ++i) {
        outHex[2U * i]      = kHex[(digest[i] >> 4) & 0x0FU];
        outHex[2U * i + 1U] = kHex[digest[i] & 0x0FU];
    }
    return true;
}

bool MatchWhitelist(const std::string& whitelistSpec,
                    const std::string& fileNameUtf8,
                    const std::string& sha256Hex,
                    std::string* outMatchedRule)
{
    if (outMatchedRule) outMatchedRule->clear();
    if (whitelistSpec.empty()) return false;

    const std::string fileLower = LowerAscii(fileNameUtf8);
    const std::string shaLower  = LowerAscii(sha256Hex);

    std::size_t pos = 0;
    while (pos <= whitelistSpec.size()) {
        std::size_t end = whitelistSpec.find_first_of(",;|", pos);
        if (end == std::string::npos) end = whitelistSpec.size();
        const std::string rule = TrimAscii(whitelistSpec.substr(pos, end - pos));
        pos = end + 1U;
        if (rule.empty()) continue;

        const std::string ruleLower = LowerAscii(rule);
        if (IsHex64(ruleLower)) {
            if (!shaLower.empty() && ruleLower == shaLower) {
                if (outMatchedRule) *outMatchedRule = rule;
                return true;
            }
            continue;
        }

        if (ruleLower.compare(0, 7, "sha256:") == 0) {
            const std::string hexPart = ruleLower.substr(7);
            if (IsHex64(hexPart) && !shaLower.empty() && hexPart == shaLower) {
                if (outMatchedRule) *outMatchedRule = rule;
                return true;
            }
            continue;
        }

        const std::size_t colon = ruleLower.rfind(':');
        if (colon != std::string::npos && IsHex64(ruleLower.substr(colon + 1U))) {
            const std::string namePat = TrimAscii(ruleLower.substr(0, colon));
            const std::string hexPart = ruleLower.substr(colon + 1U);
            const bool nameOk = namePat.empty() ||
                                (!fileLower.empty() &&
                                 HasSafeUnpinnedWildcardPrefix(namePat) &&
                                 WildcardMatchLower(namePat, fileLower));
            if (nameOk && !shaLower.empty() && hexPart == shaLower) {
                if (outMatchedRule) *outMatchedRule = rule;
                return true;
            }
            continue;
        }

        if (!HasSafeUnpinnedWildcardPrefix(ruleLower)) {
            continue;
        }
        if (!fileLower.empty() && WildcardMatchLower(ruleLower, fileLower)) {
            if (outMatchedRule) *outMatchedRule = rule;
            return true;
        }
    }
    return false;
}

void Init(HMODULE selfModule)
{
    std::lock_guard<std::mutex> lk(g_sessionMx);
    g_selfModule = selfModule;
}

void Shutdown()
{
    {
        std::lock_guard<std::mutex> lk(g_sessionMx);
        g_apiEnabled = false;
        g_whitelistSpec.clear();
        for (auto& s : g_sessions) s = SessionSlot{};
        g_hashCache.clear();
    }
    {
        std::lock_guard<std::mutex> clk(g_cmdMx);
        g_cmdQueue.clear();
        for (auto& r : g_cmdRing) r = CommandRecord{};
    }
    {
        std::lock_guard<std::mutex> slk(g_stateMx);
        g_pubState = PublishedState{};
    }
}

void Configure(bool enabled, const std::string& whitelistSpec)
{
    std::lock_guard<std::mutex> lk(g_sessionMx);
    g_apiEnabled = enabled;
    g_whitelistSpec = whitelistSpec;
    for (auto& s : g_sessions) {
        if (!s.active) continue;
        if (!g_apiEnabled || !MatchWhitelist(g_whitelistSpec, s.fileName, s.sha256, nullptr)) {
            Log("api: revoked session 0x%llX file=\"%s\" after policy change",
                static_cast<unsigned long long>(s.token), s.fileName.c_str());
            s = SessionSlot{};
        }
    }
}

std::int32_t AcquireFromCaller(const void* returnAddress,
                               std::uint32_t apiVersion,
                               const char* callerTag,
                               BetterINQ_Session* outSession,
                               BetterINQ_ApiV1* outApi)
{
    const HMODULE callerMod = ModuleFromReturnAddress(returnAddress);
    const std::wstring fullPath = ModulePathOf(callerMod);
    return AcquireInternal(callerMod, fullPath, apiVersion, callerTag, outSession, outApi);
}

void PublishSnapshot(bool ready,
                     bool enabled,
                     bool characterLinked,
                     bool startupReady,
                     int saveSlot,
                     const inq::LiveContext& live,
                     const Config& effectiveCfg,
                     bool hasUnsavedExternalChanges,
                     const std::int32_t itemStoreToDisplay[81])
{
    PublishedState next;
    next.status.structSize                = sizeof(BetterINQ_Status);
    next.status.apiVersion                = BETTERINQ_API_VERSION_1;
    next.status.ready                     = ready ? 1 : 0;
    next.status.enabled                   = enabled ? 1 : 0;
    next.status.characterLinked           = characterLinked ? 1 : 0;
    next.status.startupReady              = startupReady ? 1 : 0;
    next.status.loading                   = live.loading;
    next.status.hasUnsavedExternalChanges = hasUnsavedExternalChanges ? 1 : 0;
    next.status.saveSlot                  = saveSlot;
    next.status.stage                     = live.stage;
    next.status.restartStage              = live.restartStage;
    next.status.activeSet                 = effectiveCfg.activeSet;
    next.status.questId                   = effectiveCfg.questId;
    next.status.food                      = effectiveCfg.food;
    next.status.enteredQuest              = live.enteredQuest;
    next.status.reset                     = effectiveCfg.reset ? 1 : 0;
    next.status.acceptOnly                = effectiveCfg.acceptOnly ? 1 : 0;
    next.status.wyvern                    = effectiveCfg.wyvern ? 1 : 0;

    next.sets.reserve(effectiveCfg.sets.size());
    for (const auto& s : effectiveCfg.sets) {
        BetterINQ_SetInfo info{};
        info.structSize = sizeof(BetterINQ_SetInfo);
        info.id = s.id;
        CopyTrunc(info.nameUtf8, s.name);
        if (!s.equip.empty()) {
            const int eq = std::atoi(s.equip.c_str());
            if (eq >= 1 && eq <= 224) info.equip = eq;
        }
        if (!s.item.empty() && s.item[0] == '#') {
            const int st = std::atoi(s.item.c_str() + 1);
            if (st >= 1 && st <= 80) info.itemStore = st;
        } else if (!s.item.empty() && s.item[0] >= '0' && s.item[0] <= '9') {
            const int st = std::atoi(s.item.c_str());
            if (st >= 1 && st <= 80) info.itemStore = st;
        }
        info.food       = s.food;
        info.quest      = s.quest;
        info.linkRadial = s.linkRadial ? 1 : 0;
        next.sets.push_back(info);
    }

    next.foods.reserve(effectiveCfg.foods.size());
    for (const auto& kv : effectiveCfg.foods) {
        BetterINQ_FoodInfo info{};
        info.structSize = sizeof(BetterINQ_FoodInfo);
        info.id = kv.first;
        CopyTrunc(info.nameUtf8, kv.second.name);
        info.hp        = kv.second.hp;
        info.stamina   = kv.second.stamina;
        info.attack    = kv.second.attack;
        info.defense   = kv.second.defense;
        info.elemRes   = kv.second.elemRes;
        info.skills[0] = kv.second.skill[0];
        info.skills[1] = kv.second.skill[1];
        info.skills[2] = kv.second.skill[2];
        info.deleted   = kv.second.deleted ? 1 : 0;
        next.foods.push_back(info);
    }

    if (itemStoreToDisplay) {
        std::memcpy(next.itemStoreToDisplay, itemStoreToDisplay, sizeof(next.itemStoreToDisplay));
    }

    std::lock_guard<std::mutex> slk(g_stateMx);
    g_pubState = std::move(next);
}

std::vector<PendingCommand> TakePendingCommands()
{
    std::lock_guard<std::mutex> clk(g_cmdMx);
    std::vector<PendingCommand> out;
    out.swap(g_cmdQueue);
    return out;
}

void CompleteCommand(std::uint64_t requestId, std::int32_t result)
{
    if (requestId == 0ULL) return;
    std::lock_guard<std::mutex> clk(g_cmdMx);
    CommandRecord& rec = g_cmdRing[requestId % kCommandRingSize];
    if (rec.requestId == requestId) {
        rec.state = (result == BETTERINQ_OK) ? BETTERINQ_CMD_COMPLETED : BETTERINQ_CMD_FAILED;
        rec.result = result;
    }
}

namespace testing {

void SetSelfModule(HMODULE selfMod)
{
    std::lock_guard<std::mutex> lk(g_sessionMx);
    g_selfModule = selfMod;
}

std::int32_t AcquireForModulePath(HMODULE callerMod,
                                  const std::wstring& fullPath,
                                  std::uint32_t apiVersion,
                                  const char* callerTag,
                                  BetterINQ_Session* outSession,
                                  BetterINQ_ApiV1* outApi)
{
    return AcquireInternal(callerMod, fullPath, apiVersion, callerTag, outSession, outApi);
}

void ClearAll()
{
    Shutdown();
    std::lock_guard<std::mutex> lk(g_sessionMx);
    g_selfModule = nullptr;
}

} // namespace testing

} // namespace api
