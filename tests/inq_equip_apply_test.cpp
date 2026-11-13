// Standalone white-box test: compile this instead of src/inq.cpp, and link
// src/config.cpp src/gamedata.cpp src/i18n.cpp src/hook.cpp (no game process needed).
#include "../src/inq.cpp"

#include <cstdio>
#include <vector>

// Keep diagnostics local; tests must not invoke the game's chat function.
void Log(const char*, ...) {}
static std::vector<std::string> chats;
void Chat(const std::string& text) { chats.push_back(text); }

static int failures = 0;
#define CHECK(cond, label) do { if (!(cond)) { std::printf("FAIL: %s\n", label); ++failures; } } while (0)

template<class T> static void Put(std::uintptr_t address, T value)
{
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
}
static bool MapGlobal(std::uintptr_t address)
{
    const auto base = address & ~std::uintptr_t(0xFFFF);
    return ::VirtualAlloc(reinterpret_cast<void*>(base), 0x10000,
                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != nullptr;
}
static hook::Ctx Context()
{
    hook::Ctx c{};
    c.rax = 0x71D0;
    c.rbx = 0x02A0;
    return c;
}

// Exercise the real Tick loading path: matching a preset is not proof that the
// plugin owns the meal. The player can eat exactly the same meal at the canteen.
static void CheckMealApply(std::uintptr_t load, std::uintptr_t food)
{
    using namespace inq;
    Config cfg;
    cfg.foods[4] = FoodPreset{};
    auto& preset = cfg.foods[4];
    preset.hp = 5; preset.stamina = 2; preset.attack = 3;
    preset.defense = 1; preset.elemRes = 2;
    preset.skill[0] = 1; preset.skill[1] = 2; preset.skill[2] = 3;
    cfg.foods[5] = preset;
    cfg.foods[5].attack = 1;
    cfg.foods[6] = FoodPreset{}; // Explicitly selected neutral meal, not unselected.
    const std::int32_t manual[10] = {12345, 5, 2, 3, 1, 2, 1, 2, 3, 54321};
    auto seed = [&]() { std::memcpy(reinterpret_cast<void*>(food + 0x19A4), manual, sizeof(manual)); };
    auto unchanged = [&]() { return std::memcmp(reinterpret_cast<void*>(food + 0x19A4), manual, sizeof(manual)) == 0; };
    Put<std::uint32_t>(load + 0x1D04, 1);
    CancelPending();

    for (int selected : {-1, 0}) {
        for (int active : {-1, 0, 999}) {
            cfg.food = selected; cfg.activeSet = active;
            seed(); ApplyOptions(cfg); Tick(cfg); Tick(cfg);
            CHECK(unchanged(), "unselected meal and set preserve matching manual meal during repeated loading ticks");
            CHECK(g.equipLoadout == 0 && g_pendingSetItem == 0 && g_pendingEquipApply.load() == 0,
                  "unselected set never queues equipment or item overrides");
            auto c = Context(); OnApplyResetAndEquip(&c);
            CHECK(c.rax == 0x71D0 && c.rbx == 0x02A0, "unselected set leaves equipment hook untouched");
        }
        seed(); Put<std::int32_t>(food + 0x19A8, 4);
        std::int32_t other[10];
        std::memcpy(other, reinterpret_cast<void*>(food + 0x19A4), sizeof(other));
        Tick(cfg);
        CHECK(std::memcmp(other, reinterpret_cast<void*>(food + 0x19A4), sizeof(other)) == 0,
              "unselected meal preserves nonmatching manual meal too");
    }

    LoadoutSet set; set.id = 1;
    cfg.sets.push_back(set); cfg.activeSet = 1;
    for (int inherited : {-1, 0}) {
        cfg.sets[0].food = inherited;
        for (int global : {-1, 0, 4}) {
            cfg.food = global; seed(); ApplyOptions(cfg); QueueSet(cfg.sets[0]);
            CHECK(g.food == global, "set food zero or minus one inherits global selection");
            Tick(cfg);
            CHECK(unchanged(), "inherited unselected or matching selected meal preserves all fields and guards");
        }
    }
    cfg.food = 4; cfg.sets[0].food = 5;
    seed(); ApplyOptions(cfg); QueueSet(cfg.sets[0]); Tick(cfg);
    CHECK(g.food == 5 && game::Rd<std::int32_t>(food + 0x19B0) == 1,
          "explicit set meal still overrides global meal");
    CHECK(game::Rd<std::int32_t>(food + 0x19A4) == manual[0] &&
          game::Rd<std::int32_t>(food + 0x19C8) == manual[9], "selected meal leaves adjacent memory unchanged");

    cfg.activeSet = -1;
    ApplyOptions(cfg);
    CHECK(g.food == 5, "deselecting set during loading preserves prior effective meal");
    cfg.food = -1; ApplyOptions(cfg); // explicit global-food change
    cfg.food = 4;
    seed(); ApplyOptions(cfg); Tick(cfg);
    cfg.food = -1; ApplyOptions(cfg); Tick(cfg);
    CHECK(unchanged(), "deselecting an applied meal does not clear previously written values");
    RequestRestart(cfg);
    CHECK(g.restart == 1 && g.equipLoadout == 0 && g_pendingSetItem == 0,
          "restart with no selected set queues no loadout override");
    Tick(cfg); CHECK(unchanged(), "unselected restart preserves manual meal during loading");
    CancelPending();

    cfg.food = 5; ApplyOptions(cfg); seed();
    Put<std::uint32_t>(load + 0x1D04, 0); Tick(cfg);
    CHECK(unchanged(), "selected meal is not written outside loading");
    Put<std::uint32_t>(load + 0x1D04, 1); Tick(cfg);
    CHECK(game::Rd<std::int32_t>(food + 0x19B0) == 1, "selected global meal still applies during loading");
    cfg.food = 999; ApplyOptions(cfg); seed(); Tick(cfg);
    CHECK(unchanged(), "missing positive meal id does not clear manual meal");
    cfg.food = 6; ApplyOptions(cfg); Tick(cfg);
    const std::int32_t neutral[8] = {0, 0, 0, 0, 0, -1, -1, -1};
    CHECK(std::memcmp(reinterpret_cast<void*>(food + 0x19A8), neutral, sizeof(neutral)) == 0,
          "explicit positive neutral preset can still request no meal");
    Put<std::uint32_t>(load + 0x1D04, 0);
    CancelPending();
}

static int directCalls = 0, nativeCalls = 0, seenIndex = -1, seenMode = -1;
static std::uintptr_t seenOwner = 0;
static void FakeDirect(std::uintptr_t owner, int index, int mode) {
    ++directCalls; seenOwner=owner; seenIndex=index; seenMode=mode;
}
static void FakeNative(const inq::ItemCommand*, void*, std::uintptr_t*) { ++nativeCalls; }

int main()
{
    using namespace inq;
    Config cfg;
    if (!MapGlobal(game::pSave) || !MapGlobal(game::pUiBase) || !MapGlobal(game::pFood)) {
        std::printf("FAIL: could not reserve fixed game global addresses\\n");
        return 1;
    }
    // pQuest and pFood share one 64KiB allocation. Use real mapped pointers,
    // never a fake classifier seam, for request and hook-side safety checks.
    std::vector<unsigned char> save(0xB0), slot(0x140500), base(0x10000), ui(0x400),
        load(0x1D10), equipUi(0x3000), pageUi(0x300), indexUi(0x300), objects(0x2B40),
        food(0x1A00), quest(0x100), itemUi(0x100);
    auto At = [](std::vector<unsigned char>& v) { return reinterpret_cast<std::uintptr_t>(v.data()); };
    Put(game::pSave, At(save)); Put(At(save) + 0xA8, At(slot));
    Put(game::pUiBase, At(base)); Put(At(base) + 0x278, At(ui));
    Put(At(ui) + 0x20, At(load)); Put(At(ui) + 0x310, At(equipUi));
    Put(At(ui) + 0x170, At(itemUi));
    Put(At(ui) + 0x60, At(objects) + 0x80);
    Put(At(equipUi) + 0x2960, At(pageUi)); Put(At(equipUi) + 0x2958, At(indexUi));
    Put(At(objects) + 0x80 + 0x470, At(objects) + 0x5E0);
    Put(game::pFood, At(food)); Put(game::pQuest, At(quest));
    Put<std::uint32_t>(At(quest) + 0x38, 1);
    cfg.enabled = true;
    CHECK(!RequestEquipApply(cfg, "1"), "Dormant refuses requests");
    CHECK(g_pendingEquipApply.load() == 0, "Dormant does not publish work");
    g_enabled.store(true);
    CHECK(!RequestEquipApply(cfg, "1"), "uninstalled hooks refuse requests");
    g_ready = true;
    cfg.enabled = false;
    CHECK(!RequestEquipApply(cfg, "1"), "disabled config refuses requests");
    cfg.enabled = true;
    CHECK(RequestEquipApply(cfg, "1"), "selection queues without restart");
    CHECK(g.restart == 0 && g_pendingEquipApply.load() == 1, "no restart side effect");
    CHECK(RequestEquipApply(cfg, "224"), "last slot accepted");
    CHECK(g_pendingEquipApply.load() == 224, "latest selection wins");
    CHECK(!RequestEquipApply(cfg, "225"), "invalid slot rejected");
    CHECK(g_pendingEquipApply.load() == 0, "invalid selection clears stale request");
    CHECK(RequestEquipApply(cfg, ""), "empty selection cancels");
    cfg.activeSet = 1;
    LoadoutSet set;
    set.id = 1; set.equip = "2"; set.item = "#3"; set.food = 4; set.quest = 101;
    cfg.sets.push_back(set);
    g.food = -1; g.questId = 0;
    cfg.sets[0].item.clear(); // equipment-only request before callable fixtures exist
    CHECK(RequestActiveSetEquipApply(cfg), "active set queues equipment only");
    cfg.sets[0].item = "#3";
    CHECK(g_pendingEquipApply.load() == 2 && g_pendingSetItem == 0 && g.food == -1 && g.questId == 0,
          "item food quest unchanged");
    cfg.activeSet = -1;
    CHECK(RequestActiveSetEquipApply(cfg) && g_pendingEquipApply.load() == 0,
          "unselect cancels pending without unequipping");

    CheckMealApply(At(load), At(food));

    // Names are read from the current save each time, never cached by slot ID.
    const auto nameAt = At(slot) + 0x10CCF4 - 0x100;
    std::memcpy(reinterpret_cast<void*>(nameAt), "Before rename", 14);
    auto names = ListEquipLoadouts();
    CHECK(!names.empty() && names[0].number == 1 && names[0].name == "Before rename", "read original game loadout name");
    std::memset(reinterpret_cast<void*>(nameAt), 0, 0x100);
    std::memcpy(reinterpret_cast<void*>(nameAt), "After rename", 13);
    names = ListEquipLoadouts();
    CHECK(!names.empty() && names[0].number == 1 && names[0].name == "After rename", "reread sees in-game rename without restart or changing stable slot");

    // Hub -> quest TOCTOU: revalidate at the actual game-thread apply point.
    CHECK(RequestEquipApply(cfg, "2"), "hub accepts equipment request");
    Put<std::uint32_t>(At(quest) + 0x38, 2);
    const auto beforeWarning = chats.size();
    auto blocked = Context(); OnApplyResetAndEquip(&blocked);
    CHECK(blocked.rax == 0x71D0 && g_pendingEquipApply.load() == 0,
          "queued hub equipment cannot execute after quest entry");
    CHECK(chats.size() == beforeWarning, "hook never chats directly");
    Tick(cfg);
    CHECK(chats.size() == beforeWarning + 1, "worker emits deferred safety warning");

    cfg.activeSet = 1; cfg.food = -1;
    cfg.foods[4] = FoodPreset{}; cfg.foods[4].attack = 3;
    Put<std::int32_t>(At(food) + 0x19B0, 77);
    cfg.sets[0].linkRadial = true;
    Put<std::uint32_t>(At(slot) + 3 * 0x768 + 0x278, 1);
    for (int i = 0; i < 80; ++i) Put<std::uint8_t>(At(slot) + kItemOrder + i, (std::uint8_t)i);
    Put<std::uint8_t>(At(slot) + 0x140415, 7);
    g.food = -1; g_haveFoodOption = false;
    CHECK(RequestActiveSetEquipApply(cfg), "quest selection accepts item-only set");
    CHECK(g_pendingEquipApply.load() == 0 && g.equipLoadout == 0 &&
          g_pendingSetItem == 3 && g_pendingLink && g.food == -1,
          "quest selection queues items and radial only");
    ApplyOptions(cfg); ApplyOptions(cfg); // reqSave follows selection
    CHECK(g.food == -1, "saving quest active-set selection does not activate set meal");
    Put<std::uint32_t>(At(load) + 0x1D04, 1);
    Tick(cfg);
    CHECK(g_pendingSetItem == 0 && game::Rd<std::uint8_t>(At(slot) + 0x140415) == 7,
          "loading cancels immediate items without applying radial");
    CHECK(game::Rd<std::int32_t>(At(food) + 0x19B0) == 77,
          "selection then save then loading does not apply newly selected set meal");
    CHECK(!RequestActiveSetEquipApply(cfg) && g_pendingSetItem == 0,
          "loading selection cannot publish delayed items");
    Put<std::uint32_t>(At(load) + 0x1D04, 0);
    RequestActiveSetEquipApply(cfg);
    Tick(cfg);
    CHECK(g_pendingSetItem == 0 && game::Rd<std::uint64_t>(At(base) + 0x4F60) == game::kItemLoadoutCmd &&
          game::Rd<std::uint64_t>(At(base) + 0x4F68) == 2 &&
          game::Rd<std::uint8_t>(At(slot) + 0x140415) == 1,
          "item UI command and linked radial apply in quest");
    Tick(cfg);
    CHECK(game::Rd<std::uint8_t>(At(slot) + 0x140415) == 7, "radial restores after item application");
    cfg.food = 5; ApplyOptions(cfg);
    CHECK(g.food == 5, "manual global meal selection remains independent");
    g.food = 4; ApplyOptions(cfg, true);
    CHECK(g.food == 5, "configuration replacement resets old effective food even with unchanged global ID");
    RequestActiveSetEquipApply(cfg); cfg.activeSet = -1; RequestActiveSetEquipApply(cfg);
    CHECK(g_pendingSetItem == 0 && !g_hasPendingLink, "deselect cancels old immediate item work");
    cfg.activeSet = 1; RequestActiveSetEquipApply(cfg);
    cfg.sets[0].item.clear(); RequestActiveSetEquipApply(cfg);
    CHECK(g_pendingSetItem == 0, "set without items replaces old item selection");
    cfg.sets[0].item = "#3";
    for (unsigned stage : {0u, 2u, 4u, 5u, 0xDu, 99u}) {
        Put<std::uint32_t>(At(quest) + 0x38, stage);
        CHECK(!RequestEquipApply(cfg, "2") && g_pendingEquipApply.load() == 0,
              "unknown quest result and expedition states reject immediate equipment");
    }
    Put<std::uintptr_t>(game::pQuest, 0);
    CHECK(!RequestEquipApply(cfg, "2"), "missing quest chain fails closed");
    CHECK(!RequestActiveSetEquipApply(cfg) && g_pendingSetItem == 0 && g_pendingEquipApply.load() == 0,
          "unknown state cannot publish equipment or delayed items");
    Put(game::pQuest, At(quest)); Put<std::uint32_t>(At(quest) + 0x38, 2);
    RequestRestart(cfg);
    CHECK(g.equipLoadout == 2 && g_pendingSetItem == 0 && g.food == 4,
          "in-quest restart retains equipment and meal but never queues an item command");
    const int restartItems = g_pendingSetItem;
    cfg.sets[0].item.clear();
    CHECK(!RequestActiveSetEquipApply(cfg) && g_pendingSetItem == restartItems,
          "panel selection cannot overwrite intentional restart item queue");
    auto restartEquip = Context(); OnApplyResetAndEquip(&restartEquip);
    CHECK(restartEquip.rax == game::kVtEquipApply && g.equipLoadout == 0,
          "original restart equipment applies even in quest");
    CancelPending(); SetEnabled(false); SetEnabled(true);
    RequestActiveSetEquipApply(cfg);
    CHECK(chats.size() == beforeWarning + 1, "safety warning once survives restart cancellation and Dormant");
    CancelPending(); cfg.activeSet = -1;
    Put<std::uint32_t>(At(quest) + 0x38, 1);

    // Hub origin is required; loading and chained resets never masquerade as entry.
    {
        ItemEntryTracker t;
        CHECK(!t.Observe(2,true,false,At(slot),0) && !t.Observe(2,true,false,At(slot),600),
              "enabling in an existing quest does not auto-apply");
        CHECK(!t.Observe(1,true,false,At(slot),700) && !t.Observe(1,true,false,At(slot),1200),
              "stable hub arms one arrival without applying in hub");
        CHECK(!t.Observe(2,false,false,At(slot),1300) && !t.Observe(2,true,false,At(slot),1400),
              "entry waits for readable idle and stability");
        CHECK(t.Observe(2,true,false,At(slot),1900), "hub to quest applies once");
        CHECK(!t.Observe(2,true,false,At(slot),2500), "same quest does not repeat");
        t.Observe(2,false,false,At(slot),2600); t.Observe(2,true,false,At(slot),2700);
        CHECK(!t.Observe(2,true,false,At(slot),3300), "same-quest loading does not rearm");
        t.BeginReset(false,At(slot));
        t.Observe(2,false,true,At(slot),3400); t.Observe(2,true,false,At(slot),3500);
        CHECK(!t.Observe(2,true,false,At(slot),4100), "reset remains suppressed after restart flag clears");
        t.BeginReset(true,At(slot));
        t.Observe(2,false,true,At(slot),4200); t.Observe(2,true,false,At(slot),4300);
        CHECK(t.Observe(2,true,false,At(slot),4900), "hub restart applies only after arrival");
        t.BeginReset(true,At(slot));
        t.Observe(2,true,false,At(slot)+0x26cc00,5000);
        CHECK(!t.Observe(2,true,false,At(slot)+0x26cc00,5600), "character change discards old hub origin");
    }
    // Accepted reset requests synchronously drop pending item/refill work.
    cfg.activeSet=1; cfg.sets[0].item="#3";
    g_pendingSetItem=3; g_refillRequest.store(true); g_refillPlan={At(slot),3,999999};
    RequestResetOnly();
    CHECK(g_pendingSetItem==0 && !g_refillRequest.load() && !g_refillPlan.store,
          "reset-only drops item requests before starting transition");
    Put<std::uint64_t>(At(base)+0x8F60,0);
    Put<std::uint32_t>(At(load)+0x1D04,1);
    Tick(cfg);
    CHECK(game::Rd<std::uint64_t>(At(base)+0x8F60)==0, "reset loading tick never publishes item UI command");
    CancelPending(); Put<std::uint32_t>(At(load)+0x1D04,0); cfg.activeSet=-1;

    RequestEquipApply(cfg, "10");
    auto c = Context(); c.rax = 0;
    OnApplyResetAndEquip(&c);
    CHECK(g_pendingEquipApply.load() == 10, "unmatched UI call leaves request queued");
    Put<std::uint32_t>(At(load) + 0x1D04, 1);
    c = Context(); OnApplyResetAndEquip(&c);
    CHECK(c.rax == 0x71D0 && g_pendingEquipApply.load() == 0, "loading cancels unsafe immediate request without changing registers");
    Put<std::uint32_t>(At(load) + 0x1D04, 0);
    RequestEquipApply(cfg, "10");
    Put<std::uintptr_t>(At(equipUi) + 0x2958, 0);
    c = Context(); OnApplyResetAndEquip(&c);
    CHECK(g_pendingEquipApply.load() == 10, "missing UI chain defers");
    Put(At(equipUi) + 0x2958, At(indexUi));
    c = Context(); OnApplyResetAndEquip(&c);
    CHECK(g_pendingEquipApply.load() == 0 && c.rax == game::kVtEquipApply, "matching game thread call applies without restart");
    CHECK(game::Rd<std::uint32_t>(At(pageUi) + 0x240) == 1 &&
          game::Rd<std::uint32_t>(At(indexUi) + 0x274) == 1 && c.rdx == 1,
          "slot ten translates to page one index one");
    CHECK(g.restart == 0 && g_pendingSetItem == 0, "equipment application has no other action");

    Put<std::uint32_t>(At(slot) + 0x10CCF4, 0xFFFFFFFF);
    RequestEquipApply(cfg, "1");
    c = Context(); OnApplyResetAndEquip(&c);
    CHECK(c.rax == 0x71D0 && g_pendingEquipApply.load() == 0 && (g.message & 1),
          "empty loadout reports invalid without redirecting game call");
    RequestEquipApply(cfg, "2");
    g.restart = 3;
    c = Context(); OnApplyResetAndEquip(&c);
    CHECK(g_pendingEquipApply.load() == 2 && c.rax == 0x71D0,
          "immediate selection never interrupts the restart flow");
    g.restart = 0;
    RequestRestart(cfg);
    CHECK(g_pendingEquipApply.load() == 0 && g.restart == 1,
          "restart supersedes stale immediate selection");
    CancelPending();
    RequestEquipApply(cfg, "2"); CancelPending();
    CHECK(g_pendingEquipApply.load() == 0, "profile cancellation clears immediate work");
    RequestEquipApply(cfg, "2"); SetEnabled(false);
    c = Context(); OnApplyResetAndEquip(&c);
    CHECK(g_pendingEquipApply.load() == 0 && c.rax == 0x71D0, "Dormant clears work and hook is pass-through");
    CHECK(!RequestEquipApply(cfg, "3") && g_pendingEquipApply.load() == 0, "Dormant selection never revives work");
    // Semantic categories stay consumed across callers, IDs and languages.
    SetEnabled(true);
    auto noticeStart = chats.size();
    g_ready = false;
    RequestRestart(cfg); RequestResetOnly(); RequestRestart(cfg);
    CHECK(chats.size() == noticeStart + 1, "restart and reset-only share CoreUnavailable category");
    g_ready = true;

    noticeStart = chats.size();
    LoadoutSet missing;
    missing.equip = "old equipment name A"; missing.item = "old item name A";
    QueueSet(missing);
    missing.equip = "old equipment name B"; missing.item = "old item name B";
    i18n::Set(i18n::kJa); QueueSet(missing); i18n::Set(i18n::kEn); QueueSet(missing);
    CHECK(chats.size() == noticeStart + 2, "missing equipment and item each emit once despite text and language changes");
    cfg.activeSet = 999;
    RequestRestart(cfg); CancelPending();
    cfg.activeSet = 1000;
    RequestRestart(cfg); CancelPending();
    CHECK(chats.size() == noticeStart + 3, "missing active set emits once across different IDs");
    cfg.activeSet = -1;

    noticeStart = chats.size();
    for (int n : {78, 79, 78}) {
        g_pendingSetItem = n;
        Tick(cfg);
    }
    CHECK(chats.size() == noticeStart + 1, "empty item emits once across loadout IDs");
    noticeStart = chats.size();
    g.message = kMsgEquip | kMsgQuest | kMsgCant | kMsgWindow | kMsgNoQuest;
    Tick(cfg);
    CHECK(chats.size() == noticeStart + 5 && g.message == 0,
          "each deferred failure category emits once and drains the pending bitmask");
    CancelPending(); SetEnabled(false); SetEnabled(true);
    i18n::Set(i18n::kZh);
    g.message = kMsgEquip | kMsgQuest | kMsgCant | kMsgWindow | kMsgNoQuest;
    Tick(cfg);
    CHECK(chats.size() == noticeStart + 5 && g.message == 0,
          "deferred failure categories stay consumed across language change and Dormant");
    Config gateCfg;
    gateCfg.enabled = true;
    gateCfg.palicoEnabled = false;
    LoadoutSet palicoSet;
    palicoSet.id = 41;
    palicoSet.palicoLoadout = 7;
    ApplyOptions(gateCfg);
    QueueSet(palicoSet);
    CHECK(g.palicoLoadout == 0, "Palico loadout queue is inert while the feature is disabled");
    gateCfg.palicoEnabled = true;
    ApplyOptions(gateCfg);
    QueueSet(palicoSet);
    CHECK(g.palicoLoadout == 7, "Palico loadout queue resumes after explicit enable");
    gateCfg.palicoEnabled = false;
    ApplyOptions(gateCfg);
    CHECK(g.palicoLoadout == 0, "disabling clears a pending Palico application");
    const auto record = [&](int n) { return At(slot) + 0x13B574 + std::uintptr_t(n - 1) * 0x130; };
    for (int n = 1; n <= 24; ++n) Put<std::uint32_t>(record(n) + 0x100, 0xFFFFFFFFu);
    Put<std::uint32_t>(record(3) + 0x100, 1); // occupied, unnamed record
    Put<std::uint32_t>(record(1) + 0x100, 1);
    std::memcpy(reinterpret_cast<void*>(record(1)), "   ", 4);
    Put<std::uint32_t>(record(2) + 0x100, 1);
    std::memcpy(reinterpret_cast<void*>(record(2)), "Alpha", 6);
    Put<std::uint32_t>(record(4) + 0x100, 0xFFFFFFFFu);
    std::memcpy(reinterpret_cast<void*>(record(4)), "Empty", 6);
    Put<std::uint32_t>(record(7) + 0x100, 1);
    std::memcpy(reinterpret_cast<void*>(record(7)), "Bravo", 6);
    const auto palicoNames = ListPalicoLoadouts();
    CHECK(palicoNames.size() == 4 && palicoNames[0].number == 1 && !palicoNames[0].name.empty() &&
          palicoNames[1].number == 2 && palicoNames[1].name == "Alpha" &&
          palicoNames[2].number == 3 && !palicoNames[2].name.empty() &&
          palicoNames[3].number == 7 && palicoNames[3].name == "Bravo",
          "Palico names preserve occupied unnamed records and sparse IDs, but omit empty equipment");
    // No game code is ever rewritten, even when enabling/disabling the option.
    CHECK(MapGlobal(game::kItemSetRead1), "map isolated original setting reads");
    const std::uint64_t originals[3] = {0x840014041588B60FULL,0x00140415B8B60F44ULL,0x480014041588B60FULL};
    const std::uintptr_t sites[3] = {game::kItemSetRead1,game::kItemSetRead2,game::kItemSetRead3};
    for (int i=0;i<3;++i) Put(sites[i],originals[i]);
    CHECK(!SetItemBarrier(true), "missing native callable fails closed");
    CHECK(SetItemBarrier(false), "disable needs no executable writes");
    for (int i=0;i<3;++i) CHECK(game::Rd<std::uint64_t>(sites[i])==originals[i], "all native setting reads remain unchanged");
    CHECK(MapGlobal(game::kItemLoadoutCmd), "map isolated native command table");
    CHECK(game::Readable(game::EXE+0x1D38110, 10), "apply prologue shares the mapped setting-read page");
    const std::uintptr_t tableFixture[6] = {game::EXE+0x1D37230,game::EXE+0x1D37230,
        game::EXE+0x1D37CC0,0x1111,0x2222,0x3333};
    std::memcpy(reinterpret_cast<void*>(game::kItemLoadoutCmd),tableFixture,sizeof(tableFixture));
    const unsigned char applyHead[10]={0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10};
    std::memcpy(reinterpret_cast<void*>(game::EXE+0x1D38110),applyHead,sizeof(applyHead));
    CHECK(SetItemBarrier(true), "valid ABI enables private table without patching game table");
    CHECK(g_itemCommandTable[0]==reinterpret_cast<std::uintptr_t>(&CopyItemCommand) &&
          g_itemCommandTable[2]==reinterpret_cast<std::uintptr_t>(&InvokeItemCommand) &&
          g_itemCommandTable[3]==0x1111 && g_itemCommandTable[4]==0x2222 && g_itemCommandTable[5]==0x3333,
          "private table replaces only copy/move/invoke slots");
    CHECK(std::memcmp(reinterpret_cast<void*>(game::kItemLoadoutCmd),tableFixture,sizeof(tableFixture))==0,
          "native command vtable itself remains byte-for-byte unchanged");
    g_itemApply = FakeDirect; g_itemOriginalInvoke = FakeNative;
    g_itemCommandOverride.store(true); SetEnabled(true);
    std::uintptr_t ownerMemory[1] = {game::EXE+0x3460220};
    std::uintptr_t owner = reinterpret_cast<std::uintptr_t>(ownerMemory);
    ItemCommand command{reinterpret_cast<std::uintptr_t>(g_itemCommandTable),79,0}, copied{};
    CopyItemCommand(&command,&copied);
    CHECK(copied.table==command.table && copied.index==79 && copied.mode==0, "copy preserves private callable identity and payload");
    InvokeItemCommand(&copied,nullptr,&owner);
    CHECK(directCalls==1 && nativeCalls==0 && seenOwner==owner && seenIndex==79 && seenMode==0, "only private command takes native yes/include-radial apply");
    command.mode=1; InvokeItemCommand(&command,nullptr,&owner);
    CHECK(directCalls==2 && seenMode==1, "explicit no-radial mode passed intact");
    g.restart=1; InvokeItemCommand(&copied,nullptr,&owner);
    CHECK(directCalls==2 && nativeCalls==0, "already queued private command is dropped during reset");
    g.restart=0; Put<std::uint32_t>(At(load)+0x1D04,1);
    InvokeItemCommand(&copied,nullptr,&owner);
    CHECK(directCalls==2 && nativeCalls==0, "already queued private command is dropped during loading after reset flag clears");
    Put<std::uint32_t>(At(load)+0x1D04,0);
    SetItemBarrier(false); InvokeItemCommand(&copied,nullptr,&owner);
    CHECK(directCalls==2 && nativeCalls==1, "queued command after disable uses original policy");
    g_itemCommandOverride.store(true); ownerMemory[0]=0;
    InvokeItemCommand(&copied,nullptr,&owner);
    CHECK(directCalls==2 && nativeCalls==2, "unexpected owner is never directly applied");
    ownerMemory[0]=game::EXE+0x3460220; SetEnabled(false);
    InvokeItemCommand(&copied,nullptr,&owner);
    CHECK(directCalls==2 && nativeCalls==3, "Dormant cannot invoke direct apply");
    CancelPending(); SetEnabled(true); SetItemBarrier(false);
    cfg.itemAutoApply=false; cfg.activeSet=1; cfg.sets[0].item="#3";
    Put<std::uint32_t>(At(quest)+0x38,1); Put<std::uint32_t>(At(load)+0x1D04,0);
    CHECK(RequestActiveSetEquipApply(cfg) && g_pendingSetItem==3 && g_explicitHubItem,
          "hub selection queues equipment and explicit BIQ items with auto-apply off");
    Put<std::uint64_t>(At(base)+0x8F60,0); Tick(cfg);
    ItemCommand hubCommand=game::Rd<ItemCommand>(At(base)+0x4F60);

    CHECK(hubCommand.table==reinterpret_cast<std::uintptr_t>(g_itemCommandTable) &&
          hubCommand.index==2 && (hubCommand.mode & kExplicitHubItem),
          "hub selection publishes private callable independently of auto switch");
    const int beforeHub=directCalls, beforeNative=nativeCalls;
    InvokeItemCommand(&hubCommand,nullptr,&owner);
    CHECK(directCalls==beforeHub+1 && nativeCalls==beforeNative && seenMode==0,
          "explicit hub callable applies with auto-apply off and masks its origin bit");
    Put<std::uint32_t>(At(quest)+0x38,2); InvokeItemCommand(&hubCommand,nullptr,&owner);
    CHECK(directCalls==beforeHub+1 && nativeCalls==beforeNative,
          "hub-only callable is discarded if consumed after leaving the hub");
    Put<std::uint32_t>(At(quest)+0x38,1); RequestActiveSetEquipApply(cfg);
    Put<std::uint32_t>(At(load)+0x1D04,1); Put<std::uint64_t>(At(base)+0x8F60,0); Tick(cfg);
    CHECK(g_pendingSetItem==0 && game::Rd<std::uint64_t>(At(base)+0x8F60)==0,
          "loading cancels a pending hub selection instead of replaying it later");
    Put<std::uint32_t>(At(load)+0x1D04,0); RequestActiveSetEquipApply(cfg);
    cfg.activeSet=-1; RequestActiveSetEquipApply(cfg);
    CHECK(g_pendingSetItem==0 && !g_explicitHubItem, "deselect cancels explicit hub item request");
    CancelPending(); SetEnabled(true);
    cfg.activeSet=1; cfg.itemAutoApply=true;
    Put<std::uint32_t>(At(quest)+0x38,1); Put<std::uint32_t>(At(load)+0x1D04,0);
    Put<std::uintptr_t>(At(itemUi),game::EXE+0x3460220);
    ApplyOptions(cfg); Tick(cfg); // publish current selection only
    const int beforeDepart=directCalls;
    auto departureCtx=Context(); g.restart=0;
    OnItemDeparture(&departureCtx);
    CHECK(directCalls==beforeDepart+1 && seenOwner==At(itemUi) && seenIndex==2 && seenMode==0,
          "ordinary hub departure applies on GUI thread before original departure");
    Put<std::uint32_t>(At(quest)+0x38,2); OnItemDeparture(&departureCtx);
    CHECK(directCalls==beforeDepart+1, "in-quest departure or chained restart cannot auto-apply");
    g.restart=0; Put<std::uint64_t>(At(base)+0x8F60,0); Tick(cfg);
    CHECK(game::Rd<std::uint64_t>(At(base)+0x8F60)==0,
          "quest arrival does not enqueue a late item command");
    Put<std::uint32_t>(At(quest)+0x38,1); Put<std::uint32_t>(At(load)+0x1D04,1);
    OnItemDeparture(&departureCtx);
    CHECK(directCalls==beforeDepart+1, "loading never invokes departure item GUI");
    Put<std::uint32_t>(At(load)+0x1D04,0); cfg.itemAutoApply=false; ApplyOptions(cfg);
    OnItemDeparture(&departureCtx);
    CHECK(directCalls==beforeDepart+1, "auto switch disables departure hook behavior");
    cfg.itemAutoApply=true; ApplyOptions(cfg); g_departureSlot.store(At(slot)+0x26cc00);
    OnItemDeparture(&departureCtx);
    CHECK(directCalls==beforeDepart+1, "departure refuses selection from another save slot");
    CancelPending(); SetEnabled(true);
    cfg.itemAutoApply=true; cfg.activeSet=1; ApplyOptions(cfg);
    std::vector<unsigned char> prepare(0x3000), questMenu(0x2300);
    Put<std::uintptr_t>(At(ui)+0x3B8,At(prepare));
    auto departViaBiq=[&]() {
        auto context=Context(); context.rcx=At(questMenu);
        g.restart=4; Put<std::uint32_t>(At(prepare)+0x28F0,1);
        OnQuestLoad(&context);
    };
    Put<std::uint32_t>(At(quest)+0x38,1);
    RequestRestart(cfg);
    const int beforeBiq=directCalls;
    departViaBiq();
    CHECK(directCalls==beforeBiq+1 && seenOwner==At(itemUi) && seenIndex==2 &&
          game::Rd<std::uint32_t>(At(prepare)+0x28F0)==8 && g.restart==0,
          "hub-origin BIQ restart applies items before writing departure state");
    OnRestartItemDeparture();
    CHECK(directCalls==beforeBiq+1, "BIQ departure origin is consumed only once");
    Put<std::uint32_t>(At(quest)+0x38,2); RequestRestart(cfg);
    Put<std::uint32_t>(At(quest)+0x38,1); departViaBiq();
    CHECK(directCalls==beforeBiq+1,
          "field-origin restart never applies even when departure later observes a hub");
    RequestResetOnly();
    CHECK(!g_restartItemFromHub.load() && g_restartDepartureItem.load()==0,
          "reset-only cannot retain hub restart departure work");
    CancelPending(); RequestRestart(cfg); cfg.itemAutoApply=false; ApplyOptions(cfg);
    departViaBiq();
    CHECK(directCalls==beforeBiq+1, "disabling auto before BIQ departure suppresses item apply");
    cfg.itemAutoApply=true; ApplyOptions(cfg); RequestRestart(cfg); CancelPending();
    CHECK(!g_restartItemFromHub.load() && g_restartDepartureItem.load()==0,
          "cancellation clears restart departure origin and selection");
    for (int i=0;i<3;++i) CHECK(game::Rd<std::uint64_t>(sites[i])==originals[i], "private command never patches global instructions");
    SetEnabled(false);
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
