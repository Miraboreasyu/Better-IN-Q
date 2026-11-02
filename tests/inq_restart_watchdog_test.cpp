// White-box state test: compile instead of src/inq.cpp; link config.cpp,
// gamedata.cpp, i18n.cpp, hook.cpp (same dependencies as inq_equip_apply_test).
#include "../src/inq.cpp"
#include <cstdio>
#include <thread>
#include <vector>

static int failures = 0, chats = 0;
void Log(const char*, ...) {}
void Chat(const std::string&) { ++chats; }
#define CHECK(c, label) do { if (!(c)) { std::printf("FAIL: %s\n", label); ++failures; } } while (0)

template<class T> static void Put(std::uintptr_t a, T v)
{
    std::memcpy(reinterpret_cast<void*>(a), &v, sizeof(v));
}
static bool MapGlobal(std::uintptr_t a)
{
    return ::VirtualAlloc(reinterpret_cast<void*>(a & ~std::uintptr_t(0xFFFF)),
                          0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

int main()
{
    using namespace inq;
    Config cfg;
    cfg.enabled = true;
    if (!MapGlobal(game::pUiBase) || !MapGlobal(game::pSave)) return 2;
    std::vector<unsigned char> base(0x300), ui(0x400), load(0x1D10), slot(0x140500);
    auto At = [](std::vector<unsigned char>& v) { return reinterpret_cast<std::uintptr_t>(v.data()); };
    Put(game::pUiBase, At(base)); Put(At(base) + 0x278, At(ui));
    Put(At(ui) + 0x20, At(load));
    g_ready = true;
    SetEnabled(true);

    g.optReset = 1; g.optAcceptOnly = 1; g.optWyvern = 1;
    g.questId = 101; g.food = 7;
    g_enteredSeen = 101;
    g.restart = 3; g.optResetOnly = 1; g.equipLoadout = 2; g.itemLoadout = 5;
    g_pendingSetItem = 4; g_pendingEquipApply.store(8);
    g_pendingLink = true; g_hasPendingLink = true;
    g_savedRadial = 2; g_savedRadialSlot = At(slot);
    Put<std::uint8_t>(At(slot) + 0x140415, 1);
    CHECK(!CheckRestartWatchdog(cfg, 0), "first sample starts timer");
    CHECK(!CheckRestartWatchdog(cfg, 11999), "no premature timeout");
    CHECK(CheckRestartWatchdog(cfg, 12000), "stalled stage recovers");
    CHECK(Enabled() && g_gateEnabled == 1, "recovery reopens previous runtime state");
    CHECK(g.restart == 0 && g.optResetOnly == 0 && g.equipLoadout == 0 && g.itemLoadout == 0 &&
          g_pendingSetItem == 0 && g_pendingEquipApply.load() == 0 && !g_hasPendingLink,
          "recovery clears all pending work");
    CHECK(game::Rd<std::uint8_t>(At(slot) + 0x140415) == 2 && g_savedRadial == -1,
          "recovery restores saved radial byte");
    CHECK(g.optReset == 1 && g.optAcceptOnly == 1 && g.optWyvern == 1 &&
          g.questId == 101 && g.food == 7 && g_enteredSeen == 101 && cfg.enabled,
          "effective options, entered baseline and persistent enabled preserved");
    CHECK(chats == 1, "one recovery notification");

    // Simulate a callback admitted before the entrance closes. It publishes a
    // late stage, just as QuestLoad or the BaseLoad2 call gate can do. Cancellation
    // must drain BOTH writers before its final clear, never revive restart.
    g.restart = 2;
    g_inflight.fetch_add(1);
    ::InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_gateInflight));
    std::thread writers([] {
        while (Enabled() || g_gateEnabled != 0) std::this_thread::yield();
        g.restart = 3;
        g_inflight.fetch_sub(1, std::memory_order_release);
        g.restart = 4;
        ::InterlockedDecrement(reinterpret_cast<volatile LONG*>(&g_gateInflight));
    });
    CancelPending();
    writers.join();
    CHECK(g.restart == 0 && Enabled() && g_gateEnabled == 1,
          "safe cancellation drains late C++ and gate writes before clearing/reopening");

    // A timeout sample is only tentative: an admitted hook can make progress
    // before the drain completes. Do not cancel that newly progressed request.
    g.restart = 2;
    CHECK(!CheckRestartWatchdog(cfg, 20000), "restart starts fresh timer after cancel");
    g_inflight.fetch_add(1);
    std::thread progress([] {
        while (Enabled()) std::this_thread::yield();
        g.restart = 3;
        g_inflight.fetch_sub(1, std::memory_order_release);
    });
    CHECK(!CheckRestartWatchdog(cfg, 32000), "post-drain stage progress vetoes recovery");
    progress.join();
    CHECK(g.restart == 3 && Enabled() && chats == 1, "progress remains live without notification");

    // Loading may begin while an admitted hook is draining, even if restart
    // itself has not changed. Recheck the real loading flag before clearing.
    CancelPending();
    g.restart = 3;
    CHECK(!CheckRestartWatchdog(cfg, 33000), "prime loading-transition race");
    g_inflight.fetch_add(1);
    std::thread loading([&] {
        while (Enabled()) std::this_thread::yield();
        Put<std::uint32_t>(At(load) + 0x1D04, 1);
        g_inflight.fetch_sub(1, std::memory_order_release);
    });
    CHECK(!CheckRestartWatchdog(cfg, 45000), "post-drain loading vetoes recovery");
    loading.join();
    CHECK(g.restart == 3 && Enabled() && chats == 1, "new disk loading remains live");
    Put<std::uint32_t>(At(load) + 0x1D04, 0);

    // Same-stage completion followed by a new request must not inherit age.
    CancelPending(); RequestRestart(cfg);
    CHECK(g.restart == 1, "restart publishes request");
    CHECK(!CheckRestartWatchdog(cfg, 50000), "new request gets fresh interval");
    CancelPending(); RequestResetOnly();
    CHECK(!CheckRestartWatchdog(cfg, 80000), "reset-only request gets fresh interval");
    CHECK(!CheckRestartWatchdog(cfg, 91999), "reset-only not prematurely cancelled");

    Put<std::uint32_t>(At(load) + 0x1D04, 1);
    CHECK(!CheckRestartWatchdog(cfg, 1000000), "long loading never times out");
    Put<std::uintptr_t>(At(ui) + 0x20, 0);
    CHECK(!CheckRestartWatchdog(cfg, 2000000), "missing load pointer is unknown, not idle");
    Put<std::uintptr_t>(At(ui) + 0x20, 0x20000);
    CHECK(!CheckRestartWatchdog(cfg, 3000000), "unreadable load flag is unknown, not idle");
    Put<std::uintptr_t>(At(ui) + 0x20, At(load));
    Put<std::uint32_t>(At(load) + 0x1D04, 0);
    CHECK(!CheckRestartWatchdog(cfg, 4000000), "idle after unknown starts full fresh interval");
    CHECK(!CheckRestartWatchdog(cfg, 4011999), "unknown elapsed time excluded");

    // Real Tick's early returns must not bypass radial restoration or silently
    // accumulate timeout age across a missing UI chain.
    g_savedRadial = 2; g_savedRadialSlot = At(slot);
    Put<std::uint8_t>(At(slot) + 0x140415, 1);
    Put<std::uintptr_t>(game::pUiBase, 0);
    Tick(cfg);
    CHECK(g_savedRadial == -1 && game::Rd<std::uint8_t>(At(slot) + 0x140415) == 2,
          "Tick restores saved radial even without UiBase");
    Put(game::pUiBase, At(base)); Put<std::uintptr_t>(At(base) + 0x278, 0);
    Tick(cfg);
    CHECK(g.restart == 1 && Enabled(), "missing pui cannot trigger false timeout");
    Put(At(base) + 0x278, At(ui));
    SetEnabled(false);
    CHECK(!CheckRestartWatchdog(cfg, 9000000) && !Enabled() && g_gateEnabled == 0,
          "watchdog never resurrects disabled runtime");
    RequestRestart(cfg); RequestResetOnly();
    CHECK(g.restart == 0, "disabled restart publishers remain inactive");
    CancelPending();
    CHECK(!Enabled() && g_gateEnabled == 0, "cancel preserves dormant runtime");
    SetEnabled(true); RequestRestart(cfg);
    CHECK(!CheckRestartWatchdog(cfg, 10000000), "reenable starts clean timing");
    CHECK(CheckRestartWatchdog(cfg, 10012000), "reenabled stuck request recovers without Tick self-deadlock");
    CHECK(chats == 2, "each separate watchdog recovery keeps its ordinary Chat feedback");
    // Exercise actual Tick timeout entry (not only its helper). No inflight
    // token may survive into CloseAndDrain, otherwise this call would hang.
    RequestRestart(cfg);
    const auto now = ::GetTickCount64();
    if (now >= RestartWatchdog::kTimeoutMs) {
        CHECK(!CheckRestartWatchdog(cfg, now - RestartWatchdog::kTimeoutMs), "prime actual Tick timeout");
        const int previousChats = chats;
        Tick(cfg);
        CHECK(g.restart == 0 && Enabled() && chats == previousChats + 1,
              "actual Tick recovers without draining its own ActiveCall");
    }
    SetEnabled(false);
    std::printf("inq_restart_watchdog_test: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
