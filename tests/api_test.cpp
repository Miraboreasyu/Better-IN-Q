#include "api.h"
#include "betterinq_api.h"
#include "config.h"
#include "game.h"
#include "log.h"
#include "settings_transfer.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace inq {
// Minimal stub for api_test (settings/whitelist/IPC tests do not hook the game)
int TakeEnteredQuest() { return 0; }
} // namespace inq

namespace {

int g_fail = 0;

#define CHECK(cond, fmt, ...)                                                      \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::printf("FAIL: " fmt "\n", ##__VA_ARGS__);                         \
            ++g_fail;                                                              \
        }                                                                          \
    } while (0)

std::wstring TempPath(const wchar_t* name)
{
    wchar_t dir[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}

bool WriteBytes(const std::wstring& path, const void* data, DWORD len)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    const bool ok = ::WriteFile(h, data, len, &wr, nullptr) && wr == len;
    ::CloseHandle(h);
    return ok;
}

bool ReadTextFile(const std::wstring& path, std::string& out)
{
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > (1 << 22)) {
        ::CloseHandle(h);
        return false;
    }
    out.assign(static_cast<std::size_t>(sz.QuadPart), '\0');
    DWORD rd = 0;
    const bool ok = out.empty() || (::ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &rd, nullptr) && rd == out.size());
    ::CloseHandle(h);
    return ok;
}

std::wstring SelfExePath()
{
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    return std::wstring(buf, n);
}

std::wstring Kernel32Path()
{
    HMODULE k = ::GetModuleHandleW(L"kernel32.dll");
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = ::GetModuleFileNameW(k, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    return std::wstring(buf, n);
}

} // namespace

