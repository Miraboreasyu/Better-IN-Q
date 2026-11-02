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
    CHECK(RequestActiveSetEquipApply(cfg), "active set queues equipment only");
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
    CHECK(g.equipLoadout == 2 && g_pendingSetItem == 3 && g.food == 4,
          "intentional restart retains original equipment item and meal path");
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
    SetEnabled(false);
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