int main()
{
    const std::wstring logPath = TempPath(L"biq_api_test.log");
    LogInit(logPath);

    // 1. Real SHA-256 via bcrypt.dll ("abc" NIST test vector)
    {
        const std::wstring f = TempPath(L"biq_sha_abc.bin");
        CHECK(WriteBytes(f, "abc", 3), "write sha test file");
        std::string hex;
        CHECK(api::ComputeFileSha256Hex(f, hex), "ComputeFileSha256Hex succeeds");
        CHECK(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "SHA256('abc') mismatch: %s", hex.c_str());
        ::DeleteFileW(f.c_str());
    }

    // 2. Whitelist matching: exact, shadow-copy glob, bare sha256, file:sha256, unsafe glob guard
    {
        const std::string shaA = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        const std::string shaB = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
        const std::string shaC = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        const std::string wl = "Auto_Quest_Start.dll,thk.*.dll,mhw_rng.dll:" + shaA + ",sha256:" + shaB;

        std::string matched;
        CHECK(api::MatchWhitelist(wl, "auto_quest_start.dll", "anything", &matched), "exact case-insensitive filename match");
        CHECK(api::MatchWhitelist(wl, "thk.3.12345.dll", "", &matched), "mhw_rng shadow-copy prefix glob match");
        CHECK(api::MatchWhitelist(wl, "MHW_RNG.DLL", shaA, &matched), "pinned filename:sha256 match");
        CHECK(!api::MatchWhitelist(wl, "mhw_rng.dll", shaC, &matched), "pinned filename rejects mismatched sha256");
        CHECK(api::MatchWhitelist(wl, "arbitrary_shadow_copy.dll", shaB, &matched), "sha256-only rule matches shadow copy");
        CHECK(!api::MatchWhitelist(wl, "evil.dll", "1111111111111111111111111111111111111111111111111111111111111111"),
              "unlisted dll denied");

        // Unsafe unpinned wildcards (<2 literal prefix chars before '*') must be rejected
        CHECK(!api::MatchWhitelist("*", "any.dll", shaA), "bare '*' rejected");
        CHECK(!api::MatchWhitelist("*.dll", "any.dll", shaA), "'*.dll' rejected");
        CHECK(!api::MatchWhitelist("a*.dll", "abc.dll", shaA), "'a*.dll' (<2 prefix chars) rejected");
        CHECK(!api::MatchWhitelist("?hk.dll", "thk.dll", shaA), "unpinned '?' rejected");
    }

    // 3. Acquire denied logging + session lifecycle + _ReturnAddress cross-module check
    {
        api::testing::ClearAll();
        const std::wstring exePath = SelfExePath();
        const HMODULE exeMod = ::GetModuleHandleW(nullptr);
        std::string exeSha;
        CHECK(api::ComputeFileSha256Hex(exePath, exeSha), "self exe sha256 computed");

        BetterINQ_Session sess = 0;
        BetterINQ_ApiV1 tbl{};

        api::Configure(false, "api_test.exe");
        CHECK(api::testing::AcquireForModulePath(exeMod, exePath, BETTERINQ_API_VERSION_1, "probe-disabled", &sess, &tbl)
                  == BETTERINQ_ERR_DISABLED,
              "Acquire rejected when disabled");

        api::Configure(true, "other.dll");
        CHECK(api::testing::AcquireForModulePath(exeMod, exePath, BETTERINQ_API_VERSION_1, "probe-unlisted", &sess, &tbl)
                  == BETTERINQ_ERR_NOT_WHITELISTED,
              "Acquire rejected when not in whitelist");

        std::string logText;
        CHECK(ReadTextFile(logPath, logText), "read diagnostic log");
        CHECK(logText.find("DENIED (disabled)") != std::string::npos, "disabled attempt logged");
        CHECK(logText.find("DENIED (not whitelisted)") != std::string::npos, "not-whitelisted attempt logged");
        CHECK(logText.find(exeSha) != std::string::npos && logText.find("api_test.exe") != std::string::npos,
              "diagnostic log includes caller filename + SHA256 for INI onboarding");

        // Bad version check
        CHECK(api::testing::AcquireForModulePath(exeMod, exePath, 99u, "badver", &sess, &tbl)
                  == BETTERINQ_ERR_BAD_VERSION,
              "bad API version rejected");

        // Cross-module _ReturnAddress verification: bind a session to kernel32, then call from api_test.exe
        const HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
        const std::wstring k32Path = Kernel32Path();
        api::Configure(true, "kernel32.dll,api_test.exe:" + exeSha);
        BetterINQ_Session k32Sess = 0;
        BetterINQ_ApiV1 k32Tbl{};
        CHECK(api::testing::AcquireForModulePath(k32, k32Path, BETTERINQ_API_VERSION_1, "k32", &k32Sess, &k32Tbl) == BETTERINQ_OK,
              "acquire for kernel32 succeeds in harness");
        BetterINQ_Status st{};
        CHECK(k32Tbl.GetStatus(k32Sess, &st) == BETTERINQ_ERR_INVALID_SESSION,
              "_ReturnAddress detects caller module mismatch (api_test.exe != kernel32.dll)");

        // Real valid session bound to api_test.exe
        CHECK(api::testing::AcquireForModulePath(exeMod, exePath, BETTERINQ_API_VERSION_1, "runner", &sess, &tbl) == BETTERINQ_OK &&
              sess != 0 && tbl.version == BETTERINQ_API_VERSION_1,
              "valid pinned acquire succeeds");

        // Publish snapshot and test read APIs via real _ReturnAddress()
        Config effCfg;
        effCfg.activeSet = 2;
        effCfg.questId = 66845;
        effCfg.food = 5;
        effCfg.reset = true;
        LoadoutSet ls; ls.id = 2; ls.name = "Fatalis"; ls.equip = "10"; ls.item = "#3"; ls.linkRadial = true; ls.food = 5; ls.quest = 66845;
        effCfg.sets.push_back(ls);
        FoodPreset fp; fp.name = "ATK L"; fp.hp = 5; fp.stamina = 2; fp.attack = 3; fp.skill[0] = 9;
        effCfg.foods[5] = fp;
        std::int32_t s2d[81]{};
        s2d[3] = 17;
        inq::LiveContext live{};
        live.stage = 1; live.loading = 0; live.restartStage = 0; live.enteredQuest = 66845;
        api::PublishSnapshot(true, true, true, true, 1, live, effCfg, true, s2d);

        CHECK(tbl.GetStatus(sess, &st) == BETTERINQ_OK &&
              st.ready == 1 && st.enabled == 1 && st.characterLinked == 1 &&
              st.hasUnsavedExternalChanges == 1 && st.activeSet == 2 && st.questId == 66845 && st.stage == 1,
              "GetStatus returns published snapshot");

        BetterINQ_SetInfo setsBuf[4]{};
        std::uint32_t cnt = 0;
        CHECK(tbl.ListSets(sess, setsBuf, 4, &cnt) == BETTERINQ_OK && cnt == 1 &&
              setsBuf[0].id == 2 && setsBuf[0].equip == 10 && setsBuf[0].itemStore == 3 && setsBuf[0].linkRadial == 1,
              "ListSets returns loadout metadata");

        BetterINQ_FoodInfo foodBuf[4]{};
        CHECK(tbl.ListFoods(sess, foodBuf, 4, &cnt) == BETTERINQ_OK && cnt == 1 &&
              foodBuf[0].id == 5 && foodBuf[0].attack == 3 && foodBuf[0].skills[0] == 9,
              "ListFoods returns meal metadata");

        std::int32_t disp = 0;
        CHECK(tbl.ResolveItemStore(sess, 3, &disp) == BETTERINQ_OK && disp == 17,
              "ResolveItemStore returns snapshot mapping #3 -> display 17");

        // Command queue + completion status + rate limiter
        std::uint64_t reqId = 0;
        BetterINQ_ConfigPatch patch{};
        patch.structSize = sizeof(patch);
        patch.fieldMask = BETTERINQ_PATCH_ACTIVE_SET | BETTERINQ_PATCH_QUEST_ID;
        patch.activeSet = 2;
        patch.questId = 66845;
        CHECK(tbl.PatchSessionConfig(sess, &patch, &reqId) == BETTERINQ_OK && reqId != 0, "enqueue PatchSessionConfig");

        BetterINQ_CommandStatus cstat{};
        CHECK(tbl.GetCommandStatus(sess, reqId, &cstat) == BETTERINQ_OK && cstat.state == BETTERINQ_CMD_QUEUED,
              "command initially QUEUED");
        auto pending = api::TakePendingCommands();
        CHECK(pending.size() == 1 && pending[0].requestId == reqId, "TakePendingCommands yields PatchSessionConfig");
        api::CompleteCommand(reqId, BETTERINQ_OK);
        CHECK(tbl.GetCommandStatus(sess, reqId, &cstat) == BETTERINQ_OK &&
              cstat.state == BETTERINQ_CMD_COMPLETED && cstat.result == BETTERINQ_OK,
              "GetCommandStatus observes COMPLETED");

        // Burst > 20 control calls within 1000ms triggers rate limit (and queue full checks cleanly before 32)
        int rateLimited = 0;
        for (int i = 0; i < 25; ++i) {
            std::uint64_t tmpId = 0;
            const auto rc = tbl.CancelPending(sess, &tmpId);
            if (rc == BETTERINQ_ERR_RATE_LIMITED) {
                ++rateLimited;
                break;
            }
        }
        (void)api::TakePendingCommands();
        CHECK(rateLimited > 0, "rate limit triggers after burst");

        // Configure removing caller immediately revokes active session
        api::Configure(true, "someone_else.dll");
        CHECK(tbl.GetStatus(sess, &st) == BETTERINQ_ERR_INVALID_SESSION, "Configure revokes removed whitelist caller");

        // Re-acquire and explicit Release invalidates token
        api::Configure(true, "api_test.exe:" + exeSha);
        CHECK(api::testing::AcquireForModulePath(exeMod, exePath, BETTERINQ_API_VERSION_1, "runner2", &sess, &tbl) == BETTERINQ_OK,
              "re-acquire succeeds");
        CHECK(tbl.Release(sess) == BETTERINQ_OK, "Release succeeds");
        CHECK(tbl.GetStatus(sess, &st) == BETTERINQ_ERR_INVALID_SESSION, "released session rejected");
    }

    // 4. ExternalSessionOverrides isolation vs persisted INI + settings_transfer local [Api] preservation
    {
        Config persisted;
        persisted.questId = 101;
        persisted.activeSet = -1;
        persisted.food = -1;
        persisted.apiEnabled = true;
        persisted.apiWhitelist = "auto_quest_start.dll";

        api::ExternalSessionOverrides ov;
        ov.hasActiveSet = true; ov.activeSet = 7;
        ov.hasQuestId   = true; ov.questId = 66845;
        ov.hasReset     = true; ov.reset = true;
        const Config eff = ov.ApplyTo(persisted);
        CHECK(eff.activeSet == 7 && eff.questId == 66845 && eff.reset, "session overrides alter effective Config");
        CHECK(persisted.activeSet == -1 && persisted.questId == 101 && !persisted.reset,
              "session overrides do NOT mutate persisted baseline Config");

        ov.CommitTo(persisted);
        ov.Clear();
        CHECK(!ov.Any() && persisted.activeSet == 7 && persisted.questId == 66845 && persisted.reset,
              "CommitTo writes overrides only when explicitly requested");

        // Test settings_transfer::ImportFile preserves local machine [Api] section
        const std::wstring baseP  = TempPath(L"biq_imp_base.ini");
        const std::wstring slot1P = TempPath(L"biq_imp_s1.ini");
        const std::wstring slot2P = TempPath(L"biq_imp_s2.ini");
        const std::wstring slot3P = TempPath(L"biq_imp_s3.ini");
        const std::wstring bakDir = TempPath(L"biq_imp_bak");
        const std::wstring pkgP   = TempPath(L"biq_imp_pkg.biq");
        ::CreateDirectoryW(bakDir.c_str(), nullptr);
        ::DeleteFileW(pkgP.c_str());

        settings_transfer::Storage stg;
        stg.globalPath  = baseP;
        stg.slotPaths[0] = slot1P;
        stg.slotPaths[1] = slot2P;
        stg.slotPaths[2] = slot3P;
        stg.currentSlot = 1;

        Config live;
        live.apiEnabled = true;
        live.apiWhitelist = "auto_quest_start.dll";
        SaveSplit(baseP, slot1P, live);
        SaveSplit(baseP, slot2P, live);
        SaveSplit(baseP, slot3P, live);

        std::string err;
        CHECK(settings_transfer::ExportFile(stg, live, pkgP, err), "ExportFile succeeds: %s", err.c_str());

        // Change local whitelist to pinned rule, then import the old backup -> local whitelist must survive!
        live.apiEnabled = true;
        live.apiWhitelist = "local_only.dll";
        std::wstring recovery;
        CHECK(settings_transfer::ImportFile(stg, pkgP, bakDir, live, recovery, err),
              "ImportFile succeeds: %s", err.c_str());
        CHECK(live.apiEnabled && live.apiWhitelist == "local_only.dll",
              "ImportFile preserves live local [Api] whitelist");

        Config diskCheck;
        CHECK(LoadSplit(baseP, slot1P, diskCheck, err) &&
              diskCheck.apiEnabled && diskCheck.apiWhitelist == "local_only.dll",
              "ImportFile writes local [Api] whitelist back to disk INI");

        ::DeleteFileW(baseP.c_str());
        ::DeleteFileW(slot1P.c_str());
        ::DeleteFileW(slot2P.c_str());
        ::DeleteFileW(slot3P.c_str());
        ::DeleteFileW(pkgP.c_str());
        if (!recovery.empty()) ::DeleteFileW(recovery.c_str());
    }

    // 5. Hardening checks: game::Readable overflow & game::Writable PAGE_READONLY guard
    {
        void* roPage = ::VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        CHECK(roPage != nullptr, "VirtualAlloc test page");
        if (roPage) {
            DWORD oldProt = 0;
            CHECK(::VirtualProtect(roPage, 4096, PAGE_READONLY, &oldProt), "VirtualProtect PAGE_READONLY");
            const auto addr = reinterpret_cast<std::uintptr_t>(roPage);
            CHECK(game::Readable(addr, 4), "game::Readable allows PAGE_READONLY");
            CHECK(!game::Writable(addr, 4), "game::Writable rejects PAGE_READONLY");
            CHECK(!game::Wr<std::uint32_t>(addr, 0x12345678u), "game::Wr refuses writing PAGE_READONLY");
            ::VirtualFree(roPage, 0, MEM_RELEASE);
        }
        CHECK(!game::Readable(static_cast<std::uintptr_t>(-4), 16), "game::Readable rejects wrap-around");
    }

    api::testing::ClearAll();
    ::DeleteFileW(logPath.c_str());
    std::printf("\n%s (%d failures)\n", g_fail ? "FAIL" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
