// 面板的手柄导航：模拟十字键 / A / B，看页面和焦点是不是按设计走。离屏渲染，不开窗口。
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <algorithm>
#include <vector>
#include <utility>
#include <xinput.h>

#include "../src/config.h"
#include "../src/gamedata.h"
#include "../src/input.h"
#include "../src/i18n.h"
#include "../src/ui.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx11.h"

// 测试专用钩子：不为了检查展示文案扩展 ui.h 的公共接口。
namespace ui {
std::string DebugSetSummary(const Model& m, int index);
std::string DebugFoodSkillsSummary(const FoodPreset& f);
void DebugFocusHotkey(bool pad);
void DebugFocusSearch();
std::string DebugSearch();
void DebugFocusSettingsTransfer(bool importing);
ImVec2 DebugSettingsExportCenter();
void DebugFocusPickEntry(const wchar_t* name);
std::vector<std::wstring> DebugPickEntries();
bool DebugSettingsConfirm();
}

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) std::printf("  ok   " __VA_ARGS__); \
    else { std::printf("  FAIL " __VA_ARGS__); ++g_fail; } std::printf("\n"); } while (0)

static ID3D11Device* dev = nullptr;
static ID3D11DeviceContext* ctx = nullptr;
static ID3D11RenderTargetView* rtv = nullptr;
static ui::Model m;
static std::vector<int> g_sideTrace;   // 每帧焦点在侧栏第几格

static void Frame()
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    ui::Draw(m, io.DisplaySize.x, io.DisplaySize.y);
    ImGui::Render();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_sideTrace.push_back(ui::DebugSidebarFocusIndex());
}

static float PanelH() { return ui::DebugFrameHeight(); }

// 录键：手柄 / 键盘按住一组，再全部松开
static void Hold(unsigned pad, std::vector<int> keys = {})
{
    m.padHeld = pad;
    m.keysHeld = keys;
    for (int i = 0; i < 3; ++i) Frame();
}

static void Press(ImGuiKey k)
{
    ImGui::GetIO().AddKeyEvent(k, true);
    Frame();
    ImGui::GetIO().AddKeyEvent(k, false);
    Frame();
    Frame();
}

// Real filesystem enumeration and actual button/navigation events. Transfer IO is
// deliberately owned by main: the UI may only emit the two explicit requests.
static void TestSettingsTransfer()
{
    wchar_t temp[MAX_PATH] = {}, unique[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    GetTempFileNameW(temp, L"biq", 0, unique);
    DeleteFileW(unique);
    CHECK(CreateDirectoryW(unique, nullptr) != FALSE, "settings picker fixture directory created");
    const std::wstring dir = unique;
    const wchar_t* files[] = {L"backup.BIQ", L"legacy.bin", L"config.txt", L"ignore.exe"};
    for (const auto* name : files) {
        HANDLE f = CreateFileW((dir + L"\\" + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        CHECK(f != INVALID_HANDLE_VALUE, "settings picker real file fixture created");
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    const std::wstring oldDir = m.backupsDir;
    m.backupsDir = dir;
    const int oldLanguage = i18n::Current();
    auto settle = [] { for (int n = 0; n < 5; ++n) Frame(); };
    auto focus = [&](bool importing) { ui::DebugFocusSettingsTransfer(importing); Frame(); settle(); };
    auto open = [&] {
        ui::OnOpen(m); ui::DebugGo(6, 0); settle();
        m.reqClose = m.reqSave = m.reqApplyEquip = m.reqRestart = m.reqResetOnly = false;
        m.reqExportSettings = m.reqImportSettings = m.reqImportInq = false;
        m.settingsPath.clear();
    };
    auto pick = [&](ImGuiKey activate) {
        focus(true); Press(activate); settle();
        const auto entries = ui::DebugPickEntries();
        CHECK(std::find(entries.begin(), entries.end(), L"backup.BIQ") != entries.end() &&
              std::find(entries.begin(), entries.end(), L"legacy.bin") != entries.end() &&
              std::find(entries.begin(), entries.end(), L"config.txt") == entries.end() &&
              std::find(entries.begin(), entries.end(), L"ignore.exe") == entries.end(),
              "backup picker enumerates real .BIQ/.bin only");
        ui::DebugFocusPickEntry(L"backup.BIQ"); Frame(); settle(); Press(activate); settle();
        CHECK(ui::DebugSettingsConfirm() && !m.reqImportSettings && m.settingsPath.empty(),
              "selecting a backup stages confirmation without IO request");
    };
    for (int language = 0; language < 3; ++language) {
        i18n::Set((i18n::Lang)language); open(); focus(false); Press(ImGuiKey_GamepadFaceDown);
        CHECK(m.reqExportSettings && !m.reqImportSettings && !m.reqSave && !m.reqApplyEquip,
              "export emits only one-shot main request language=%d", language);
        m.reqExportSettings = false;
        pick(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugHintText().find(language == 0 ? "覆盖" : language == 1 ? "Replace" : "上書き") != std::string::npos,
              "confirmation footer is context specific language=%d", language);
        Press(ImGuiKey_GamepadFaceDown); // Safe default is Cancel, not Import.
        CHECK(!ui::DebugSettingsConfirm() && !m.reqImportSettings && !m.reqClose && ui::DebugPage() == 6,
              "default A cancels without request or panel-back language=%d", language);
        pick(ImGuiKey_GamepadFaceDown); Press(ImGuiKey_GamepadFaceRight);
        CHECK(!ui::DebugSettingsConfirm() && !m.reqImportSettings && !m.reqClose && ui::DebugPage() == 6,
              "B cancels confirmation without leaking to underlying page language=%d", language);
        pick(ImGuiKey_GamepadFaceDown); Press(ImGuiKey_GamepadDpadRight); Press(ImGuiKey_GamepadFaceDown);
        CHECK(m.reqImportSettings && m.settingsPath == dir + L"\\backup.BIQ" &&
              !m.reqImportInq && !m.reqApplyEquip && !m.reqRestart && !m.reqResetOnly,
              "only explicit confirm emits exact path and no apply/restart language=%d", language);
        m.reqImportSettings = false; m.settingsPath.clear();
        open(); focus(true); Press(ImGuiKey_Enter); settle(); Press(ImGuiKey_Escape);
        CHECK(!m.reqImportSettings && m.settingsPath.empty() && !m.reqClose && ui::DebugPage() == 6,
              "keyboard picker Esc cancels without a transfer request language=%d", language);
    }
    open(); focus(false);
    // NavRectRel may retain the pre-scroll scoring rectangle. Hit-test the
    // actual rendered item after scrolling, never a stale navigation proxy.
    const ImVec2 exportCenter = ui::DebugSettingsExportCenter();
    ImGui::GetIO().AddMousePosEvent(exportCenter.x, exportCenter.y); Frame();
    ImGui::GetIO().AddMouseButtonEvent(0, true); Frame();
    ImGui::GetIO().AddMouseButtonEvent(0, false); Frame(); settle();
    CHECK(m.reqExportSettings && !m.reqImportSettings && ui::DebugHintSource() == 2,
          "mouse export click emits main request and uses mouse footer");
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX); Frame();
    open(); pick(ImGuiKey_Enter); Press(ImGuiKey_Escape);
    CHECK(!ui::DebugSettingsConfirm() && !m.reqImportSettings && !m.reqClose,
          "keyboard confirmation Esc cancels only modal");
    // The original picker is still txt-only and cannot emit a backup request.
    ui::DebugOpenPick(); settle();
    const auto legacy = ui::DebugPickEntries();
    CHECK(!m.reqImportSettings && !ui::DebugSettingsConfirm(), "original IN-Q picker remains a separate import mode");
    Press(ImGuiKey_Escape);
    ui::OnOpen(m); settle();
    for (const auto* name : files) DeleteFileW((dir + L"\\" + name).c_str());
    RemoveDirectoryW(dir.c_str());
    m.backupsDir = oldDir;
    i18n::Set((i18n::Lang)oldLanguage);
}

// Real AddKeyEvent input, including NewFrame's native Ctrl+arrow navigation.
static void TestDirectReorder()
{
    Config* original = m.cfg;
    Config c;
    for (int id = 1; id <= 18; ++id) {
        LoadoutSet s; s.id = id * 3; s.name = "same"; s.food = id;
        c.sets.push_back(s); c.foods[id] = FoodPreset(); c.foodOrder.push_back(id);
    }
    c.activeSet = c.sets.front().id; c.food = 1;
    const Config baseline = c;
    m.cfg = &c;
    ImGuiIO& io = ImGui::GetIO();
    auto open = [&](int page) {
        ui::OnOpen(m); ui::DebugGo(page, 0);
        for (int n = 0; n < 6; ++n) Frame();
        m.reqSave = m.reqApplyEquip = false;
    };
    auto order = [&](int page) {
        std::vector<int> r;
        if (page == 2) for (const auto& s : c.sets) r.push_back(s.id);
        else r = ui::FoodRowIds(c);
        return r;
    };
    auto chord = [&](ImGuiKey key) {
        io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(key, true); Frame();
        io.AddKeyEvent(key, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); Frame(); Frame();
    };
    for (int page : {2, 4}) {
        c = baseline; open(page);
        const auto initial = order(page);
        const ImGuiID focused = GImGui->NavId;
        Press(ImGuiKey_GamepadR1);
        auto expected = initial; std::swap(expected[0], expected[1]);
        CHECK(order(page) == expected && GImGui->NavId == focused && m.reqSave && !m.reqApplyEquip,
              "direct RB moves one row, stable focus/save only page=%d", page);
        chord(ImGuiKey_UpArrow);
        CHECK(order(page) == initial && GImGui->NavId == focused,
              "Ctrl+Up sorts without also navigating page=%d", page);
        m.reqSave = false;
        chord(ImGuiKey_UpArrow); Press(ImGuiKey_GamepadL1);
        CHECK(order(page) == initial && GImGui->NavId == focused && !m.reqSave,
              "top edge no save or focus drift page=%d", page);
        chord(ImGuiKey_DownArrow);
        CHECK(order(page) == expected && GImGui->NavId == focused,
              "Ctrl+Down moves same stable row page=%d", page);
        io.AddKeyEvent(ImGuiKey_GamepadR1, true);
        for (int n = 0; n < 150; ++n) Frame();
        io.AddKeyEvent(ImGuiKey_GamepadR1, false); Frame(); Frame();
        CHECK(order(page).back() == initial.front() && GImGui->NavId == focused && ui::DebugListScroll() > 0,
              "held RB repeats to bottom with focus and scrolling page=%d", page);
        const auto bottom = order(page); m.reqSave = false;
        chord(ImGuiKey_DownArrow); Press(ImGuiKey_GamepadR1);
        CHECK(order(page) == bottom && GImGui->NavId == focused && !m.reqSave,
              "bottom edge never navigates to New button page=%d", page);
        CHECK(c.activeSet == baseline.activeSet && c.food == baseline.food &&
              c.FindSetById(baseline.activeSet)->food == 1,
              "reorder retains active stable ID and meal references page=%d", page);
        Press(ImGuiKey_E); const auto moving = order(page);
        Press(ImGuiKey_GamepadL1); Press(ImGuiKey_GamepadR1);
        CHECK(ui::DebugMoveActive() && order(page) == moving,
              "LB/RB disabled during E move mode page=%d", page);
        chord(ImGuiKey_UpArrow);
        auto movedOnce = moving; std::swap(movedOnce[movedOnce.size()-1], movedOnce[movedOnce.size()-2]);
        CHECK(order(page) == movedOnce && ui::DebugMoveActive(),
              "move mode Ctrl+Up moves only once, not a second shortcut page=%d", page);
        Press(ImGuiKey_Escape);
        c = baseline; open(page);
        io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_DownArrow, true);
        for (int n = 0; n < 150; ++n) Frame();
        io.AddKeyEvent(ImGuiKey_DownArrow, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); Frame(); Frame();
        CHECK(order(page).back() == initial.front() && GImGui->NavId == focused,
              "held Ctrl+Down repeats sorting only to bottom page=%d", page);
        c = baseline; open(page); Press(ImGuiKey_F); const auto renamed = order(page);
        chord(ImGuiKey_DownArrow); Press(ImGuiKey_GamepadR1);
        CHECK(order(page) == renamed, "rename popup cannot reorder page=%d", page);
        io.AddKeyEvent(ImGuiKey_GamepadR1, true); Frame();
        Press(ImGuiKey_Escape);
        for (int n = 0; n < 45; ++n) Frame();
        CHECK(order(page) == renamed, "popup held shoulder cannot leak repeat on close page=%d", page);
        io.AddKeyEvent(ImGuiKey_GamepadR1, false); Frame(); Frame();
        open(page);
        CHECK(!ui::DebugMoveActive() && order(page) == initial,
              "close/reopen clears reorder state page=%d", page);
        // Existing plain arrows, letters, edit and apply still operate on the row.
        Press(ImGuiKey_S); const ImGuiID second = GImGui->NavId;
        Press(ImGuiKey_W);
        CHECK(GImGui->NavId != second && order(page) == initial,
              "W/S navigation does not reorder page=%d", page);
        Press(ImGuiKey_Y);
        CHECK(ui::DebugPage() == page + 1, "Y editing survives direct reorder page=%d", page);
    }
    c = baseline; c.foodOrder = {1, 1, 999, 2}; open(4);
    const auto canonical = ui::FoodRowIds(c);
    Press(ImGuiKey_GamepadR1);
    auto normalized = canonical; std::swap(normalized[0], normalized[1]);
    CHECK(c.foodOrder == normalized && c.foods.size() == baseline.foods.size(),
          "meal reorder persists canonical display order without duplicate/missing IDs");
    wchar_t tempDir[MAX_PATH] = {}, tempFile[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir); GetTempFileNameW(tempDir, L"inq", 0, tempFile);
    Config restored; std::string loadError;
    const bool roundTrip = SaveConfig(tempFile, c) && LoadConfig(tempFile, restored, loadError);
    CHECK(roundTrip && restored.foodOrder == normalized && restored.activeSet == c.activeSet &&
          restored.FindSetById(c.activeSet) && restored.FindSetById(c.activeSet)->food == 1,
          "actual config save/load preserves reordered foodOrder and stable references");
    DeleteFileW(tempFile);
    open(6); ui::DebugStartCapture(m, 0, false);
    const auto captureOrder = c.foodOrder;
    chord(ImGuiKey_DownArrow); Press(ImGuiKey_GamepadR1);
    CHECK(c.foodOrder == captureOrder, "hotkey capture never reorders lists");
    Press(ImGuiKey_Escape); ui::OnOpen(m);
    m.cfg = original;
    for (int n = 0; n < 4; ++n) Frame();
}

static const char* PageName(int p)
{
    static const char* n[] = {"主页", "换任务", "套装", "套装编辑", "猫饭", "猫饭编辑", "设置"};
    return p >= 0 && p < 7 ? n[p] : "?";
}

// X is also ImGui's default window-management button. Hold well past the
// highlight delay and inspect every rendered frame, not just the released state.
static void TestNoWindowing()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const int navFlags = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    CHECK(!g.ConfigNavWindowingWithGamepad &&
          g.ConfigNavWindowingKeyNext == ImGuiKey_None && g.ConfigNavWindowingKeyPrev == ImGuiKey_None,
          "初始化关闭手柄及 Ctrl+Tab / Ctrl+Shift+Tab 整窗切换");
    CHECK((io.ConfigFlags & navFlags) == navFlags, "禁用整窗切换仍保留键盘及手柄控件导航");
    auto noWindowing = [&]() {
        return g.NavWindowingTarget == nullptr && g.NavWindowingTargetAnim == nullptr &&
               g.NavWindowingHighlightAlpha == 0.0f && g.DimBgRatio == 0.0f;
    };
    for (int page : {0, 6}) {
        ui::OnOpen(m);
        ui::DebugGo(page, 0);
        for (int i = 0; i < 90; ++i) Frame(); // Let normal page-height animation settle first.
        const ImGuiID focus = g.NavId;
        const ImVec2 pos = g.NavWindow->RootWindow->Pos;
        const ImVec2 size = g.NavWindow->RootWindow->Size;
        bool clean = true;
        io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, true);
        for (int i = 0; i < 90; ++i) { Frame(); clean &= noWindowing(); }
        io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, false);
        for (int i = 0; i < 12; ++i) { Frame(); clean &= noWindowing(); }
        CHECK(clean, "%s长按 X 及松开后：逐帧无整窗目标、高亮或背景变暗", PageName(page));
        CHECK(g.NavId == focus && g.NavLayer == ImGuiNavLayer_Main && ui::DebugPage() == page &&
              !m.reqClose && !m.reqRestart,
              "%s长按 X 后：页面、控件焦点和主导航层保持不变", PageName(page));
        CHECK(g.NavWindow->RootWindow->Pos.x == pos.x && g.NavWindow->RootWindow->Pos.y == pos.y &&
              std::fabs(g.NavWindow->RootWindow->Size.x - size.x) < 1.0f &&
              std::fabs(g.NavWindow->RootWindow->Size.y - size.y) < 1.0f,
              "%s长按 X 不移动或缩放整个面板", PageName(page));
    }
    for (bool reverse : {false, true}) {
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        if (reverse) io.AddKeyEvent(ImGuiMod_Shift, true);
        io.AddKeyEvent(ImGuiKey_Tab, true);
        bool clean = true;
        for (int i = 0; i < 60; ++i) { Frame(); clean &= noWindowing(); }
        io.AddKeyEvent(ImGuiKey_Tab, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        if (reverse) io.AddKeyEvent(ImGuiMod_Shift, false);
        for (int i = 0; i < 12; ++i) { Frame(); clean &= noWindowing(); }
        CHECK(clean, "%s 长按及松开后不进入整窗切换", reverse ? "Ctrl+Shift+Tab" : "Ctrl+Tab");
    }
    ui::OnOpen(m);
    for (int i = 0; i < 4; ++i) Frame();
}

// Exercise real IO events: no synthetic arrows, all callers get the Init context hook.
static void TestLetterNavigation()
{
    Config* original = m.cfg;
    Config c;
    for (int id = 1; id <= 12; ++id) {
        LoadoutSet set; set.id = id; set.name = "row" + std::to_string(id);
        c.sets.push_back(set);
        c.foods[id] = FoodPreset(); c.foodOrder.push_back(id);
    }
    m.cfg = &c;
    ImGuiIO& io = ImGui::GetIO();
    auto open = [&](int page = 0) {
        ui::OnOpen(m); ui::DebugGo(page, 0);
        for (int n = 0; n < 5; ++n) Frame();
        m.reqSave = m.reqApplyEquip = m.reqClose = m.reqRestart = false;
    };
    auto arrowsClean = [&]() {
        for (ImGuiKey key : {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow})
            if (ImGui::IsKeyDown(key) || ImGui::IsKeyPressed(key)) return false;
        return true;
    };
    // Compare exact target IDs to native arrows, including horizontal row geometry/bias.
    for (const auto& pair : std::vector<std::pair<ImGuiKey, ImGuiKey>>{
             {ImGuiKey_S, ImGuiKey_DownArrow}, {ImGuiKey_J, ImGuiKey_DownArrow},
             {ImGuiKey_W, ImGuiKey_UpArrow}, {ImGuiKey_K, ImGuiKey_UpArrow},
             {ImGuiKey_A, ImGuiKey_LeftArrow}, {ImGuiKey_H, ImGuiKey_LeftArrow},
             {ImGuiKey_D, ImGuiKey_RightArrow}, {ImGuiKey_L, ImGuiKey_RightArrow}}) {
        open(); Press(ImGuiKey_DownArrow);
        Press(pair.second); const ImGuiID expected = GImGui->NavId;
        open(); Press(ImGuiKey_DownArrow);
        io.AddKeyEvent(pair.first, true); Frame();
        CHECK(arrowsClean(), "字母 %s 不生成或持有伪方向键", ImGui::GetKeyName(pair.first));
        io.AddKeyEvent(pair.first, false); Frame(); Frame();
        CHECK(GImGui->NavId == expected && !m.reqRestart && !m.reqClose,
              "字母 %s 与原生 %s 焦点一致且不触发确认", ImGui::GetKeyName(pair.first), ImGui::GetKeyName(pair.second));
    }
    // Adjacent frames and a single Win32-pump batch: Press() deliberately leaves
    // two settling frames, so it cannot exercise rapid opposite-direction input.
    auto rapid = [&](ImGuiKey up, ImGuiKey down, int mode) {
        io.AddKeyEvent(up, true); Frame();
        if (mode == 1) { io.AddKeyEvent(down, true); Frame(); io.AddKeyEvent(up, false); }
        else { io.AddKeyEvent(up, false); io.AddKeyEvent(down, true); }
        Frame(); io.AddKeyEvent(down, false); Frame(); Frame(); Frame();
    };
    for (int page : {0, 2, 4, 6}) {
        for (int mode : {0, 1, 2}) {
            for (bool reverse : {false, true}) {
                auto run = [&](ImGuiKey up, ImGuiKey down) {
                    open(page);
                    if (page == 0 || page == 6) Press(ImGuiKey_DownArrow);
                    if (reverse) std::swap(up, down);
                    if (mode == 2) {
                        io.AddKeyEvent(up, true); io.AddKeyEvent(up, false);
                        io.AddKeyEvent(down, true); io.AddKeyEvent(down, false);
                        for (int n = 0; n < 8; ++n) Frame();
                    } else rapid(up, down, mode);
                    return GImGui->NavId;
                };
                const ImGuiID expected = run(ImGuiKey_UpArrow, ImGuiKey_DownArrow);
                for (bool vim : {false, true}) {
                    const ImGuiID actual = run(vim ? ImGuiKey_K : ImGuiKey_W, vim ? ImGuiKey_J : ImGuiKey_S);
                    CHECK(actual == expected && arrowsClean(),
                          "rapid %s page=%d mode=%d reverse=%d matches native arrows",
                          vim ? "K/J" : "W/S", page, mode, reverse);
                }
            }
        }
    }
    // The hook must respect genuine ImGui owners, not merely inspect Down.
    open(2); const ImGuiID ownerFocus = GImGui->NavId;
    ImGui::SetKeyOwner(ImGuiKey_S, ownerFocus, ImGuiInputFlags_LockUntilRelease);
    Press(ImGuiKey_S);
    CHECK(GImGui->NavId == ownerFocus, "owned S is not stolen by letter navigation");
    Press(ImGuiKey_S);
    CHECK(GImGui->NavId != ownerFocus, "S navigates again after owner releases key");
    for (int mode : {0, 1}) {
        for (bool bottom : {false, true}) {
            auto runSide = [&](ImGuiKey up, ImGuiKey down) {
                open(); Press(ImGuiKey_LeftArrow);
                if (bottom) for (int n = 0; n < 3; ++n) Press(ImGuiKey_DownArrow);
                rapid(bottom ? down : up, bottom ? up : down, mode);
                return std::make_pair(ui::DebugSidebarFocusIndex(), ui::DebugPage());
            };
            const auto expected = runSide(ImGuiKey_UpArrow, ImGuiKey_DownArrow);
            for (bool vim : {false, true}) {
                CHECK(runSide(vim ? ImGuiKey_K : ImGuiKey_W, vim ? ImGuiKey_J : ImGuiKey_S) == expected,
                      "rapid sidebar %s mode=%d bottom=%d matches native at edge", vim ? "K/J" : "W/S", mode, bottom);
            }
        }
    }
    // Leave the sidebar before reusing the content-focus fixture: OnOpen alone
    // intentionally preserves ImGui's live focus until a content item claims it.
    Press(ImGuiKey_RightArrow);
    // Sorting consumes pulses directly rather than submitting an ImGui move.
    // Check both list edges with no settling frame between opposite presses.
    const Config listBaseline = c;
    for (int page : {2, 4}) {
        for (int mode : {0, 1}) {
            for (bool bottom : {false, true}) {
                auto runSort = [&](ImGuiKey up, ImGuiKey down) {
                    c = listBaseline; open(page); Press(ImGuiKey_E);
                    if (bottom) for (int n = 0; n < 16; ++n) Press(ImGuiKey_DownArrow);
                    rapid(bottom ? down : up, bottom ? up : down, mode);
                    std::vector<int> order;
                    if (page == 2) for (const auto& set : c.sets) order.push_back(set.id);
                    else order = c.foodOrder;
                    CHECK(ui::DebugMoveActive(), "rapid list sorting remains active page=%d", page);
                    Press(ImGuiKey_Escape);
                    return order;
                };
                const auto expected = runSort(ImGuiKey_UpArrow, ImGuiKey_DownArrow);
                for (bool vim : {false, true}) {
                    CHECK(runSort(vim ? ImGuiKey_K : ImGuiKey_W, vim ? ImGuiKey_J : ImGuiKey_S) == expected,
                          "rapid sorting %s page=%d mode=%d bottom=%d matches native at edge",
                          vim ? "K/J" : "W/S", page, mode, bottom);
                }
            }
        }
    }
    c = listBaseline;
    for (bool vim : {false, true}) {
        open();
        Press(vim ? ImGuiKey_H : ImGuiKey_A);
        CHECK(ui::DebugSidebarFocusIndex() == 0, "%s 左：进入当前页侧栏", vim ? "Vim" : "WASD");
        Press(vim ? ImGuiKey_J : ImGuiKey_S);
        CHECK(ui::DebugSidebarFocusIndex() == 1 && ui::DebugPage() == 2, "字母下：侧栏切套装");
        Press(vim ? ImGuiKey_J : ImGuiKey_S);
        Press(vim ? ImGuiKey_K : ImGuiKey_W);
        CHECK(ui::DebugSidebarFocusIndex() == 1 && ui::DebugPage() == 2, "字母上：侧栏返回套装");
        Press(vim ? ImGuiKey_L : ImGuiKey_D);
        CHECK(!ui::DebugSidebarFocused() && ui::DebugPage() == 2, "字母右：返回内容默认项");
    }
    open(); Press(ImGuiKey_DownArrow); const ImGuiID oneDown = GImGui->NavId;
    open();
    io.AddKeyEvent(ImGuiKey_DownArrow, true); io.AddKeyEvent(ImGuiKey_S, true); Frame();
    io.AddKeyEvent(ImGuiKey_DownArrow, false); io.AddKeyEvent(ImGuiKey_S, false); Frame(); Frame();
    CHECK(GImGui->NavId == oneDown, "方向键与同向字母同帧只移动一次");
    open(); Press(ImGuiKey_A);
    io.AddKeyEvent(ImGuiKey_DownArrow, true); io.AddKeyEvent(ImGuiKey_L, true); Frame();
    io.AddKeyEvent(ImGuiKey_DownArrow, false); io.AddKeyEvent(ImGuiKey_L, false); Frame(); Frame();
    CHECK(ui::DebugSidebarFocusIndex() == 1 && ui::DebugPage() == 2,
          "侧栏 DownArrow+L：原生下优先，字母右不误退出侧栏");
    open(2); Press(ImGuiKey_E); const int movingId = ui::DebugMoveId();
    io.AddKeyEvent(ImGuiKey_DownArrow, true); io.AddKeyEvent(ImGuiKey_W, true); Frame();
    io.AddKeyEvent(ImGuiKey_DownArrow, false); io.AddKeyEvent(ImGuiKey_W, false); Frame(); Frame();
    CHECK(c.sets[1].id == movingId, "排序 DownArrow+W：原生下优先，不被字母上反转");
    Press(ImGuiKey_Escape);
    for (ImGuiKey key : {ImGuiKey_S, ImGuiKey_J}) {
        open(2); const ImGuiID first = GImGui->NavId;
        io.AddKeyEvent(key, true); for (int n = 0; n < 45; ++n) Frame();
        io.AddKeyEvent(key, false); Frame(); Frame();
        CHECK(GImGui->NavId != first && !ui::DebugSidebarFocused(), "长按 %s 重复导航", ImGui::GetKeyName(key));
    }
    for (int page : {2, 4}) {
        open(page); Press(ImGuiKey_E);
        const int id = ui::DebugMoveId();
        Press(ImGuiKey_S); Press(ImGuiKey_J); Press(ImGuiKey_W); Press(ImGuiKey_K);
        CHECK((page == 2 ? c.sets[0].id : c.foodOrder[0]) == id && ui::DebugMoveActive(),
              "%s排序：S/J 下移、W/K 上移回原位，保持移动项", PageName(page));
        io.AddKeyEvent(ImGuiKey_J, true); for (int n = 0; n < 90; ++n) Frame();
        io.AddKeyEvent(ImGuiKey_J, false); Frame(); Frame();
        CHECK((page == 2 ? c.sets.back().id : c.foodOrder.back()) == id && ui::DebugListScroll() > 0,
              "%s排序：长按 J 连续换位到末尾并滚动", PageName(page));
        Press(ImGuiKey_Escape);
    }
    // Genuine rename text input; alphabetic navigation must not steal focus or apply a row.
    open(2); ui::DebugOpenRename(); for (int n = 0; n < 4; ++n) Frame();
    const ImGuiID input = GImGui->ActiveId;
    io.AddKeyEvent(ImGuiMod_Ctrl, true); Press(ImGuiKey_A);
    io.AddKeyEvent(ImGuiMod_Ctrl, false); Frame();
    for (ImGuiKey key : {ImGuiKey_W, ImGuiKey_A, ImGuiKey_S, ImGuiKey_D, ImGuiKey_H, ImGuiKey_J, ImGuiKey_K, ImGuiKey_L}) {
        io.AddInputCharacter((unsigned int)('a' + (key - ImGuiKey_A)));
        Press(key);
        CHECK(GImGui->ActiveId == input && !m.reqApplyEquip && arrowsClean(),
              "改名输入 %s：保留输入焦点、不套用、不伪造方向键", ImGui::GetKeyName(key));
    }
    Press(ImGuiKey_Enter);
    CHECK(c.sets[0].name == "wasdhjkl", "真实改名框完整保存 wasdhjkl");

    // Real search box: request focus at the actual InputText submission site.
    // A flattened child may inherit a different ID stack while being drawn.
    open(1); ui::DebugFocusSearch(); Frame(); Frame(); Frame();
    CHECK(io.WantTextInput && GImGui->ActiveId == GImGui->InputTextState.ID,
          "搜索框已激活文本输入");
    {
        const ImGuiID search = GImGui->ActiveId;
        CHECK(search != 0, "搜索框具有非空活动 ID，不能跳过字母验证");
        for (ImGuiKey key : {ImGuiKey_W, ImGuiKey_A, ImGuiKey_S, ImGuiKey_D, ImGuiKey_H, ImGuiKey_J, ImGuiKey_K, ImGuiKey_L}) {
            io.AddInputCharacter((unsigned int)('a' + (key - ImGuiKey_A))); Press(key);
            CHECK(GImGui->ActiveId == search && ui::DebugPage() == 1 && arrowsClean(), "搜索输入 %s 不导航", ImGui::GetKeyName(key));
        }
        CHECK(ui::DebugSearch() == "wasdhjkl", "真实搜索框接收完整 wasdhjkl，字母验证没有被跳过");
        Press(ImGuiKey_Escape);
    }
    // Capture receives only physical VK letters; Ctrl+A must remain a shortcut, not left.
    open(6); ui::DebugFocusHotkey(false); Frame(); Frame();
    ui::DebugStartCapture(m, 1, false); Hold(0);
    const ImGuiID captureFocus = GImGui->NavId;
    io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true);
    Hold(0, {VK_CONTROL, 'A'});
    CHECK(GImGui->NavId == captureFocus && arrowsClean() && ui::DebugPage() == 6,
          "快捷键录入 Ctrl+A 不导航、不污染方向键");
    io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); Hold(0);
    CHECK(c.hkPanel.find("Ctrl+A") != std::string::npos && c.hkPanel.find("Left") == std::string::npos,
          "录入保存真实 Ctrl+A，无伪 Left");
    for (bool pad : {false, true}) {
        ui::DebugStartCapture(m, 1, pad); Hold(0);
        const ImGuiID before = GImGui->NavId;
        for (ImGuiKey key : {ImGuiKey_W, ImGuiKey_A, ImGuiKey_S, ImGuiKey_D, ImGuiKey_H, ImGuiKey_J, ImGuiKey_K, ImGuiKey_L}) {
            Press(key);
            CHECK(GImGui->NavId == before && ui::DebugPage() == 6 && arrowsClean(),
                  "%s录入期间 %s 不导航", pad ? "手柄" : "键盘", ImGui::GetKeyName(key));
        }
        Press(ImGuiKey_Escape);
    }
    // Holding a letter across capture end must not suddenly start repeating navigation.
    open(6); ui::DebugStartCapture(m, 1, false); Hold(0);
    io.AddKeyEvent(ImGuiKey_S, true); Frame();
    Press(ImGuiKey_Escape); const ImGuiID ended = GImGui->NavId;
    for (int n = 0; n < 50; ++n) Frame();
    CHECK(GImGui->NavId == ended, "录入结束后仍按住的 S 不误开始导航，必须先松开");
    io.AddKeyEvent(ImGuiKey_S, false); Frame();
    open(); const ImGuiID focus = GImGui->NavId;
    io.AddKeyEvent(ImGuiMod_Ctrl, true); Press(ImGuiKey_S); io.AddKeyEvent(ImGuiMod_Ctrl, false); Frame();
    CHECK(GImGui->NavId == focus, "Ctrl+S 不作为导航字母");
    m.cfg = original; ui::OnOpen(m); for (int n = 0; n < 5; ++n) Frame();
}

static void TestSetSummary()
{
    const i18n::Lang before = i18n::Current();
    i18n::Set(i18n::kEn);
    Config c;
    LoadoutSet s;
    s.id = 41; s.name = "same"; s.equip = "7"; s.item = "#7"; s.food = 19;
    c.sets.push_back(s);
    FoodPreset meal;
    meal.name = "Dinner";
    c.foods[19] = meal;
    ui::Model model;
    model.cfg = &c;
    model.equipNames = {{2, "Other gear", 2}, {7, "Game gear", 7}};
    model.itemNames = {{7, "Other items", 2}, {2, "Game items", 7}};
    auto summary = [&]() { return ui::DebugSetSummary(model, 0); };
    CHECK(summary() == "Equip 7  Game gear · Items 2  Game items · Dinner",
          "摘要使用游戏名字、预设名字；道具 #7 存放格显示为第 2 项");
    model.equipNames[1].name = "Renamed gear";
    model.itemNames[1].name = "Renamed items";
    CHECK(summary() == "Equip 7  Renamed gear · Items 2  Renamed items · Dinner" &&
          c.sets[0].equip == "7" && c.sets[0].item == "#7" && !model.reqSave,
          "游戏名单更新后摘要立即显示新名，不改稳定引用或请求存盘");
    model.equipNames[1].name = "Game gear";
    model.itemNames[1].name = "Game items";
    std::swap(model.itemNames[0], model.itemNames[1]);
    std::swap(model.equipNames[0], model.equipNames[1]);
    CHECK(summary() == "Equip 7  Game gear · Items 2  Game items · Dinner",
          "名单 vector 顺序不影响按存放格找到的名称");
    model.itemNames[0].number = 8; // store 7 移到游戏显示位置 8，引用仍指向 store 7
    CHECK(summary() == "Equip 7  Game gear · Items 8  Game items · Dinner",
          "游戏内道具重排后显示新编号，仍跟随相同存放格");
    c.sets[0].item = "7";
    CHECK(summary() == "Equip 7  Game gear · Items 7  Other items · Dinner",
          "无 # 的兼容编号按显示编号读取，不能错当存放格");

    c.sets[0].item = "#7";
    model.itemNames[0].name = model.itemNames[1].name = "Same items";
    model.equipNames[0].name = model.equipNames[1].name = "Same gear";
    c.foods[3] = meal;
    c.foodOrder = {3, 19};
    c.foods[19].deleted = true;
    CHECK(summary() == "Equip 7  Same gear · Items 8  Same items · Dinner (deleted)",
          "同名装备/道具/猫饭仍按 ID 选择，墓碑不冒充另一份同名活猫饭");
    CHECK(c.sets[0].equip == "7" && c.sets[0].item == "#7" && c.sets[0].food == 19 &&
          c.foods[19].deleted && c.foodOrder == std::vector<int>({3, 19}) && !model.reqSave,
          "重排、同名和墓碑摘要不会改写稳定引用或猫饭顺序");
    c.foods[19].name.clear();
    CHECK(summary() == "Equip 7  Same gear · Items 8  Same items · Meal 19 (deleted)",
          "无名墓碑回退到稳定编号和已删除标记");
    c.foods[19].deleted = false;
    CHECK(summary() == "Equip 7  Same gear · Items 8  Same items · Meal 19",
          "无名猫饭回退到预设编号，不使用显示顺序");
    c.foods.erase(19);
    CHECK(summary() == "Equip 7  Same gear · Items 8  Same items · Meal 19 (missing)",
          "猫饭引用缺失时明确标记，不借用同名或其它预设");

    model.equipNames[0].name.clear(); // store 7
    model.itemNames[0].name.clear();  // store 7 / display 8
    c.sets[0].food = -1;
    CHECK(summary() == "Equip 7 · Items 8", "读到的名字为空时保留正确编号，无空白尾缀");
    model.equipNames.clear();
    model.itemNames.clear();
    CHECK(summary() == "Equip 7 · Items #7", "名单尚未读取时保留原始装备号与 #存放格");
    model.equipNames = {{2, "Other gear", 2}};
    model.itemNames = {{7, "Other items", 2}};
    CHECK(summary() == "Equip 7 · Items #7", "缺失存放格不误匹配同号显示项");
    model.itemNames.push_back({8, "Game items", 7});
    c.sets[0].item = "#7junk";
    CHECK(summary() == "Equip 7 · Items #7junk", "畸形存放格不被 atoi 截成有效编号");
    c.sets[0].equip = "Other gear";
    c.sets[0].item = "Other items";
    CHECK(summary() == "Equip Other gear · Items Other items", "旧名字引用只原样展示，绝不按名字解析");
    CHECK(c.sets[0].id == 41 && c.sets[0].equip == "Other gear" && c.sets[0].item == "Other items" &&
          c.sets[0].food == -1 && !model.reqSave && c.foods.count(19) == 0,
          "生成摘要不修改引用、创建缺失猫饭或请求保存");
    c.sets[0].equip.clear(); c.sets[0].item.clear();
    CHECK(summary() == "empty set", "空套装回退保持不变");
    CHECK(ui::DebugSetSummary(model, -1).empty() && ui::DebugSetSummary(model, 1).empty(),
          "测试入口对越界索引安全返回");
    FoodPreset f;
    f.skill[0] = 32; f.skill[1] = 9; f.skill[2] = 39;
    CHECK(ui::DebugFoodSkillsSummary(f) == "Moxie · Defender Hi · Booster", "猫饭英文主行按三个技能槽位顺序显示短名");
    i18n::Set(i18n::kZh);
    CHECK(ui::DebugFoodSkillsSummary(f) == "大毅力 · 防御大 · 短催", "猫饭中文主行即时切换，不依赖预设名称");
    i18n::Set(i18n::kJa);
    CHECK(ui::DebugFoodSkillsSummary(f) == "ド根性 · 防御大 · 短期催眠", "猫饭日文主行即时切换");
    f.skill[0] = -1; f.skill[1] = 37; f.skill[2] = 999;
    CHECK(ui::DebugFoodSkillsSummary(f) == "なし · ? · ?", "空技能槽与未知技能保持位置，不冒充有效技能");
    i18n::Set(before);
}


// Real ImGui IO: cancellation, keyboard/gamepad, mouse, localization and references.
static void TestDeleteModal()
{
    Config* original = m.cfg;
    const auto lang = i18n::Current();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec4 dim = ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg];
    Config c;
    LoadoutSet set; set.id = 41; set.name = "Delete me"; set.food = 19;
    c.sets.push_back(set); c.activeSet = 41; c.food = 19;
    c.foods[19] = FoodPreset(); c.foods[19].name = "Referenced meal";
    c.foodOrder = {19}; m.cfg = &c;
    auto settle = [&] { for (int n = 0; n < 6; ++n) Frame(); };
    auto edit = [&](int page) {
        ui::OnOpen(m); ui::DebugGo(page, page == 3 ? 0 : 19); settle();
        m.reqSave = m.reqApplyEquip = m.reqClose = false;
    };
    auto popup = [&]() -> ImGuiWindow* {
        for (auto* w : GImGui->Windows)
            if (w->Active && (w->Flags & ImGuiWindowFlags_Modal) && std::strstr(w->Name, "confirm_del")) return w;
        return nullptr;
    };
    auto open = [&] { ui::DebugOpenDelete(); settle(); CHECK(popup(), "delete modal opens"); };
    auto click = [&](bool confirm) {
        auto* w = popup(); if (!w) return;
        const float scale = ImGui::GetIO().DisplaySize.y / 1080.0f;
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(w->Pos.x + w->Size.x * (confirm ? 0.72f : 0.28f),
                            w->Pos.y + w->Size.y - w->WindowPadding.y - 22 * scale);
        Frame(); io.AddMouseButtonEvent(0, true); Frame(); io.AddMouseButtonEvent(0, false); settle();
        io.AddMousePosEvent(-100, -100); Frame();
    };
    edit(3);
    // Activate the real page button with A, not the debug hook.
    for (auto* w : GImGui->Windows) if (w->Active && std::strstr(w->Name, "##content")) {
        ImGui::SetFocusID(w->GetID("##del"), w); ImGui::SetNavCursorVisible(true); break;
    }
    Press(ImGuiKey_GamepadFaceDown);
    CHECK(popup() && c.sets.size() == 1, "real A opens card without same-input cancellation/deletion");
    Press(ImGuiKey_GamepadFaceDown);
    CHECK(!popup() && c.sets.size() == 1 && ui::DebugPage() == 3 && !m.reqSave, "default A cancels without saving");
    for (auto key : {ImGuiKey_GamepadFaceRight, ImGuiKey_Escape}) {
        open(); Press(key);
        CHECK(!popup() && ui::DebugPage() == 3 && !m.reqClose && !m.reqSave, "B/Esc closes modal only key=%d", key);
    }
    open(); click(false);
    CHECK(!popup() && c.sets.size() == 1 && !m.reqSave, "mouse cancel preserves data");
    open(); Press(ImGuiKey_GamepadDpadRight);
    auto& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true); io.AddKeyEvent(ImGuiKey_Escape, true); Frame();
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, false); io.AddKeyEvent(ImGuiKey_Escape, false); settle();
    CHECK(!popup() && c.sets.size() == 1 && !m.reqSave, "cancel wins over simultaneous delete activation");
    // Pending test-only requests must not survive reopening the panel.
    ui::DebugOpenDelete(); edit(3);
    CHECK(!popup(), "OnOpen clears stale DebugOpenDelete request");
    for (int height : {1080, 720}) {
        io.DisplaySize = ImVec2(height * 16.0f / 9.0f, (float)height); ui::Init(height / 1080.0f);
        for (auto language : {i18n::kZh, i18n::kEn, i18n::kJa}) {
            i18n::Set(language);
            c.sets[0].name.clear();
            const char* ch = language == i18n::kZh ? "长" : language == i18n::kJa ? "あ" : "W";
            for (int i = 0; i < 50; ++i) c.sets[0].name += ch;
            edit(3); open();
            auto* w = popup();
            CHECK(w && w->Pos.x >= 0 && w->Pos.y >= 0 && w->Pos.x + w->Size.x <= io.DisplaySize.x &&
                  w->Pos.y + w->Size.y <= io.DisplaySize.y && w->ScrollMax.x == 0 && w->ScrollMax.y == 0,
                  "50-character localized title wraps without clipping %dp lang=%d", height, language);
            CHECK(w && w->Size.y > 250 * height / 1080.0f && GImGui->NavId == w->GetID("##no"),
                  "wrapped modal keeps cancel focused %dp lang=%d", height, language);
            Press(ImGuiKey_Escape);
        }
    }
    io.DisplaySize = display; ui::Init(1.0f); i18n::Set(lang);
    edit(5); open(); click(true);
    CHECK(c.foods[19].deleted && c.food == 19 && c.sets[0].food == 19 && c.activeSet == 41 &&
          ui::FoodRowIds(c) == std::vector<int>({19}) && m.reqSave && !m.reqApplyEquip && ui::DebugPage() == 4,
          "mouse delete meal retains referenced tombstone/stable IDs and saves");
    edit(3); open(); Press(ImGuiKey_GamepadDpadRight); Press(ImGuiKey_GamepadFaceDown);
    CHECK(c.sets.empty() && c.activeSet == -1 && c.food == 19 && c.foods[19].deleted &&
          m.reqSave && m.reqApplyEquip && ui::DebugPage() == 2,
          "right then A deletes active set, clears active reference and requests equip update");
    CHECK(std::memcmp(&dim, &ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg], sizeof(dim)) == 0,
          "delete style leaves global modal dim unchanged");
    m.cfg = original; ui::OnOpen(m); settle();
}

static void CheckListHintControls()
{
    const int source = ui::DebugHintSource();
    const char* rename = i18n::Tr("改名", "Rename", "改名");
    bool hasRename = false, secondaryCorrect = true;
    for (const auto& item : ui::DebugHintItems()) {
        hasRename |= item.find(std::string(source == 0 ? "X " : "F ") + rename) != std::string::npos;
        if (item.find("LB/RB") != std::string::npos)
            secondaryCorrect &= source == 0 && (item.find("Shift") != std::string::npos || item.find("换位") != std::string::npos || item.find("並替") != std::string::npos);
        if (item.find("L3 ") != std::string::npos)
            secondaryCorrect &= source == 0 && (item.find("Move") != std::string::npos || item.find("移动") != std::string::npos || item.find("移動") != std::string::npos);
    }
    CHECK((source == 2 || hasRename) && secondaryCorrect,
          "focused list retains real rename; optional shift/move controls stay truthful source=%d lang=%d page=%d",
          source, (int)i18n::Current(), ui::DebugPage());
}

// Validate the reserved strip itself, not a generous fraction of screen height:
// 110*S used to pass that check while wasting several lines on every page.
static void CheckCompactFooter(const char* context)
{
    const float height = ImGui::GetIO().DisplaySize.y;
    const float requestedScale = height / 1080.0f;
    const float scale = requestedScale < 0.7f ? 0.7f : (requestedScale > 2.5f ? 2.5f : requestedScale);
    CHECK(ui::DebugHintHeight() > 0 && ui::DebugHintHeight() <= 32.0f * scale + 0.5f,
          "compact footer <=32*S context=%s source=%d page=%d lang=%d %dp (%.1f)",
          context, ui::DebugHintSource(), ui::DebugPage(), (int)i18n::Current(), (int)height, ui::DebugHintHeight());
    float top = 0, bottom = 0, contentBottom = 0;
    CHECK(ui::DebugHintLayout(top, bottom, contentBottom) && top >= contentBottom - 0.5f &&
          top >= 0 && bottom > top && bottom - top <= 32.0f * scale + 0.5f && bottom <= height + 0.5f,
          "footer outside content/on screen context=%s source=%d page=%d lang=%d %dp (%.1f/%.1f/%.1f)",
          context, ui::DebugHintSource(), ui::DebugPage(), (int)i18n::Current(), (int)height, contentBottom, top, bottom);
    const auto bounds = ui::DebugHintBounds();
    const auto items = ui::DebugHintItems();
    CHECK(!items.empty() && bounds.rows == ui::DebugHintRows() && bounds.rows == 1,
          "actual rendered footer has one row context=%s source=%d page=%d lang=%d %dp rows=%d items=%d",
          context, ui::DebugHintSource(), ui::DebugPage(), (int)i18n::Current(), (int)height, bounds.rows, (int)items.size());
    bool atomicItems = true, conciseNavigation = true;
    for (const auto& item : items) {
        atomicItems &= !item.empty() && item.find_first_of("\r\n") == std::string::npos;
        conciseNavigation &= item.find("Arrows") == std::string::npos &&
                             item.find("HJKL") == std::string::npos && item.find("Tab") == std::string::npos;
    }
    CHECK(conciseNavigation,
          "actual footer never advertises Arrows/HJKL/Tab context=%s source=%d page=%d lang=%d %dp",
          context, ui::DebugHintSource(), ui::DebugPage(), (int)i18n::Current(), (int)height);
    CHECK(atomicItems && bounds.availableLeft >= 0 && bounds.availableRight <= ImGui::GetIO().DisplaySize.x + 0.5f &&
          bounds.availableRight > bounds.availableLeft && bounds.textRight > bounds.textLeft &&
          bounds.textLeft >= bounds.availableLeft - 0.5f && bounds.textRight <= bounds.availableRight + 0.5f &&
          bounds.textTop >= top - 0.5f && bounds.textBottom > bounds.textTop && bounds.textBottom <= bottom + 0.5f,
          "actual unwrapped key/action groups fit footer context=%s source=%d page=%d lang=%d %dp text=(%.1f,%.1f)-(%.1f,%.1f)",
          context, ui::DebugHintSource(), ui::DebugPage(), (int)i18n::Current(), (int)height,
          bounds.textLeft, bounds.textTop, bounds.textRight, bounds.textBottom);
    const auto rows = ui::DebugHintRowBounds();
    CHECK(rows.size() == 1 && rows[0].groupCount == static_cast<int>(items.size()) &&
          rows[0].right > rows[0].left && rows[0].bottom > rows[0].top &&
          rows[0].left >= bounds.availableLeft - 0.5f && rows[0].right <= bounds.availableRight + 0.5f &&
          rows[0].top >= top - 0.5f && rows[0].bottom <= bottom + 0.5f,
          "single unwrapped row contains every actual group without clipping context=%s source=%d page=%d lang=%d %dp",
          context, ui::DebugHintSource(), ui::DebugPage(), (int)i18n::Current(), (int)height);
    bool anchored = bounds.leftActionCount >= 0 && bounds.rightActionCount >= 0 &&
                    bounds.leftActionCount + bounds.rightActionCount == static_cast<int>(items.size());
    if (bounds.leftActionCount > 0)
        anchored &= std::fabs(bounds.textLeft - bounds.availableLeft) <= 1.0f &&
                    bounds.leftActionsRight > bounds.availableLeft && bounds.leftActionsRight <= bounds.availableRight + 0.5f;
    if (bounds.rightActionCount > 0)
        anchored &= std::fabs(bounds.textRight - bounds.availableRight) <= 1.0f &&
                    bounds.rightActionsLeft >= bounds.availableLeft - 0.5f && bounds.rightActionsLeft < bounds.availableRight;
    if (bounds.leftActionCount > 0 && bounds.rightActionCount > 0)
        anchored &= bounds.rightActionsLeft - bounds.leftActionsRight >= 12.0f * scale - 0.5f;
    CHECK(anchored,
          "left actions/right navigation anchored with nonoverlapping 12*S partition gap context=%s source=%d page=%d lang=%d %dp",
          context, ui::DebugHintSource(), ui::DebugPage(), (int)i18n::Current(), (int)height);
    const auto icons = ui::DebugHintIcons();
    CHECK(icons.keycaps >= 0 && icons.padButtons >= 0 && icons.shoulders >= 0 && icons.directional >= 0 &&
          icons.specialGlyphs == 0 && icons.maxCenterError >= 0 && icons.maxCenterError <= 1.0f,
          "draw-list geometric controls vertically centered without special glyphs context=%s source=%d page=%d lang=%d %dp",
          context, ui::DebugHintSource(), ui::DebugPage(), (int)i18n::Current(), (int)height);
    CHECK(bounds.separatorRight > bounds.separatorLeft && bounds.separatorLeft >= bounds.availableLeft - 0.5f &&
          bounds.separatorRight <= bounds.availableRight + 0.5f &&
          bounds.sidebarBottom + scale <= bounds.separatorY + 0.01f &&
          bounds.separatorY >= contentBottom - 0.5f && bounds.separatorY + scale <= bounds.textTop + 0.01f,
          "sidebar stops before footer separator; text clears rule context=%s source=%d page=%d lang=%d %dp",
          context, ui::DebugHintSource(), ui::DebugPage(), (int)i18n::Current(), (int)height);
}

static bool DisplayedHintContains(const char* text)
{
    for (const auto& item : ui::DebugHintItems())
        if (item.find(text) != std::string::npos) return true;
    return false;
}

// Exercise the production ImGui event hook, not a debug source setter.  In
// particular, virtual navigation generated from WASD must remain keyboard input.
static void TestContextHints()
{
    Config* original = m.cfg;
    const auto language = i18n::Current();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 display = io.DisplaySize;
    const bool trickle = io.ConfigInputTrickleEventQueue;
    io.ConfigInputTrickleEventQueue = false;
    Config c;
    for (int id = 1; id <= 20; ++id) {
        LoadoutSet s; s.id = id; s.name = "Hint row " + std::to_string(id);
        c.sets.push_back(s); c.foods[id] = FoodPreset(); c.foodOrder.push_back(id);
    }
    c.activeSet = 1; c.food = 1; m.cfg = &c;
    i18n::Set(i18n::kEn);
    auto settle = [&] { for (int n = 0; n < 45; ++n) Frame(); };
    auto open = [&](int page) {
        ui::OnOpen(m); ui::DebugGo(page, page == 5 ? 1 : 0); settle();
        m.reqClose = m.reqRestart = false;
    };
    auto key = [&] { Press(ImGuiKey_Q); };
    auto pad = [&] { Press(ImGuiKey_GamepadR2); };
    auto wheel = [&] { io.AddMouseWheelEvent(0, -1); Frame(); };
    open(6);
    key(); CHECK(ui::DebugHintSource() == 1, "real keyboard event selects keyboard hints");
    pad(); CHECK(ui::DebugHintSource() == 0, "real gamepad event selects gamepad hints");
    key(); CHECK(ui::DebugHintSource() == 1, "keyboard retakes hints from pad");
    io.AddMousePosEvent(20, 20); Frame();
    key();
    for (int n = 0; n < 30; ++n) { io.AddMousePosEvent(20.0f + static_cast<float>(n % 2), 20.0f + static_cast<float>(n % 3)); Frame(); }
    CHECK(ui::DebugHintSource() == 1, "sub-threshold oscillating mouse jitter never steals hints");
    io.AddMousePosEvent(55, 45); Frame();
    CHECK(ui::DebugHintSource() == 2, "meaningful real mouse movement selects mouse hints");
    pad(); wheel();
    CHECK(ui::DebugHintSource() == 2, "wheel without mouse movement selects mouse hints");
    key(); io.AddMouseButtonEvent(0, true); Frame();
    CHECK(ui::DebugHintSource() == 2, "mouse click without movement selects mouse hints");
    key(); io.AddMouseButtonEvent(0, false); Frame();
    CHECK(ui::DebugHintSource() == 1, "mouse button release never steals keyboard hints");
    for (float value : {0.01f, 0.08f, 0.15f, 0.01f}) {
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, true, value); Frame();
    }
    CHECK(ui::DebugHintSource() == 1, "gamepad analog noise below threshold never steals keyboard hints");
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, false, 0); Frame();
    pad(); io.AddInputCharacter('q'); Frame();
    CHECK(ui::DebugHintSource() == 1, "character-only input selects keyboard hints");
    io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, true, 0.8f); Frame();
    CHECK(ui::DebugHintSource() == 0, "fresh gamepad analog edge selects gamepad hints");
    key();
    for (int n = 0; n < 50; ++n) { io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, true, 0.8f); Frame(); }
    CHECK(ui::DebugHintSource() == 1, "held gamepad analog cannot reclaim keyboard hints");
    wheel();
    for (int n = 0; n < 10; ++n) Frame();
    CHECK(ui::DebugHintSource() == 2, "held gamepad cannot reclaim mouse hints");
    io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, false, 0); Frame();
    CHECK(ui::DebugHintSource() == 2, "gamepad release does not steal mouse source");
    io.AddKeyEvent(ImGuiKey_Q, true); io.AddKeyEvent(ImGuiKey_GamepadR2, true); Frame();
    CHECK(ui::DebugHintSource() == 0, "same-frame key then pad uses last genuine event");
    io.AddKeyEvent(ImGuiKey_Q, false); io.AddKeyEvent(ImGuiKey_GamepadR2, false); Frame();
    io.AddKeyEvent(ImGuiKey_GamepadR2, true); io.AddKeyEvent(ImGuiKey_Q, true); Frame();
    CHECK(ui::DebugHintSource() == 1, "same-frame pad then key uses last genuine event");
    io.AddKeyEvent(ImGuiKey_GamepadR2, false); io.AddKeyEvent(ImGuiKey_Q, false); Frame();
    io.AddKeyEvent(ImGuiKey_GamepadR2, true); io.AddMouseWheelEvent(0, 1); Frame();
    CHECK(ui::DebugHintSource() == 2, "same-frame pad then wheel uses last genuine event");
    io.AddKeyEvent(ImGuiKey_GamepadR2, false); Frame();
    io.AddMouseWheelEvent(0, -1); io.AddKeyEvent(ImGuiKey_Q, true); Frame();
    CHECK(ui::DebugHintSource() == 1, "same-frame wheel then key uses last genuine event");
    io.AddKeyEvent(ImGuiKey_Q, false); Frame();
    open(2); Press(ImGuiKey_S);
    CHECK(ui::DebugHintSource() == 1, "WASD synthetic navigation stays keyboard source");

    for (int height : {720, 1080}) {
        io.DisplaySize = ImVec2(height * 16.0f / 9.0f, (float)height);
        ui::Init(height / 1080.0f);
        for (auto lang : {i18n::kZh, i18n::kEn, i18n::kJa}) {
            i18n::Set(lang);
            for (int page = 0; page < 7; ++page) {
                open(page);
                key(); const std::string keyboard = ui::DebugHintText(); CheckCompactFooter("page keyboard");
                const auto keyboardItems = ui::DebugHintItems();
                if (page == 2 || page == 4) {
                    CheckListHintControls();
                    const auto icons = ui::DebugHintIcons();
                    CHECK(DisplayedHintContains("WASD") && icons.keycaps > 0 &&
                          icons.padButtons == 0 && icons.shoulders == 0,
                          "keyboard list draws WASD keycaps %dp lang=%d page=%d", height, lang, page);
                }
                pad(); const std::string gamepad = ui::DebugHintText(); CheckCompactFooter("page gamepad");
                const auto gamepadItems = ui::DebugHintItems();
                if (page == 2 || page == 4) {
                    CheckListHintControls();
                    const auto icons = ui::DebugHintIcons();
                    CHECK(icons.padButtons > 0 && icons.directional > 0,
                          "gamepad list draws face buttons and D-pad %dp lang=%d page=%d", height, lang, page);
                }
                wheel(); const std::string mouse = ui::DebugHintText(); CheckCompactFooter("page mouse");
                CHECK(keyboardItems != gamepadItems && ui::DebugHintItems() != gamepadItems,
                      "actual displayed groups react to all real sources %dp lang=%d page=%d", height, lang, page);
                CHECK(!keyboard.empty() && !gamepad.empty() && !mouse.empty() &&
                      keyboard != gamepad && mouse != gamepad,
                      "all page footers react to real sources %dp lang=%d page=%d", height, lang, page);
            }
        }
    }
    i18n::Set(i18n::kEn);
    open(0); Press(ImGuiKey_GamepadDpadLeft);
    CHECK(ui::DebugSidebarFocused() && ui::DebugHintText().find("switch page") != std::string::npos &&
          ui::DebugHintText().find("reorder") == std::string::npos,
          "sidebar hints describe switching pages, not content row actions");
    open(1); ui::DebugFocusSearch(); Frame(); io.AddInputCharacter('z'); Frame();
    CHECK(ui::DebugHintSource() == 1 && ui::DebugHintText().find("Type") != std::string::npos &&
          ui::DebugHintText().find("WASD") == std::string::npos,
          "active search describes typing, never letter navigation");
    Press(ImGuiKey_Escape); settle();
    open(2); key(); const std::string list = ui::DebugHintText();
    CheckCompactFooter("keyboard list");
    CHECK((DisplayedHintContains("Enter Apply") || DisplayedHintContains("Enter Off")) && DisplayedHintContains("Y Edit") &&
          DisplayedHintContains("WASD") && !DisplayedHintContains("Arrows") &&
          !DisplayedHintContains("HJKL") && !DisplayedHintContains("Tab") &&
          DisplayedHintContains("F Rename"),
          "actual keyboard list retains rename alongside primary apply/edit and WASD");
    CHECK(list.find("F rename") != std::string::npos && list.find("Ctrl+Up/Down") != std::string::npos &&
          list.find("LB / RB") == std::string::npos, "keyboard row hints use real keyboard rename/reorder controls only");
    pad();
    CheckCompactFooter("gamepad list");
    CHECK((DisplayedHintContains("A Apply") || DisplayedHintContains("A Off")) && DisplayedHintContains("Y Edit") &&
          DisplayedHintContains("D-pad") && DisplayedHintContains("X Rename") && !DisplayedHintContains("Ctrl+"),
          "actual gamepad list retains rename alongside primary apply/edit and D-pad");
    CHECK(ui::DebugHintText().find("X rename") != std::string::npos &&
          ui::DebugHintText().find("LB / RB") != std::string::npos && ui::DebugHintText().find("F rename") == std::string::npos,
          "gamepad row hints use real pad rename/reorder controls only");
    key();
    Press(ImGuiKey_E); const std::string moving = ui::DebugHintText();
    CheckCompactFooter("move mode");
    CHECK(DisplayedHintContains("Drop") && DisplayedHintContains("Finish") &&
          !DisplayedHintContains("Reorder") && !DisplayedHintContains("Cancel"),
          "actual move groups describe drop/finish instead of reorder/rollback");
    CHECK(ui::DebugMoveActive() && moving != list && !moving.empty(), "E move context replaces normal list actions immediately");
    CHECK(moving.find("drop") != std::string::npos && moving.find("reorder") == std::string::npos &&
          moving.find("cancel") == std::string::npos, "move hints describe drop/finish, never rollback or direct reorder");
    Press(ImGuiKey_Escape);
    CHECK(!ui::DebugMoveActive() && ui::DebugPage() == 2 && !m.reqClose,
          "move footer return means put down, not page back");
    ui::DebugOpenRename(); settle(); key();
    const std::string rename = ui::DebugHintText();
    CheckCompactFooter("rename modal");
    CHECK(DisplayedHintContains("Type name") && DisplayedHintContains("Confirm") &&
          !DisplayedHintContains("Reorder"), "actual rename groups replace underlying list actions");
    CHECK(!rename.empty() && rename != list && rename.find("rename") == std::string::npos &&
          rename.find("reorder") == std::string::npos, "rename footer suppresses underlying list shortcuts");
    Press(ImGuiKey_Escape); settle();
    open(3); key(); const std::string edit = ui::DebugHintText();
    ui::DebugOpenDelete(); settle(); key();
    CheckCompactFooter("delete modal");
    CHECK(DisplayedHintContains("Close") && !DisplayedHintContains("Reorder"),
          "actual delete modal groups replace underlying editor actions");
    CHECK(!ui::DebugHintText().empty() && ui::DebugHintText() != edit,
          "delete modal footer replaces underlying editor shortcuts");
    Press(ImGuiKey_Escape); settle();
    open(6); key(); const std::string settings = ui::DebugHintText();
    ui::DebugStartCapture(m, 0, false); Frame();
    CheckCompactFooter("keyboard capture");
    CHECK(DisplayedHintContains("Keys") && DisplayedHintContains("Release saves") &&
          !DisplayedHintContains("Navigate"), "actual keyboard capture groups replace settings navigation");
    CHECK(m.capturing && !ui::DebugHintText().empty() && ui::DebugHintText() != settings,
          "keyboard capture footer overrides settings navigation");
    Press(ImGuiKey_Escape); settle();
    ui::DebugStartCapture(m, 0, true); Frame(); pad();
    CheckCompactFooter("gamepad capture");
    CHECK(DisplayedHintContains("X Alone: clear") && !DisplayedHintContains("B Cancel"),
          "actual pad capture groups explain X clear without false B cancel");
    CHECK(m.capturing && ui::DebugHintText().find("X alone clears") != std::string::npos &&
          ui::DebugHintText().find("B cancel") == std::string::npos,
          "gamepad capture explains X clear, does not falsely promise B cancellation");
    Press(ImGuiKey_Escape); settle();
    open(6); ui::DebugFocusHotkey(false); Frame(); key();
    CheckCompactFooter("hotkey chip");
    CHECK(DisplayedHintContains("Capture"), "actual focused hotkey group advertises capture");
    CHECK(ui::DebugHintText().find("capture hotkey") != std::string::npos,
          "focused hotkey chip advertises capture");
    // Cross-context regression uses real D-pad navigation without OnOpen/Init
    // between targets: stale flags must be reset every frame, not on page open.
    for (int n = 0; n < 8 && !ui::DebugSidebarFocused(); ++n) Press(ImGuiKey_GamepadDpadLeft);
    CheckCompactFooter("hotkey-to-sidebar");
    CHECK(DisplayedHintContains("D-pad Nav") && !DisplayedHintContains("Capture"),
          "actual sidebar navigation clears previous hotkey context");
    CHECK(ui::DebugSidebarFocused() && ui::DebugHintText().find("switch page") != std::string::npos &&
          ui::DebugHintText().find("capture hotkey") == std::string::npos,
          "real hotkey-to-sidebar transition clears capture context");
    Press(ImGuiKey_GamepadDpadRight);
    for (int n = 0; n < 16 && ui::DebugHintText().find("adjust") == std::string::npos; ++n)
        Press(ImGuiKey_GamepadDpadDown);
    CheckCompactFooter("sidebar-to-slider");
    CHECK(DisplayedHintContains("Adjust") && !DisplayedHintContains("Capture"),
          "actual slider groups clear previous hotkey context");
    CHECK(ui::DebugHintText().find("adjust") != std::string::npos &&
          ui::DebugHintText().find("capture hotkey") == std::string::npos,
          "real sidebar-to-opacity navigation replaces hotkey with slider hints");
    for (int n = 0; n < 8 && !ui::DebugSidebarFocused(); ++n) Press(ImGuiKey_GamepadDpadLeft);
    Press(ImGuiKey_GamepadDpadUp); Press(ImGuiKey_GamepadDpadUp); // Settings -> Meals -> Loadouts
    Press(ImGuiKey_GamepadDpadRight);
    CHECK(ui::DebugPage() == 2 && ui::DebugHintText().find("X rename") != std::string::npos &&
          ui::DebugHintText().find("adjust") == std::string::npos &&
          ui::DebugHintText().find("capture hotkey") == std::string::npos,
          "real slider-to-loadout navigation clears slider/hotkey context");
    Press(ImGuiKey_GamepadFaceRight); // Loadouts -> Home
    CHECK(ui::DebugPage() == 0 && ui::DebugHintText().find("rename") == std::string::npos &&
          ui::DebugHintText().find("adjust") == std::string::npos,
          "real list-to-home back clears list and slider actions");
    Press(ImGuiKey_GamepadDpadDown); Press(ImGuiKey_GamepadDpadLeft); Press(ImGuiKey_GamepadFaceDown);
    CHECK(ui::DebugPage() == 1 && ui::DebugHintText().find("rename") == std::string::npos &&
          ui::DebugHintText().find("adjust") == std::string::npos &&
          ui::DebugHintText().find("capture hotkey") == std::string::npos,
          "real home-to-quest navigation leaves no sticky action context");
    open(0); key();
    CHECK(ui::DebugHintText().find("capture hotkey") == std::string::npos,
          "hotkey context cannot leak into home on the next frame");
    c.enabled = true; open(2); key(); const std::string loaded = ui::DebugHintText();
    c.enabled = false; Frame();
    CheckListHintControls();
    pad(); CheckListHintControls(); CheckCompactFooter("disabled pad list"); key();
    CHECK(!ui::DebugHintText().empty() && ui::DebugHintText() != loaded,
          "Dormant list footer does not promise disabled apply action");
    // Reach Meals through actual sidebar navigation. Noncontiguous preset IDs
    // make an accidental row-index/id mixup visible; locked/dormant only block A.
    const Config beforeMealStates = c;
    for (int state = 0; state < 4; ++state) {
        c = Config();
        LoadoutSet set; set.id = 17; set.name = "Meal focus fixture";
        set.food = state == 2 ? 41 : -1;
        c.sets.push_back(set); c.activeSet = 17;
        c.foods[41] = FoodPreset(); c.foods[73] = FoodPreset();
        c.foods[41].name = "First meal"; c.foods[73].name = "Second meal";
        c.foodOrder = {41, 73}; c.food = state == 0 ? 41 : -1;
        c.enabled = state != 3;
        open(0);
        Press(ImGuiKey_GamepadDpadLeft);
        Press(ImGuiKey_GamepadDpadDown); Press(ImGuiKey_GamepadDpadDown);
        CHECK(ui::DebugSidebarFocused() && ui::DebugSidebarFocusIndex() == 2 && ui::DebugPage() == 4,
              "real sidebar reaches Meals state=%d", state);
        io.AddKeyEvent(ImGuiKey_GamepadDpadRight, true); Frame();
        CHECK(!ui::DebugSidebarFocused() && DisplayedHintContains("Y Edit") && DisplayedHintContains("X Rename"),
              "sidebar Right first draw immediately exposes existing meal row focus state=%d", state);
        io.AddKeyEvent(ImGuiKey_GamepadDpadRight, false); Frame(); settle();
        CHECK(!ui::DebugSidebarFocused() && ui::DebugPage() == 4 &&
              DisplayedHintContains("Y Edit") && DisplayedHintContains("X Rename"),
              "selected/unselected/locked/dormant Meals retains Y edit and X rename state=%d", state);
        CheckCompactFooter("real sidebar Meals states");
        if (state >= 2) {
            CHECK(!DisplayedHintContains("A Apply") && !DisplayedHintContains("A Off"),
                  "locked/dormant Meals suppresses only apply action state=%d", state);
            const int fallback = c.food;
            Press(ImGuiKey_GamepadFaceDown);
            CHECK(c.food == fallback && DisplayedHintContains("Y Edit") && DisplayedHintContains("X Rename"),
                  "blocked A cannot change meal or hide editing state=%d", state);
        }
        const int fallback = c.food;
        Press(ImGuiKey_GamepadFaceUp);
        CHECK(ui::DebugPage() == 5 && c.food == fallback,
              "real Y enters food editor without changing selection state=%d", state);
        if (state == 0) {
            Press(ImGuiKey_GamepadFaceRight); settle();
            Press(ImGuiKey_GamepadL3); settle();
            CHECK(ui::DebugPage() == 4 && ui::DebugMoveActive() && ui::DebugMoveId() == 41 &&
                  ui::DebugHintIcons().stickPress > 0 && DisplayedHintContains("Drop"),
                  "real Meals LS press enters moving actual stable ID with geometric press cue");
            Press(ImGuiKey_GamepadFaceRight);
        }
    }
    c = beforeMealStates;
    io.ConfigInputTrickleEventQueue = trickle;
    io.DisplaySize = display; ui::Init(display.y / 1080.0f); i18n::Set(language);
    m.cfg = original; m.padHeld = 0; m.keysHeld.clear(); ui::OnOpen(m); settle();
}

static void TestFoodPreviewNavigation()
{
    const ui::Model before = m;
    Config* original = m.cfg;
    const auto language = i18n::Current();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    Config c;
    LoadoutSet set; set.id = 17; set.name = "Preview set"; set.food = 41;
    c.sets.push_back(set); c.activeSet = 17; c.food = 73;
    FoodPreset meal; meal.name = "Preview meal"; meal.hp = 5; meal.stamina = 2;
    meal.skill[0] = 32; meal.skill[1] = 9; meal.skill[2] = 39;
    c.foods[41] = meal; meal.name = "Candidate meal"; c.foods[73] = meal; c.foodOrder = {41, 73};
    m.cfg = &c;
    auto checkPreview = [&](bool popup, int fid) {
        const auto b = ui::DebugFoodPreviewBounds();
        const std::string text = ui::DebugFoodPreviewText();
        CHECK(b.visible && b.fromPopup == popup && b.foodId == fid &&
              b.left >= 0 && b.top >= 0 && b.right > b.left && b.bottom > b.top &&
              b.right <= ImGui::GetIO().DisplaySize.x + 0.5f && b.bottom <= ImGui::GetIO().DisplaySize.y + 0.5f &&
              (b.right <= b.anchorLeft + 0.5f || b.left >= b.anchorRight - 0.5f ||
               b.bottom <= b.anchorTop + 0.5f || b.top >= b.anchorBottom - 0.5f),
              "meal preview real anchor/screen bounds want popup=%d id=%d lang=%d %dp actual visible=%d popup=%d id=%d rect=(%.1f,%.1f)-(%.1f,%.1f) anchor=(%.1f,%.1f)-(%.1f,%.1f) nav=%08X active=%08X",
              popup, fid, (int)i18n::Current(), (int)ImGui::GetIO().DisplaySize.y,
              b.visible, b.fromPopup, b.foodId, b.left, b.top, b.right, b.bottom,
              b.anchorLeft, b.anchorTop, b.anchorRight, b.anchorBottom, GImGui->NavId, GImGui->ActiveId);
        CHECK(text.find(i18n::Tr("体力+50 耐力+50", "HP+50 Stam+50", "体力+50 スタミナ+50")) != std::string::npos &&
              text.find(ui::DebugFoodSkillsSummary(c.foods[fid])) != std::string::npos,
              "meal preview contains HP/stamina bonus and all three ordered localized short skills");
        const ImGuiID focus = GImGui->NavId;
        for (int n = 0; n < 3; ++n) Frame();
        CHECK(GImGui->NavId == focus && c.sets[0].food == 41 && c.food == 73 && !m.reqSave && !m.reqApplyEquip,
              "foreground meal preview never steals nav or mutates selection");
    };
    for (int height : {720, 1080}) {
        ImGui::GetIO().DisplaySize = ImVec2(height * 16.0f / 9.0f, (float)height); ui::Init(height / 1080.0f);
        for (auto lang : {i18n::kZh, i18n::kEn, i18n::kJa}) {
            i18n::Set(lang);
            c.foods[41].name = "Preview meal";
            if (height == 720 && lang == i18n::kZh) {
                c.foods[41].name.clear();
                for (int n = 0; n < 50; ++n) c.foods[41].name += "长";
            }
            ui::OnOpen(m); ui::DebugGo(2, 0);
            for (int n = 0; n < 45; ++n) Frame();
            Press(ImGuiKey_GamepadFaceUp);
            CHECK(ui::DebugPage() == 3, "real Y opens loadout editor before meal preview");
            Press(ImGuiKey_GamepadDpadDown); Press(ImGuiKey_GamepadDpadDown); Press(ImGuiKey_GamepadDpadDown);
            m.reqSave = m.reqApplyEquip = false;
            checkPreview(false, 41);
            const auto rowBounds = ui::DebugFoodPreviewBounds();
            Press(ImGuiKey_GamepadDpadUp);
            const ImGuiID rowFocus = GImGui->NavId;
            // ImGui ignores invalid->same-last-valid position as a mouse move.
            // Cross a real screen point first so every language case exits pad hover suppression.
            ImGui::GetIO().AddMousePosEvent(10, 10); Frame();
            ImGui::GetIO().AddMousePosEvent((rowBounds.anchorLeft + rowBounds.anchorRight) * 0.5f,
                                          (rowBounds.anchorTop + rowBounds.anchorBottom) * 0.5f); Frame();
            CHECK(ui::DebugFoodPreviewBounds().visible && GImGui->NavId == rowFocus,
                  "meal row mouse hover preview preserves existing keyboard/pad focus visible=%d id=%d nav=%08X expected=%08X hoverDisabled=%d mouse=(%.1f,%.1f) anchor=(%.1f,%.1f)-(%.1f,%.1f)",
                  ui::DebugFoodPreviewBounds().visible, ui::DebugFoodPreviewBounds().foodId, GImGui->NavId, rowFocus,
                  GImGui->NavHighlightItemUnderNav, ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y,
                  rowBounds.anchorLeft, rowBounds.anchorTop, rowBounds.anchorRight, rowBounds.anchorBottom);
            ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX); Frame();
            Press(ImGuiKey_GamepadDpadDown);
            Press(ImGuiKey_GamepadFaceDown);
            checkPreview(true, 41);
            const auto candidate41 = ui::DebugFoodPreviewBounds();
            Press(ImGuiKey_GamepadDpadDown);
            checkPreview(true, 73);
            const ImGuiID candidateFocus = GImGui->NavId;
            ImGui::GetIO().AddMousePosEvent(10, 10); Frame();
            ImGui::GetIO().AddMousePosEvent((candidate41.anchorLeft + candidate41.anchorRight) * 0.5f,
                                          (candidate41.anchorTop + candidate41.anchorBottom) * 0.5f); Frame(); Frame();
            CHECK(ui::DebugFoodPreviewBounds().visible && ui::DebugFoodPreviewBounds().fromPopup &&
                  ui::DebugFoodPreviewBounds().foodId == 41 && GImGui->NavId == candidateFocus &&
                  c.sets[0].food == 41 && !m.reqSave,
                  "picker hover prioritizes inspected candidate without moving navigation or applying");
            ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX); Frame();
            Press(ImGuiKey_GamepadFaceRight);
            CHECK(c.sets[0].food == 41 && !m.reqSave, "picker preview cancellation does not apply hovered/focused candidate");
            c.sets[0].food = -1; Frame(); Frame();
            CHECK(ui::DebugFoodPreviewBounds().visible && ui::DebugFoodPreviewBounds().foodId == 73 &&
                  !m.reqSave && c.sets[0].food == -1,
                  "global meal row previews actual global preset without storing an override");
            c.sets[0].food = 999; Frame(); Frame();
            CHECK(!ui::DebugFoodPreviewBounds().visible && c.foods.count(999) == 0 && !m.reqSave,
                  "unknown meal reference creates no preview or invented preset");
            c.sets[0].food = 41; c.foods[41].deleted = true; Frame(); Frame();
            CHECK(ui::DebugFoodPreviewBounds().visible && ui::DebugFoodPreviewBounds().foodId == 41 &&
                  c.foods[41].deleted && !m.reqSave,
                  "referenced deleted meal preserves its real stored preview without resurrecting it");
            c.foods[41].deleted = false;
        }
    }
    ImGui::GetIO().DisplaySize = display; ui::Init(display.y / 1080.0f); i18n::Set(language);
    m = before; m.cfg = original; ui::OnOpen(m); for (int n = 0; n < 5; ++n) Frame();
}

static void TestQuestSearchBoundary()
{
    const ui::Model before = m;
    Config* original = m.cfg;
    Config c; c.questId = 0; m.cfg = &c;
    auto searchId = [&]() {
        for (auto* w : GImGui->Windows)
            if (w->Active && std::strstr(w->Name, "##content")) return w->GetID("##search");
        return ImGuiID(0);
    };
    for (const auto& keys : std::vector<std::pair<ImGuiKey, ImGuiKey>>{
             {ImGuiKey_UpArrow, ImGuiKey_DownArrow}, {ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown},
             {ImGuiKey_W, ImGuiKey_S}}) {
        ui::OnOpen(m); ui::DebugGo(1, 0);
        for (int n = 0; n < 45; ++n) Frame();
        m.reqClose = m.reqSave = false;
        const ImGuiID firstRow = GImGui->NavId;
        Press(keys.first);
        const ImGuiID search = searchId();
        CHECK(search != 0 && GImGui->NavId == search && ui::DebugPage() == 1,
              "top quest row %s reaches native search field first=%08X search=%08X nav=%08X active=%08X text=%d flags=%X backend=%X query=%s", ImGui::GetKeyName(keys.first),
              firstRow, search, GImGui->NavId, GImGui->ActiveId, ImGui::GetIO().WantTextInput,
              ImGui::GetIO().ConfigFlags, ImGui::GetIO().BackendFlags, ui::DebugSearch().c_str());
        Press(keys.second);
        CHECK(GImGui->NavId == firstRow && !ImGui::GetIO().WantTextInput,
              "search %s returns to first list row without selecting quest", ImGui::GetKeyName(keys.second));
        Press(keys.first);
        Press(keys.first == ImGuiKey_GamepadDpadUp ? ImGuiKey_GamepadFaceDown : ImGuiKey_Enter);
        CHECK(ImGui::GetIO().WantTextInput && GImGui->ActiveId == search,
              "search reached from list activates for actual typing");
        ImGui::GetIO().AddInputCharactersUTF8("66845"); Frame(); Frame();
        CHECK(ui::DebugSearch() == "66845" && c.questId == 0 && !m.reqSave,
              "actual search typing filters without applying a quest");
        Press(ImGuiKey_Enter); // Commit input; Escape may restore the pre-edit value.
        Press(keys.second);
        CHECK(ui::DebugPage() == 1 && GImGui->NavId != search && !m.reqClose && ui::DebugSearch() == "66845",
              "filtered search exits down into native list navigation");
        Press(keys.first);
        CHECK(GImGui->NavId == search, "first filtered list item Up returns to native search");
        ui::DebugSetSearch("zzzz-no-quest-xyz");
        for (int n = 0; n < 4; ++n) Frame();
        // Empty-result boundary has no selectable result: search remains the
        // reachable native focus, then Down cannot apply a phantom quest.
        CHECK(GImGui->NavId == search, "empty filtered list retains reachable search boundary");
        Press(keys.second);
        CHECK(ui::DebugPage() == 1 && c.questId == 0 && !m.reqSave && !m.reqClose,
              "empty-result Down is safe and does not select or close");
    }
    m = before; m.cfg = original; ui::OnOpen(m);
    for (int n = 0; n < 5; ++n) Frame();
}

// The feature is opt-in. Exercise the real page navigation, popup selection and
// numeric edit mode rather than testing only the serialized configuration.
static void TestPalicoControls()
{
    Config* const originalPtr = m.cfg;
    const Config original = *originalPtr;
    const auto originalNames = m.palicoNames;
    Config cfg;
    LoadoutSet set;
    set.id = 41;
    cfg.sets.push_back(set);
    cfg.activeSet = -1;
    m.cfg = &cfg;
    m.palicoNames = {{2, "t", 2}, {7, "Bravo", 7}}; // A real name must not be mistaken for escaped whitespace.
    auto openEdit = [&] {
        ui::OnOpen(m);
        ui::DebugGo(2, 0);
        for (int n = 0; n < 5; ++n) Frame();
        Press(ImGuiKey_GamepadFaceUp);
        for (int n = 0; n < 3; ++n) Frame();
        CHECK(ui::DebugPage() == 3, "Palico test enters the loadout editor");
    };
    auto down = [&](int count) { for (int n = 0; n < count; ++n) Press(ImGuiKey_GamepadDpadDown); };

    cfg.palicoEnabled = false;
    openEdit();
    down(5);
    Press(ImGuiKey_GamepadFaceDown);
    CHECK(ui::DebugPage() == 2 && cfg.activeSet == 41,
          "disabled Palico feature hides every Palico row and preserves confirm navigation");

    cfg.palicoEnabled = true;
    cfg.activeSet = -1;
    openEdit();
    down(7);
    Press(ImGuiKey_GamepadFaceDown);
    Press(ImGuiKey_GamepadDpadDown);
    Press(ImGuiKey_GamepadFaceDown);
    CHECK(cfg.sets[0].palicoLoadout == 2, "Palico popup selects the real sparse ID 2");
    openEdit();
    down(7);
    Press(ImGuiKey_GamepadFaceDown);
    Press(ImGuiKey_GamepadDpadDown);
    Press(ImGuiKey_GamepadFaceDown);
    CHECK(cfg.sets[0].palicoLoadout == 7, "Palico popup keeps sparse ID 7 rather than list index 2");
    cfg.sets[0].palicoLoadout = 24;
    openEdit();
    down(7);
    CHECK(cfg.sets[0].palicoLoadout == 24, "missing saved Palico ID is not silently remapped");
    Press(ImGuiKey_GamepadFaceDown);
    Press(ImGuiKey_GamepadDpadDown);
    Press(ImGuiKey_GamepadFaceDown);
    CHECK(cfg.sets[0].palicoLoadout == 2, "explicit replacement of a missing ID still selects Alpha");

    cfg.sets[0].palicoLevel = 0;
    openEdit();
    down(6);
    CHECK(ui::DebugHintText().find("调整等级") != std::string::npos, "Palico level row exposes adjustment hint");
    Press(ImGuiKey_GamepadFaceDown);
    Press(ImGuiKey_GamepadDpadDown);
    CHECK(cfg.sets[0].palicoLevel == 0, "level adjustment clamps zero at the lower bound");
    Press(ImGuiKey_GamepadDpadUp);
    Press(ImGuiKey_GamepadDpadRight);
    Press(ImGuiKey_GamepadDpadRight);
    Press(ImGuiKey_GamepadDpadLeft);
    Press(ImGuiKey_GamepadDpadDown);
    CHECK(cfg.sets[0].palicoLevel == 10, "Palico level up/down is +/-1 and left/right is +/-10");
    CHECK(ui::DebugHintText().find("结束调整") != std::string::npos, "numeric edit footer explains how to finish");
    Press(ImGuiKey_GamepadFaceRight);
    CHECK(ui::DebugPage() == 3 && cfg.sets[0].palicoLevel == 10,
          "B exits numeric editing without returning to the loadout list");
    down(1);
    CHECK(cfg.sets[0].palicoLevel == 10 && ui::DebugPage() == 3,
          "navigation resumes after B without applying another numeric delta");

    cfg.sets[0].palicoLevel = 50;
    openEdit();
    down(6);
    Press(ImGuiKey_GamepadFaceDown);
    Press(ImGuiKey_GamepadDpadRight);
    CHECK(cfg.sets[0].palicoLevel == 50, "Palico level adjustment clamps at 50");
    Press(ImGuiKey_Escape);
    CHECK(ui::DebugPage() == 3 && cfg.sets[0].palicoLevel == 50,
          "Escape exits numeric editing without leaking to page back");

    cfg.sets[0].palicoGadgetLevel = 15;
    openEdit();
    down(8);
    Press(ImGuiKey_GamepadFaceDown);
    Press(ImGuiKey_GamepadDpadRight);
    CHECK(cfg.sets[0].palicoGadgetLevel == 15, "gadget level adjustment clamps at 15");
    Press(ImGuiKey_GamepadDpadDown);
    CHECK(cfg.sets[0].palicoGadgetLevel == 14, "gadget level supports the same numeric step controls");
    Press(ImGuiKey_GamepadFaceRight);

    *originalPtr = original;
    m.cfg = originalPtr;
    m.palicoNames = originalNames;
    m.reqSave = false;
    ui::OnOpen(m);
    for (int n = 0; n < 3; ++n) Frame();
}

int main()
{
    TestSetSummary();
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 1920; td.Height = 1080; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* rt = nullptr;
    dev->CreateTexture2D(&td, nullptr, &rt);
    dev->CreateRenderTargetView(rt, nullptr, &rtv);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1920, 1080);
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    ImGui_ImplDX11_Init(dev, ctx);
    ui::Init(1.0f);

    Config cfg;
    std::string err;
    LoadConfig(L"config\\BetterINQ.ini", cfg, err);
    m.cfg = &cfg;
    m.ready = true;

    Frame();
    ui::OnOpen(m);
    for (int i = 0; i < 4; ++i) Frame();
    CHECK(ui::DebugPage() == 0 && !ui::DebugSidebarFocused(), "打开面板：在主页，焦点在内容区（%s）", PageName(ui::DebugPage()));
    TestNoWindowing();
    TestLetterNavigation();
    TestDirectReorder();
    TestDeleteModal();
    TestContextHints();
    TestQuestSearchBoundary();
    TestFoodPreviewNavigation();

    g_sideTrace.clear();
    Press(ImGuiKey_GamepadDpadLeft);
    CHECK(ui::DebugSidebarFocused() && ui::DebugPage() == 0, "按左：进侧栏，还停在「任务」这一格，没乱切页（%s）", PageName(ui::DebugPage()));
    {
        bool clean = true;
        for (int i : g_sideTrace) clean &= (i == -1 || i == 0);
        CHECK(clean, "按左的过程中焦点没在别的格上停过（之前会先闪到「猫饭」）");
    }

    Press(ImGuiKey_GamepadDpadDown);
    CHECK(ui::DebugPage() == 2 && ui::DebugSidebarFocused(), "按下：切到「套装」，焦点留在侧栏（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadDpadDown);
    CHECK(ui::DebugPage() == 4, "再按下：切到「猫饭」（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadDpadUp);
    CHECK(ui::DebugPage() == 2, "按上：回到「套装」（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadDpadRight);
    CHECK(!ui::DebugSidebarFocused() && ui::DebugPage() == 2, "按右：回到内容区（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadFaceUp);
    CHECK(ui::DebugPage() == 3, "按 Y：打开第一套的编辑页（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadFaceRight);
    CHECK(ui::DebugPage() == 2, "按 B：回到套装列表（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadFaceRight);
    CHECK(ui::DebugPage() == 0 && !m.reqClose, "再按 B：回主页（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadFaceRight);
    CHECK(m.reqClose, "主页按 B：请求关面板");
    m.reqClose = false;

    // 只重置任务：在「重开」下面那排圆按钮之后，按 A 只提「只重置」，不会顺手接任务
    ui::OnOpen(m);
    for (int i = 0; i < 4; ++i) Frame();
    Press(ImGuiKey_GamepadDpadDown);    // 重开 → 中间「换套装」
    Press(ImGuiKey_GamepadDpadDown);    // → 「只重置任务」
    Press(ImGuiKey_GamepadFaceDown);
    CHECK(m.reqResetOnly && m.reqClose && !m.reqRestart, "主页：往下两次、A → 只重置任务（不重开）");
    m.reqResetOnly = false;
    m.reqClose = false;

    // 侧栏里按 A：切页并把焦点送进内容区
    ui::OnOpen(m);
    for (int i = 0; i < 4; ++i) Frame();
    Press(ImGuiKey_GamepadDpadLeft);
    Press(ImGuiKey_GamepadDpadDown);
    Press(ImGuiKey_GamepadDpadDown);
    Press(ImGuiKey_GamepadDpadDown);
    CHECK(ui::DebugPage() == 6, "侧栏里一路往下到「设置」（%s）", PageName(ui::DebugPage()));
    Press(ImGuiKey_GamepadFaceDown);
    CHECK(ui::DebugPage() == 6 && !ui::DebugSidebarFocused(), "侧栏里按 A：焦点进内容区（%s）", PageName(ui::DebugPage()));

    // 第 4 条 bug 的回归：主页换任务。「米拉」套装自己带着任务（66845），重开时套装的任务说了算，
    // 所以主页换任务要改套装里的那个，主页显示的也要跟着变
    {
        const int globalBefore = cfg.questId;
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadDpadDown);    // 重开 → 下面一排圆按钮（落在中间那个）
        Press(ImGuiKey_GamepadDpadLeft);    // → 换任务
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugPage() == 1, "主页：下、左、A 进「换任务」（%s）", PageName(ui::DebugPage()));
        Press(ImGuiKey_GamepadDpadUp);      // 66845 上面一个是 66846
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugPage() == 0, "选中后回到主页（%s）", PageName(ui::DebugPage()));
        CHECK(cfg.FindSet("米拉") && cfg.FindSet("米拉")->quest == 66846, "改的是「米拉」套装里的任务（现在 %d）",
              cfg.FindSet("米拉") ? cfg.FindSet("米拉")->quest : -1);
        CHECK(cfg.questId == globalBefore, "全局任务没被误改（%d）", cfg.questId);
        CHECK(m.reqSave, "提了存盘请求");
        m.reqSave = false;
    }

    // 往左：内容区里左边还有控件就照常移动；没有了才进侧栏，而且落在当前页那一格
    ui::OnOpen(m);
    for (int i = 0; i < 4; ++i) Frame();
    Press(ImGuiKey_GamepadDpadDown);    // 重开 → 中间的「换套装」
    Press(ImGuiKey_GamepadDpadRight);   // → 「换猫饭」
    Press(ImGuiKey_GamepadDpadLeft);
    CHECK(!ui::DebugSidebarFocused(), "主页「换猫饭」往左：留在内容区（到「换套装」）");
    Press(ImGuiKey_GamepadDpadLeft);
    Press(ImGuiKey_GamepadDpadLeft);
    CHECK(ui::DebugSidebarFocused() && ui::DebugSidebarFocusIndex() == 0 && ui::DebugPage() == 0,
          "再往左两次：进侧栏「任务」（第 %d 格）", ui::DebugSidebarFocusIndex());
    ui::DebugGo(6, 0);
    for (int i = 0; i < 4; ++i) Frame();
    for (int i = 0; i < 6; ++i) Press(ImGuiKey_GamepadDpadDown);
    g_sideTrace.clear();
    Press(ImGuiKey_GamepadDpadLeft);
    {
        bool clean = true;
        for (int i : g_sideTrace) clean &= (i == -1 || i == 3);
        CHECK(clean && ui::DebugSidebarFocusIndex() == 3 && ui::DebugPage() == 6,
              "设置页中间往左：直接到侧栏「设置」（第 %d 格，%s）", ui::DebugSidebarFocusIndex(), PageName(ui::DebugPage()));
    }

    // 换页时面板高度是渐变的
    {
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(6, 0);
        for (int i = 0; i < 40; ++i) Frame();
        const float tall = PanelH();
        ui::DebugGo(0, 0);
        Frame();
        const float h1 = PanelH();
        for (int i = 0; i < 40; ++i) Frame();
        const float shortH = PanelH();
        CHECK(tall > shortH + 50 && h1 < tall && h1 > shortH + 20,
              "设置 → 主页：高度 %.0f → %.0f（第一帧 %.0f，中间有过渡）", tall, shortH, h1);
    }

    // 录快捷键
    {
        Config& c = cfg;
        c.hkPanel = "View+Menu | Insert";
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugStartCapture(m, 1, true);
        Hold(XINPUT_GAMEPAD_A);               // 按 A 开始录的那一下还按着：不算
        CHECK(m.capturing, "开始录手柄：主循环知道在录（%d）", (int)m.capturing);
        Hold(0);
        Hold(XINPUT_GAMEPAD_LEFT_SHOULDER);
        Hold(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER);
        Hold(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER | kPadLT);
        Hold(0);
        CHECK(c.hkPanel == "LB+RB+LT | Insert" && !m.capturing && m.reqSave,
              "按 LB、RB、LT 再松开：手柄那组换成 LB+RB+LT，键盘那组不动（%s）", c.hkPanel.c_str());
        m.reqSave = false;

        ui::DebugStartCapture(m, 1, true);
        Hold(0);
        Hold(XINPUT_GAMEPAD_Y);
        Hold(0);
        CHECK(m.capturing && c.hkPanel == "LB+RB+LT | Insert" && !ui::DebugCaptureWhy().empty(),
              "只按一个 Y：不收，说明原因、接着录（%s）", ui::DebugCaptureWhy().c_str());
        // 录手柄时按 B：是组合里的键，不能被当成「返回」
        ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceRight, true);
        Hold(XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_Y);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceRight, false);
        Hold(0);
        CHECK(c.hkPanel == "B+Y | Insert" && ui::DebugPage() == 6, "B+Y 也能录，按 B 没把页面退回去（%s，%s）",
              c.hkPanel.c_str(), PageName(ui::DebugPage()));

        ui::DebugStartCapture(m, 1, false);
        Hold(0);
        Hold(0, {'R'});
        Hold(0);
        CHECK(m.capturing && c.hkPanel == "B+Y | Insert", "键盘单按 R：不收（%s）", ui::DebugCaptureWhy().c_str());
        Hold(0, {VK_CONTROL});
        Hold(0, {VK_CONTROL, 'R'});
        Hold(0);
        CHECK(c.hkPanel == "B+Y | Ctrl+R" && !m.capturing, "Ctrl+R：键盘那组换掉（%s）", c.hkPanel.c_str());

        // Delete 是普通可录键；Esc 清除，且不能冒泡到面板返回。
        ui::DebugStartCapture(m, 1, false);
        Hold(0);
        Hold(0, {VK_DELETE});
        Hold(0);
        CHECK(c.hkPanel == "B+Y | Delete" && !m.capturing, "录键盘时单按 Delete：正常设成快捷键（%s）",
              c.hkPanel.c_str());
        ui::DebugStartCapture(m, 1, false);
        Hold(0);
        Press(ImGuiKey_Escape);
        CHECK(c.hkPanel == "B+Y" && !m.capturing && ui::DebugPage() == 6 && !m.reqClose,
              "录键时 Esc 只清当前键盘组，不退页、不清另一个组");
        ui::DebugStartCapture(m, 1, true);
        Hold(0);
        Hold(XINPUT_GAMEPAD_X);
        Hold(0);
        CHECK(c.hkPanel == "B+Y" && !m.capturing,
              "面板键只剩手柄组时单按 X：不能清空最后的恢复入口（「%s」）", c.hkPanel.c_str());
        c.hkPanel = "View+Menu | Insert";
        ui::DebugStartCapture(m, 1, true);
        Hold(0);
        Press(ImGuiKey_Delete);
        CHECK(m.capturing && c.hkPanel == "View+Menu | Insert", "录手柄时 Delete 不再意外清除");
        Press(ImGuiKey_Escape);
        CHECK(c.hkPanel == "Insert" && !m.capturing && ui::DebugPage() == 6, "录手柄时 Esc 仅清手柄组且不退页（%s）", c.hkPanel.c_str());
        ui::DebugStartCapture(m, 1, false);
        Hold(0);
        Hold(0, {VK_CONTROL});
        Hold(0, {VK_CONTROL, VK_DELETE});
        Hold(0);
        CHECK(c.hkPanel == "Ctrl+Delete", "Ctrl+Delete 照样能录（%s）", c.hkPanel.c_str());
        c.hkPanel = "View+Menu | Insert";

        c.hkRestart = "LT+RT+L3+R3 | Delete";
        ui::DebugStartCapture(m, 0, false);
        Hold(0);
        Press(ImGuiKey_Escape);
        CHECK(!m.capturing && c.hkRestart == "LT+RT+L3+R3" && ui::DebugPage() == 6, "Esc：清除键盘组，保留手柄组、不退页（%s）",
              PageName(ui::DebugPage()));
        c.hkRestart = "LT+RT+L3+R3 | Delete";
        ui::DebugFocusHotkey(false);
        Frame(); Frame();
        Press(ImGuiKey_Delete);
        CHECK(c.hkRestart == "LT+RT+L3+R3 | Delete", "焦点格上的 Delete 不再清除快捷键");
        Press(ImGuiKey_Escape);
        CHECK(c.hkRestart == "LT+RT+L3+R3" && ui::DebugPage() == 6 && !m.reqClose,
              "未录制、焦点在快捷键格：Esc 清除当前格且不返回/关面板");
        ui::DebugGo(6, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_Escape);
        CHECK(ui::DebugPage() == 0, "焦点不在快捷键格时 Esc 照常返回（%s）", PageName(ui::DebugPage()));
        m.reqSave = false;
    }

    // 套装 / 猫饭：A、空格、鼠标点击只切换套用；Y 无条件直接编辑，不改变当前选择。
    {
        const int setBefore = cfg.activeSet;
        const int foodBefore = cfg.food;
        Config selection;
        LoadoutSet first;
        first.id = 41; first.name = "selection"; first.quest = 1605;
        selection.sets.push_back(first);
        selection.foods[7] = FoodPreset();
        selection.foodOrder = {7};
        selection.questId = 1234;
        m.cfg = &selection;
        auto openList = [&](int page) {
            ui::OnOpen(m);
            ui::DebugGo(page, 0);
            for (int i = 0; i < 4; ++i) Frame();
        };
        auto clickFocusedRow = [&](ImGuiMouseButton button = ImGuiMouseButton_Left) {
            ImGuiWindow* window = GImGui->NavWindow;
            CHECK(window && GImGui->NavId != 0, "鼠标切换回归：列表行有导航焦点");
            if (!window || !GImGui->NavId) return;
            const ImVec2 p = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]).GetCenter();
            io.AddMousePosEvent(p.x, p.y);
            Frame();
            io.AddMouseButtonEvent(button, true);
            Frame();
            io.AddMouseButtonEvent(button, false);
            Frame(); Frame();
            io.AddMousePosEvent(-1000, -1000);
            Frame();
        };
        for (int page : {2, 4}) {
            const int id = page == 2 ? 41 : 7;
            for (ImGuiKey key : {ImGuiKey_GamepadFaceDown, ImGuiKey_Space}) {
                selection.activeSet = -1;
                selection.food = -1;
                openList(page);
                m.reqSave = false;
                m.reqApplyEquip = false;
                Press(key);
                const int selected = page == 2 ? selection.activeSet : selection.food;
                CHECK(ui::DebugPage() == page && selected == id && m.reqSave,
                      "%s列表 %s：直接套用并留在列表", PageName(page), key == ImGuiKey_Space ? "空格" : "A");
                if (page == 2) CHECK(m.reqApplyEquip, "套装列表套用请求立即换装");
                m.reqSave = false;
                m.reqApplyEquip = false;
                Press(key);
                const int cancelled = page == 2 ? selection.activeSet : selection.food;
                CHECK(ui::DebugPage() == page && cancelled == -1 && m.reqSave,
                      "%s列表重复 %s：直接取消，不进入编辑", PageName(page), key == ImGuiKey_Space ? "空格" : "A");
                if (page == 2) CHECK(m.reqApplyEquip, "套装列表取消套用请求更新立即换装");
                CHECK(selection.questId == 1234, "切换套用不改全局任务");
            }
            selection.activeSet = -1;
            selection.food = -1;
            openList(page);
            Press(ImGuiKey_GamepadL3);
            m.reqSave = false;
            m.reqApplyEquip = false;
            Press(ImGuiKey_GamepadFaceDown);
            CHECK(!ui::DebugMoveActive() && ui::DebugPage() == page && selection.activeSet == -1 &&
                  selection.food == -1 && !m.reqSave && !m.reqApplyEquip,
                  "%s移动模式按 A：只放下，不套用或编辑", PageName(page));
            clickFocusedRow();
            CHECK(ui::DebugPage() == page && (page == 2 ? selection.activeSet : selection.food) == id,
                  "%s列表鼠标点击：直接套用", PageName(page));
            clickFocusedRow();
            CHECK(ui::DebugPage() == page && (page == 2 ? selection.activeSet : selection.food) == -1,
                  "%s列表鼠标再次点击：直接取消", PageName(page));
            m.reqSave = false;
            m.reqApplyEquip = false;
            clickFocusedRow(ImGuiMouseButton_Right);
            CHECK(ui::DebugPage() == page + 1 && selection.activeSet == -1 && selection.food == -1 &&
                  !m.reqSave && !m.reqApplyEquip,
                  "%s列表鼠标右键：直接编辑，不改变套用", PageName(page));
            for (bool selected : {false, true}) {
                for (ImGuiKey key : {ImGuiKey_GamepadFaceUp, ImGuiKey_Y}) {
                    selection.activeSet = page == 2 && selected ? 41 : -1;
                    selection.food = selected ? 7 : -1;
                    openList(page);
                    const int active = selection.activeSet, food = selection.food;
                    m.reqSave = false;
                    m.reqApplyEquip = false;
                    Press(key);
                    CHECK(ui::DebugPage() == page + 1 && selection.activeSet == active && selection.food == food &&
                          !m.reqSave && !m.reqApplyEquip,
                          "%s列表%s按%s Y：直接编辑，不改变 activeSet/food 或产生套用保存请求",
                          PageName(page), selected ? "已选中行" : "未选中行", key == ImGuiKey_Y ? "键盘" : "手柄");
                }
            }
        }

        // 编辑页原有确认和取消入口仍有效；进入编辑只能使用 Y。
        selection.activeSet = -1;
        openList(2);
        Press(ImGuiKey_GamepadFaceUp);
        for (int i = 0; i < 5; ++i) Press(ImGuiKey_GamepadDpadDown);   // 默认关闭随从功能，保持原导航
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugPage() == 2 && selection.activeSet == 41, "套装编辑确认：套用并回列表");
        Press(ImGuiKey_GamepadFaceUp);
        for (int i = 0; i < 6; ++i) Press(ImGuiKey_GamepadDpadDown);   // 默认关闭随从功能，保持原导航
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugPage() == 2 && selection.activeSet == -1, "套装编辑取消套用：回列表并取消");
        selection.food = -1;
        openList(4);
        Press(ImGuiKey_GamepadFaceUp);
        for (int i = 0; i < 7; ++i) Press(ImGuiKey_GamepadDpadDown);   // 耐力那行没了
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugPage() == 4 && selection.food == 7, "猫饭编辑确认：选择猫饭并回列表");
        Press(ImGuiKey_GamepadFaceUp);
        for (int i = 0; i < 8; ++i) Press(ImGuiKey_GamepadDpadDown);
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugPage() == 4 && selection.food == -1, "猫饭编辑取消预设：回列表并取消");
        m.cfg = &cfg;
        cfg.activeSet = setBefore;
        cfg.food = foodBefore;
        ui::OnOpen(m);

        // 套装编辑页的下拉第一项也是「—」：选它 = 这一项不换
        LoadoutSet* mila = const_cast<LoadoutSet*>(cfg.FindSet("米拉"));
        const std::string equipBefore = mila ? mila->equip : std::string();
        ui::DebugGo(3, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadFaceDown);          // 打开装备套装下拉，焦点在当前项（测试里没有游戏套装，就是「—」）
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(mila && mila->equip.empty() && ui::DebugPage() == 3, "装备套装下拉选「—」：清空（原来「%s」）", equipBefore.c_str());
        if (mila) mila->equip = equipBefore;
        m.reqSave = false;
    }

    // 猫饭锁定必须贯穿「列表 → 编辑 → 确认」，不是只拦截列表里的 A / Y。
    {
        Config meals;
        LoadoutSet set;
        set.id = 1;
        set.food = 1;
        set.name = "locked meal test";
        meals.sets.push_back(set);
        meals.activeSet = 1;
        for (int id : {1, 2, 3}) meals.foods[id] = FoodPreset();
        meals.foodOrder = {1, 2, 3};
        m.cfg = &meals;
        for (int id : {1, 2}) { // 编辑套装自带的猫饭、以及别的猫饭，两种都不能覆盖全局
            meals.food = 3;
            ui::OnOpen(m);
            ui::DebugGo(4, 0);
            for (int i = 0; i < 4; ++i) Frame();
            if (id == 2) Press(ImGuiKey_GamepadDpadDown);
            m.reqSave = false;
            m.reqApplyEquip = false;
            for (ImGuiKey key : {ImGuiKey_GamepadFaceDown, ImGuiKey_Space}) {
                Press(key);
                CHECK(ui::DebugPage() == 4 && meals.food == 3 && meals.activeSet == 1 &&
                      meals.sets[0].food == 1 && !m.reqSave && !m.reqApplyEquip,
                      "锁定列表：%s 对猫饭 %d 无操作，不改页面、选择或保存请求", key == ImGuiKey_Space ? "空格" : "A", id);
            }
            Press(id == 1 ? ImGuiKey_GamepadFaceUp : ImGuiKey_Y);
            CHECK(ui::DebugPage() == 5 && meals.food == 3 && meals.activeSet == 1 &&
                  !m.reqSave && !m.reqApplyEquip,
                  "锁定列表：Y 仍进入猫饭 %d 编辑，不改 activeSet/food", id);
            m.reqSave = false;
            Press(ImGuiKey_GamepadDpadRight); // 默认体力 +0 → +10，编辑仍能保存
            Press(ImGuiKey_GamepadFaceDown);
            CHECK(meals.foods[id].hp == 1 && m.reqSave, "锁定猫饭 %d：修改内容仍请求保存", id);
            m.reqSave = false;
            for (int i = 0; i < 7; ++i) Press(ImGuiKey_GamepadDpadDown);
            Press(ImGuiKey_GamepadFaceDown);
            CHECK(ui::DebugPage() == 4 && meals.food == 3 && meals.sets[0].food == 1 && !m.reqSave,
                  "锁定猫饭 %d 编辑确认：回列表、保留全局和套装选择、不产生选择保存", id);
        }
        for (bool activeSet : {false, true}) {
            meals.activeSet = activeSet ? 1 : 0;
            meals.sets[0].food = -1;   // -1 = 这套不带猫饭（0 是合法编号了）
            meals.food = 3;
            ui::DebugGo(5, 2);
            for (int i = 0; i < 4; ++i) Frame();
            m.reqSave = false;
            for (int i = 0; i < 7; ++i) Press(ImGuiKey_GamepadDpadDown);
            Press(ImGuiKey_GamepadFaceDown);
            CHECK(ui::DebugPage() == 4 && meals.food == 2 && m.reqSave,
                  "未锁定（%s）：编辑确认仍选择猫饭并请求保存", activeSet ? "套装没带猫饭" : "没有套装");
        }
        m.cfg = &cfg;
        m.reqSave = false;
        ui::OnOpen(m);
    }

    // 下拉选项里按 B：只关下拉，不退页
    {
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(3, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadFaceDown);          // 焦点默认在「装备套装」，A 打开下拉
        CHECK(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), "套装编辑页按 A：打开装备套装的下拉");
        Press(ImGuiKey_GamepadFaceRight);
        CHECK(!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && ui::DebugPage() == 3,
              "下拉里按 B：关掉下拉，还在套装编辑页（%s）", PageName(ui::DebugPage()));
        Press(ImGuiKey_GamepadFaceRight);
        CHECK(ui::DebugPage() == 2, "再按 B：才回套装列表（%s）", PageName(ui::DebugPage()));
    }

    // 搜任务：拼音、首字母、混着打、多音字、ID 开头
    {
        const QuestInfo* q = gamedata::FindQuest(66845);   // 传说中的黑龙
        const QuestInfo* w = gamedata::FindQuest(66846);   // 最后的白骑士
        CHECK(q && w, "找到两个任务");
        if (q && w) {
            const char* yes[] = {"heilong", "hl", "HeiLong", "hlong", "heil", "chuanshuo", "cszdhl", "zhongdehei", "zhuanshuo",
                                 "chuan shuo", "668", "66845", "黑龙"};
            for (const char* n : yes) CHECK(gamedata::QuestMatches(*q, n), "「传说中的黑龙」能用「%s」搜到", n);
            const char* no[] = {"bailong", "hlz", "66846", "白", "xyz"};
            for (const char* n : no) CHECK(!gamedata::QuestMatches(*q, n), "「传说中的黑龙」用「%s」搜不到", n);
            CHECK(gamedata::QuestMatches(*w, "bqs") && gamedata::QuestMatches(*w, "zuihou"), "「最后的白骑士」：bqs、zuihou");
        }
    }

    // 排序：按 L3 进移动模式，上下键换位置，再按 L3 放下
    {
        LoadoutSet two;
        two.id = cfg.NextSetId();
        two.name = "第二套";
        cfg.sets.push_back(two);
        cfg.activeSet = cfg.sets[0].id;   // 焦点默认落在「已经在用」的那一套上，Y 进入编辑
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(2, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadL3);                // 焦点在第一行：把它放进移动模式
        Press(ImGuiKey_GamepadDpadDown);          // 往下挪一格
        CHECK(cfg.sets.size() == 2 && cfg.sets[0].name == "第二套" && cfg.sets[1].name == "米拉",
              "L3 + 下：套装换到第 2 位（%s / %s）", cfg.sets[0].name.c_str(), cfg.sets[1].name.c_str());
        Press(ImGuiKey_GamepadDpadUp);
        CHECK(cfg.sets[0].name == "米拉", "L3 + 上：换回第 1 位");
        Press(ImGuiKey_GamepadL3);                // 放下
        Press(ImGuiKey_GamepadFaceUp);
        CHECK(ui::DebugPage() == 3, "放下后按 Y：进的是刚移动的那套（%s）", PageName(ui::DebugPage()));
    }

    // 移动模式的三条行为：长按要连续换位、B 是放下不是返回、换页后状态清空
    {
        cfg.sets.clear();
        for (int i = 0; i < 3; ++i) {
            LoadoutSet s;
            s.id = cfg.NextSetId();
            s.name = std::string("套") + std::to_string(i + 1);
            cfg.sets.push_back(s);
        }
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(2, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadL3);
        CHECK(ui::DebugMoveActive() && ui::DebugMoveId() == cfg.sets[0].id, "L3：进入移动模式");

        // 按住不放：ImGui 的按键重复要过 KeyRepeatDelay 才开始，所以按住 40 帧
        const std::string first = cfg.sets[0].name;
        ImGui::GetIO().AddKeyEvent(ImGuiKey_DownArrow, true);
        for (int i = 0; i < 40; ++i) Frame();
        ImGui::GetIO().AddKeyEvent(ImGuiKey_DownArrow, false);
        Frame();
        CHECK(cfg.sets.size() == 3 && cfg.sets[2].name == first,
              "按住下键不放：连续换位到最后一格（%s 在 %s）", first.c_str(), cfg.sets[2].name.c_str());

        const int pageBefore = ui::DebugPage();
        Press(ImGuiKey_GamepadFaceRight);   // B
        CHECK(!ui::DebugMoveActive(), "移动中按 B：只是放下，不留在移动模式");
        CHECK(ui::DebugPage() == pageBefore, "移动中按 B：不返回上一页（还在%s）", PageName(ui::DebugPage()));

        // 离开再回来：状态必须已经清掉，而且 L3 还能重新进
        ui::DebugGo(0, 0);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(2, 0);
        for (int i = 0; i < 4; ++i) Frame();
        CHECK(!ui::DebugMoveActive() && ui::DebugMoveId() == 0, "换了页再回来：移动模式已经清掉");
        Press(ImGuiKey_Escape);
        CHECK(ui::DebugPage() == 0, "移动模式清掉后，Esc 又能正常返回（%s）", PageName(ui::DebugPage()));
    }


    // 快捷键关闭面板不会走 Go：移动模式下直接重开也必须恢复导航 / 返回。
    {
        const int navFlags = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        for (int page : {2, 4}) {
            ui::OnOpen(m);
            ui::DebugGo(page, 0);
            for (int i = 0; i < 4; ++i) Frame();
            Press(ImGuiKey_GamepadL3);
            Frame();
            CHECK(ui::DebugMoveActive() && !(io.ConfigFlags & navFlags),
                  "%s移动中：导航暂时关闭", PageName(page));
            // 模拟主循环用面板快捷键关闭再打开：不按 B、不调用 DebugGo 来清状态。
            m.reqClose = false;
            ui::OnOpen(m);
            for (int i = 0; i < 4; ++i) Frame();
            CHECK(ui::DebugPage() == 0 && !ui::DebugMoveActive() && ui::DebugMoveId() == 0 &&
                  (io.ConfigFlags & navFlags) == navFlags, "%s移动中重开：回主页、清移动状态、恢复导航", PageName(page));
            Press(ImGuiKey_GamepadDpadLeft);
            CHECK(ui::DebugSidebarFocused(), "%s移动中重开：手柄仍能进入侧栏", PageName(page));
            Press(ImGuiKey_Escape);
            CHECK(m.reqClose, "%s移动中重开：Esc 仍能请求关闭", PageName(page));
            m.reqClose = false;
        }
    }

    // 猫饭排序：只改面板里的显示顺序，编号不动
    {
        cfg.foods[5] = FoodPreset();
        cfg.foodOrder = {1, 5};
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(4, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadL3);
        Press(ImGuiKey_GamepadDpadDown);
        const std::vector<int> ids = cfg.FoodIds();
        CHECK(ids.size() == 2 && ids[0] == 5 && ids[1] == 1,
              "L3 + 下：猫饭显示顺序换过来（%d / %d，编号没变）", ids.empty() ? 0 : ids[0], ids.size() > 1 ? ids[1] : 0);
        Press(ImGuiKey_GamepadL3);
    }

    // 名字长度按「字」算，不按字节：上限 50，且不能切成半个字符
    {
        std::string wide;
        for (int i = 0; i < 60; ++i) wide += "字";          // 60 个字，每个 3 字节
        const std::string got = ui::DebugClampName(wide.c_str());
        CHECK(got.size() == 150, "60 个汉字只留前 50 个（%d 字节）", (int)got.size());
        CHECK(got.size() % 3 == 0, "截断没切坏多字节字符");
        CHECK(ui::DebugClampName("abc") == "abc", "短名字原样保留");
        std::string ascii(60, 'a');
        CHECK(ui::DebugClampName(ascii.c_str()).size() == 50, "纯英文最多 50 个字符");
        CHECK(ui::DebugClampName("abc字").size() == 6, "中英混写也在同一个上限内");
        const std::string continuation(300, (char)0x80);
        CHECK(ui::DebugClampName(continuation.c_str()).empty(), "300 个孤立 continuation 字节被拒绝，不溢出名字缓冲区");
        const char* invalid[] = {"ok\xC0\xAF", "ok\xE0\x80\xAF", "ok\xED\xA0\x80",
                                 "ok\xF0\x80\x80\xAF", "ok\xF4\x90\x80\x80", "ok\xF5\x80\x80\x80",
                                 "ok\xE5\xAD", "ok\xF0\x9F\x98", "ok\xE5X"};
        for (const char* text : invalid)
            CHECK(ui::DebugClampName(text) == "ok", "畸形 UTF-8 只保留合法前缀（过长编码 / 代理项 / 越界 / 缺字节）");
    }

    // 真正的改名弹窗：必须走 InputText 和确认写回，不能只验证 DebugClampName。
    {
        const std::string setNameBefore = cfg.sets[0].name;
        const int foodId = cfg.foods.begin()->first;
        const std::string foodNameBefore = cfg.foods[foodId].name;
        std::string fiftyCjk, fiftyFourByte;
        for (int i = 0; i < 50; ++i) {
            fiftyCjk += "字";
            fiftyFourByte += "\xF0\x9F\x98\x80"; // U+1F600，每个字符 4 字节
        }
        auto openRename = [&](int page) {
            ui::OnOpen(m);
            ui::DebugGo(page, 0);
            for (int i = 0; i < 4; ++i) Frame();
            ui::DebugOpenRename();
            for (int i = 0; i < 4; ++i) Frame();
            CHECK(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && io.WantTextInput,
                  "%s改名：真实弹窗和输入框已激活", PageName(page));
        };
        auto submitRename = [&]() {
            Press(ImGuiKey_Enter);
            CHECK(!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && m.reqSave,
                  "改名确认：弹窗关闭并请求保存");
            m.reqSave = false;
        };
        for (int page : {2, 4}) {
            std::string& name = page == 2 ? cfg.sets[0].name : cfg.foods[foodId].name;
            name = "old";
            m.reqSave = false;
            openRename(page);
            io.AddKeyEvent(ImGuiMod_Ctrl, true);
            Press(ImGuiKey_A); // 选中旧名后从 ImGui 输入事件替换，不直接写测试缓冲区
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
            Frame();
            io.AddInputCharactersUTF8((fiftyCjk + "字字字字字字字字字字").c_str());
            for (int i = 0; i < 4; ++i) Frame();
            // 超长输入后继续编辑，确认截断也同步到了 ImGui 内部状态。
            Press(ImGuiKey_Backspace);
            io.AddInputCharactersUTF8("文");
            for (int i = 0; i < 4; ++i) Frame();
            submitRename();
            CHECK(name == fiftyCjk.substr(0, 49 * 3) + "文",
                  "%s弹窗输入超过 50 个汉字：按字符截断且后续编辑正常（%d 字节）", PageName(page), (int)name.size());

            name = fiftyCjk;
            openRename(page);
            submitRename();
            CHECK(name == fiftyCjk, "%s重开并确认已有 50 汉字名字：150 字节无损保留", PageName(page));

            // 默认 ImGui ImWchar 为 16 位，输入事件不接受非 BMP 字符；已有 UTF-8 名字仍应无损往返。
            name = fiftyFourByte;
            openRename(page);
            submitRename();
            CHECK(name == fiftyFourByte, "%s已有 50 个四字节字符：200 字节无损保留", PageName(page));
            name = fiftyFourByte + "\xF0\x9F\x98\x80";
            openRename(page);
            submitRename();
            CHECK(name == fiftyFourByte, "%s已有超长四字节名字：截到 50 字且不切半个 UTF-8 字符", PageName(page));
            name = std::string("ok") + std::string(300, (char)0x80);
            openRename(page);
            submitRename();
            CHECK(name == "ok", "%s已有畸形 UTF-8 名字：真实弹窗仅保留合法前缀且无缓冲区溢出", PageName(page));
        }
        cfg.sets[0].name = setNameBefore;
        cfg.foods[foodId].name = foodNameBefore;
    }

    // 移动模式滚列表：往下挪出可视区要滚，往上挪回来要滚回去
    {
        cfg.sets.clear();
        for (int i = 0; i < 12; ++i) {
            LoadoutSet s;
            s.id = cfg.NextSetId();
            s.name = std::string("滚") + std::to_string(i + 1);
            cfg.sets.push_back(s);
        }
        cfg.activeSet = 0;
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(2, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadL3);              // 进移动模式（焦点在第 1 行）
        Frame();
        const float base = ui::DebugListScroll();   // 内容区可能带着上一页的滚动位置，用相对量判断
        for (int i = 0; i < 10; ++i) {          // 一路往下挪到出界
            Press(ImGuiKey_GamepadDpadDown);
            Frame();
        }
        Frame();                                // SetScrollY 设的是目标值，下一帧才落到 Scroll.y
        const float scrolledDown = ui::DebugListScroll();
        CHECK(scrolledDown > base, "往下挪出可视区：列表跟着滚（%.1f → %.1f）", base, scrolledDown);
        for (int i = 0; i < 10; ++i) {          // 再一路挪回顶上
            Press(ImGuiKey_GamepadDpadUp);
            Frame();
        }
        Frame();
        const float scrolledBack = ui::DebugListScroll();
        CHECK(scrolledBack < scrolledDown, "往上挪回来：列表往回滚（%.1f → %.1f）", scrolledDown, scrolledBack);
        CHECK(scrolledBack == base, "挪回第一行：滚动位置回到进移动模式时（%.1f → %.1f）", base, scrolledBack);
        Press(ImGuiKey_GamepadFaceRight);       // B 放下，别把状态带给后面的用例
    }

    // 猫饭页本身：单列列表按左右不能把焦点判空，否则之后上下只在当前可见的几行里打转。
    {
        cfg.sets.clear();
        LoadoutSet one;
        one.id = cfg.NextSetId();
        one.name = "单";
        cfg.sets.push_back(one);
        cfg.activeSet = one.id;
        cfg.foods.clear();
        cfg.foodOrder.clear();
        for (int i = 0; i < 18; ++i) {
            const int n = cfg.AllocFoodId();
            cfg.foods[n] = FoodPreset();
            cfg.foodOrder.push_back(n);
        }
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(4, 0);
        for (int i = 0; i < 6; ++i) Frame();
        for (int i = 0; i < 6; ++i) { Press(ImGuiKey_GamepadDpadDown); Frame(); }
        const int before = ui::DebugListFocusId();
        Press(ImGuiKey_GamepadDpadRight);      // 列表里往右没有去处
        Frame();
        Press(ImGuiKey_GamepadDpadDown);
        Frame();
        CHECK(ui::DebugListFocusId() != before && ui::DebugListFocusId() != 0,
              "猫饭页按右以后，上下仍然能在列表里继续走（%d → %d）", before, ui::DebugListFocusId());
        const int afterRight = ui::DebugListFocusId();
        Press(ImGuiKey_GamepadDpadLeft);       // 进侧栏再回来
        Frame();
        Press(ImGuiKey_GamepadDpadRight);
        Frame();
        Press(ImGuiKey_GamepadDpadDown);
        Frame();
        CHECK(ui::DebugListFocusId() != afterRight && ui::DebugListFocusId() != 0,
              "猫饭页从侧栏回来后，上下仍然在列表里继续走（%d → %d）", afterRight, ui::DebugListFocusId());
    }

    // 换任务：点进去就得有任务能挑。分组全折叠时整页只有 7 个分组标题，手柄按下走几个就到底了，
    // 看着就像「列表里没任务、上下都卡住」。没有选中任务时自动展开第一组。
    {
        cfg.sets.clear();
        cfg.activeSet = 0;
        cfg.questId = 0;
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(1, 0);
        for (int i = 0; i < 4; ++i) Frame();
        for (int i = 0; i < 3; ++i) Press(ImGuiKey_GamepadDpadDown);
        Press(ImGuiKey_GamepadFaceDown);
        Frame();
        CHECK(ui::DebugPage() == 0 && gamedata::FindQuest(cfg.questId) != nullptr,
              "换任务：进去就展开了第一组，按下三下能选到任务（选中 %d，%s）", cfg.questId,
              PageName(ui::DebugPage()));
    }

    // 套装里的任务：0 = 不指定。列表里得有这一行，不然给套装挑过一次任务就再也退不回「不指定」了。
    {
        cfg.sets.clear();
        LoadoutSet one;
        one.id = cfg.NextSetId();
        one.name = "清";
        one.quest = 0;
        cfg.sets.push_back(one);
        cfg.activeSet = one.id;
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(3, 0);
        for (int i = 0; i < 4; ++i) Frame();
        for (int i = 0; i < 4; ++i) Press(ImGuiKey_GamepadDpadDown);   // 到「任务」这一行
        Press(ImGuiKey_GamepadFaceDown);
        for (int i = 0; i < 3; ++i) Frame();
        CHECK(ui::DebugPage() == 1, "套装编辑页的任务行：进任务列表（%s）", PageName(ui::DebugPage()));
        Press(ImGuiKey_GamepadFaceDown);                               // 焦点默认就在「不指定」上
        Frame();
        CHECK(ui::DebugPage() == 3 && cfg.sets[0].quest == 0,
              "套装的任务列表里有「不指定」，按 A 选得回去（quest=%d，%s）", cfg.sets[0].quest,
              PageName(ui::DebugPage()));
    }

    // 从套装进任务页会滚到选中项上。焦点一旦走到视口边缘，下一行整个被裁掉就不是导航候选了，
    // 卡在「往上 3 个、往下 8 个」，分组标题也永远滚不出来 —— 列表得像条真列表那样跟着滚。
    {
        int n = 0;
        const QuestInfo* qs = gamedata::Quests(n);
        int g3[200], g3n = 0;
        for (int i = 0; i < n && g3n < 200; ++i)
            if (qs[i].group == 3) g3[g3n++] = qs[i].id;
        const int mid = 59;                       // 组里第 60 个任务，两边都还剩几十个
        cfg.sets.clear();
        LoadoutSet deep;
        deep.id = cfg.NextSetId();
        deep.name = "远";
        deep.quest = g3[mid];
        cfg.sets.push_back(deep);
        cfg.activeSet = deep.id;
        cfg.questId = 0;

        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(3, 0);
        for (int i = 0; i < 4; ++i) Frame();
        for (int i = 0; i < 4; ++i) Press(ImGuiKey_GamepadDpadDown);   // 到「任务」这一行
        Press(ImGuiKey_GamepadFaceDown);
        for (int i = 0; i < 3; ++i) Frame();
        CHECK(ui::DebugPage() == 1, "远任务：从套装编辑进任务列表（%s）", PageName(ui::DebugPage()));
        for (int i = 0; i < 10; ++i) Press(ImGuiKey_GamepadDpadUp);
        Press(ImGuiKey_GamepadFaceDown);
        Frame();
        CHECK(ui::DebugPage() == 3 && cfg.sets[0].quest == g3[mid - 10],
              "往上 10 行走得动（选中 %d，应为组里第 %d 个 %d，%s）", cfg.sets[0].quest, mid - 10 + 1,
              g3[mid - 10], PageName(ui::DebugPage()));

        cfg.sets[0].quest = g3[mid];
        ui::DebugGo(3, 0);
        for (int i = 0; i < 4; ++i) Frame();
        for (int i = 0; i < 4; ++i) Press(ImGuiKey_GamepadDpadDown);
        Press(ImGuiKey_GamepadFaceDown);
        for (int i = 0; i < 3; ++i) Frame();
        for (int i = 0; i < 12; ++i) Press(ImGuiKey_GamepadDpadDown);
        Press(ImGuiKey_GamepadFaceDown);
        Frame();
        CHECK(ui::DebugPage() == 3 && cfg.sets[0].quest == g3[mid + 12],
              "往下 12 行走得动（选中 %d，应为组里第 %d 个 %d，%s）", cfg.sets[0].quest, mid + 12 + 1,
              g3[mid + 12], PageName(ui::DebugPage()));
    }

    // 滚轮必须能离开仍持有 NavId 的选中行；进页深定位第一帧就到位且后续静止帧不追赶。
    {
        cfg.sets.clear();
        cfg.activeSet = -1;
        int count = 0;
        const QuestInfo* quests = gamedata::Quests(count);
        std::vector<int> deep;
        for (int i = 0; i < count; ++i) if (quests[i].group == 3) deep.push_back(quests[i].id);
        cfg.questId = deep[59];
        ui::OnOpen(m);
        ui::DebugGo(1, 0);
        Frame();
        ImGuiWindow* list = nullptr;
        for (ImGuiWindow* w : GImGui->Windows)
            if (w->Active && std::strstr(w->Name, "/##qlist_")) list = w;
        CHECK(list != nullptr, "任务滚动回归：实际列表窗口存在");
        if (list) {
            const float first = list->Scroll.y;
            CHECK(first > 1000 && list->ScrollMax.y > first, "深处任务首次显示即定位（%.0f / %.0f）", first, list->ScrollMax.y);
            for (int i = 0; i < 5; ++i) {
                Frame();
                CHECK(std::fabs(list->Scroll.y - first) < 1.0f, "进页静止帧不分段追赶或反复跟随（%.0f / %.0f）", list->Scroll.y, first);
            }
            const ImVec2 mouse = list->InnerClipRect.GetCenter();
            io.AddMousePosEvent(mouse.x, mouse.y);
            Frame();
            io.AddMouseWheelEvent(0, -3);
            Frame();
            const float down = list->Scroll.y;
            CHECK(down > first, "鼠标滚轮向下首帧移动列表（%.0f > %.0f）", down, first);
            for (int i = 0; i < 5; ++i) Frame();
            CHECK(std::fabs(list->Scroll.y - down) < 1.0f, "滚轮结束旧焦点不会拉回列表（%.0f / %.0f）", list->Scroll.y, down);
            io.AddMouseWheelEvent(0, 2);
            Frame();
            const float up = list->Scroll.y;
            CHECK(up < down, "鼠标滚轮向上首帧移动列表（%.0f < %.0f）", up, down);
            for (int i = 0; i < 5; ++i) Frame();
            CHECK(std::fabs(list->Scroll.y - up) < 1.0f, "上滚结束没有焦点跟随振荡（%.0f / %.0f）", list->Scroll.y, up);
            io.AddMousePosEvent(-1000, -1000);
            Frame();
        }
    }

    // 套装/猫饭也复用 Row：滚轮离开首行后不能被旧焦点一帧帧拉回。
    {
        Config scrollCfg;
        for (int i = 1; i <= 45; ++i) {
            LoadoutSet s;
            s.id = i; s.name = "scroll " + std::to_string(i);
            scrollCfg.sets.push_back(s);
            scrollCfg.foods[i] = FoodPreset();
            scrollCfg.foodOrder.push_back(i);
        }
        scrollCfg.activeSet = 1;
        scrollCfg.food = 1;
        m.cfg = &scrollCfg;
        for (int page : {2, 4}) {
            ui::OnOpen(m);
            ui::DebugGo(page, 0);
            for (int i = 0; i < 5; ++i) Frame();
            ImGuiWindow* list = GImGui->NavWindow;
            const float before = list->Scroll.y;
            const ImVec2 mouse = list->InnerClipRect.GetCenter();
            io.AddMousePosEvent(mouse.x, mouse.y);
            Frame();
            if (page == 4) {
                bool tooltip = false;
                for (int n = 0; n < 60; ++n) {
                    Frame();
                    for (ImGuiWindow* w : GImGui->Windows)
                        tooltip |= w->Active && (w->Flags & ImGuiWindowFlags_Tooltip) != 0;
                }
                CHECK(GImGui->HoveredWindow == list && !tooltip, "猫饭列表持续悬停不弹出重复详情窗口");
            }
            io.AddMouseWheelEvent(0, -5);
            Frame();
            const float after = list->Scroll.y;
            CHECK(after > before, "%s长列表滚轮首帧移动（%.0f > %.0f）", PageName(page), after, before);
            for (int i = 0; i < 5; ++i) Frame();
            CHECK(std::fabs(list->Scroll.y - after) < 1.0f, "%s长列表滚轮结束不回弹（%.0f / %.0f）", PageName(page), list->Scroll.y, after);
            io.AddMousePosEvent(-1000, -1000);
            Frame();
        }
        m.cfg = &cfg;
        ui::OnOpen(m);
    }

    // 逐帧检查十字键：新 NavId 生效的绘制帧必须已完成滚动，不能再排下帧补滚。
    // Press() 会额外跑两帧，旧测试只看最终位置会漏掉这种视觉上的两段跳动。
    {
        Config navCfg;
        for (int i = 1; i <= 45; ++i) {
            LoadoutSet s; s.id = i; s.name = "nav " + std::to_string(i);
            navCfg.sets.push_back(s);
            navCfg.foods[i] = FoodPreset();
            navCfg.foodOrder.push_back(i);
        }
        navCfg.activeSet = 1; navCfg.food = 1;
        int count = 0;
        const QuestInfo* quests = gamedata::Quests(count);
        std::vector<int> deep;
        for (int i = 0; i < count; ++i) if (quests[i].group == 3) deep.push_back(quests[i].id);
        navCfg.questId = deep[59];
        m.cfg = &navCfg;
        for (int page : {1, 2, 4}) {
            ui::OnOpen(m);
            ui::DebugGo(page, 0);
            for (int i = 0; i < 6; ++i) Frame();
            bool noDeferredScroll = true, stableAfterMove = true, focusVisible = true;
            int scrollMoves = 0;
            bool everyStepMoves = true;
            for (ImGuiKey key : {ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadUp}) {
                for (int step = 0; step < 24; ++step) {
                    const ImGuiID previousId = GImGui->NavId;
                    io.AddKeyEvent(key, true);
                    Frame(); // 收集导航打分结果
                    ImGuiWindow* list = GImGui->NavWindow;
                    const float before = list->Scroll.y;
                    io.AddKeyEvent(key, false);
                    Frame(); // NewFrame 应用新 NavId，Begin 应用滚动，然后提交绘制
                    list = GImGui->NavWindow;
                    const float applied = list->Scroll.y;
                    // 任务从组中间开始，双向都未到端点；每次输入必须走到另一行。
                    if (page == 1) everyStepMoves &= GImGui->NavId != 0 && GImGui->NavId != previousId;
                    if (std::fabs(applied - before) > 0.5f) ++scrollMoves;
                    noDeferredScroll &= list->ScrollTarget.y == FLT_MAX;
                    const ImRect nav = ImGui::WindowRectRelToAbs(list, list->NavRectRel[ImGuiNavLayer_Main]);
                    focusVisible &= nav.Min.y >= list->InnerClipRect.Min.y - 1.0f && nav.Max.y <= list->InnerClipRect.Max.y + 1.0f;
                    Frame();
                    stableAfterMove &= std::fabs(list->Scroll.y - applied) < 0.5f;
                }
            }
            CHECK(scrollMoves > 0, "%s十字键双向遍历触发实际滚动（%d 次）", PageName(page), scrollMoves);
            if (page == 1) CHECK(everyStepMoves, "任务列表越过可视边缘仍逐行导航，不被手柄裁剪夹回旧行");
            CHECK(noDeferredScroll, "%s新焦点绘制帧不排下帧补滚", PageName(page));
            CHECK(stableAfterMove, "%s十字键每步只滚一次，不在静止帧二段跳动", PageName(page));
            CHECK(focusVisible, "%s双向滚动后导航行始终完整可见", PageName(page));
        }
        m.cfg = &cfg;
        ui::OnOpen(m);
    }

    // 装备/道具套装存的是编号：游戏里的名字可能重名、也可能为空（面板显示「装备套装 N」），
    // 按名字存的话重排过一次就套到别人身上去了。
    {
        cfg.sets.clear();
        LoadoutSet s;
        s.id = cfg.NextSetId();
        s.name = "id";
        cfg.sets.push_back(s);
        cfg.activeSet = s.id;
        m.equipNames = {{2, "装备套装 2", 7}, {3, "同名", 9}, {4, "同名", 12}};
        m.itemNames = {{1, "道具套装 1", 3}, {2, "同名", 8}, {3, "同名", 11}};
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(3, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadFaceDown);                       // 打开装备套装下拉
        for (int i = 0; i < 2; ++i) Press(ImGuiKey_GamepadDpadDown);   // 「—」→ 第 2 个（同名 9）
        Press(ImGuiKey_GamepadFaceDown);
        Frame();
        CHECK(cfg.sets[0].equip == "9", "装备套装存编号而不是名字（%s）", cfg.sets[0].equip.c_str());
        ui::DebugGo(3, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadDpadDown);                        // 到道具那一行
        Press(ImGuiKey_GamepadFaceDown);                       // 打开道具下拉
        for (int i = 0; i < 3; ++i) Press(ImGuiKey_GamepadDpadDown);   // 「—」→ 第 3 个（同名 11）
        Press(ImGuiKey_GamepadFaceDown);
        Frame();
        CHECK(cfg.sets[0].item == "#11", "道具套装存存放格（%s）", cfg.sets[0].item.c_str());
        m.equipNames.clear();
        m.itemNames.clear();
        m.reqSave = false;
    }

    // 删猫饭：只从列表里拿掉，内容和编号留着 —— 引用它的套装照旧吃这份
    {
        cfg.sets.clear();
        cfg.foods.clear();
        cfg.foodOrder.clear();
        LoadoutSet s;
        s.id = cfg.NextSetId();
        s.name = "留";
        cfg.sets.push_back(s);
        const int oldId = cfg.AllocFoodId();
        FoodPreset f;
        f.name = "旧饭";
        f.hp = 5;
        cfg.foods[oldId] = f;
        cfg.sets[0].food = oldId;   // 注意：s 是副本，push_back 之后再改 s 不会同步
        cfg.food = oldId;
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(5, oldId);
        for (int i = 0; i < 4; ++i) Frame();
        for (int i = 0; i < 8; ++i) Press(ImGuiKey_GamepadDpadDown);   // 档位与技能 → 确认 → 删除（耐力行已去掉）
        Press(ImGuiKey_GamepadFaceDown);
        Frame();
        Press(ImGuiKey_GamepadDpadDown);                              // 确认框里的「删除」
        Press(ImGuiKey_GamepadFaceDown);
        Frame();
        CHECK(cfg.foods.count(oldId) && cfg.foods[oldId].deleted, "删掉的猫饭留墓碑（id=%d 还在表里）", oldId);
        CHECK(cfg.foods[oldId].hp == 5 && cfg.foods[oldId].name == "旧饭",
              "删掉之后内容还在（进任务照旧吃得到）");
        CHECK(cfg.food == oldId && cfg.sets[0].food == oldId,
              "引用它的地方保持不变，不会静默退回「跟全局走」（food=%d set=%d）", cfg.food, cfg.sets[0].food);
        const int newId = cfg.AllocFoodId();
        CHECK(newId > oldId, "新建的猫饭不会复用删掉的编号（%d → %d）", oldId, newId);
        cfg.foods[newId] = FoodPreset();
        CHECK(cfg.FoodIds().size() == 1 && cfg.FoodIds()[0] == newId, "墓碑不出现在列表里（%d 项）",
              (int)cfg.FoodIds().size());
    }

    // 删掉但还被引用着的那份仍然留在列表里（内容还在，能再选回来）；引用断掉后才真正消失
    {
        cfg.sets.clear();
        cfg.foods.clear();
        cfg.foodOrder.clear();
        LoadoutSet s;
        s.id = cfg.NextSetId();
        s.name = "找";
        cfg.sets.push_back(s);
        const int gone = cfg.AllocFoodId();
        FoodPreset f;
        f.name = "旧饭";
        f.hp = 7;
        f.deleted = true;                 // 已经删掉（墓碑），但内容还在
        cfg.foods[gone] = f;
        cfg.sets[0].food = gone;
        cfg.food = -1;
        bool listed = false;
        for (int id : ui::FoodRowIds(cfg)) if (id == gone) listed = true;
        CHECK(listed, "被套装引用着的墓碑仍在列表里（id=%d）", gone);
        cfg.sets[0].food = -1;
        listed = false;
        for (int id : ui::FoodRowIds(cfg)) if (id == gone) listed = true;
        CHECK(!listed, "没人引用以后就从列表里消失");
    }
// 设置页「清空所有套装和猫饭」：两层确认之后才真的清
{
    ui::OnOpen(m);
    for (int i = 0; i < 3; ++i) Frame();
    Config& c = cfg;
    c.sets.clear();
    c.foods.clear();
    c.foodOrder.clear();
    for (int i = 0; i < 2; ++i) {
        LoadoutSet s;
        s.id = c.AllocSetId();
        s.name = "s" + std::to_string(i);
        c.sets.push_back(s);
    }
    for (int i = 0; i < 2; ++i) {
        const int id = c.AllocFoodId();
        FoodPreset f;
        f.name = "f" + std::to_string(i);
        c.foods[id] = f;
    }
    c.NormalizeFoodOrder();
    c.activeSet = c.sets[0].id;
    c.food = c.foods.begin()->first;
    m.reqSave = false;
    m.reqApplyEquip = false;

    ui::DebugGo(6, 0);
    Frame();
    ui::DebugFocusClearAll();
    Frame();
    Press(ImGuiKey_GamepadFaceDown);
    Frame();
    CHECK(ui::DebugClearStage() == 1 && !c.sets.empty(), "清空：第一层确认还不动数据（第 %d 层）", ui::DebugClearStage());
    Press(ImGuiKey_GamepadFaceDown);
    Frame();
    CHECK(ui::DebugClearStage() == 2 && !c.sets.empty(), "清空：第二层确认之前也不动数据（第 %d 层）", ui::DebugClearStage());
    Press(ImGuiKey_GamepadFaceDown);
    Frame();
    CHECK(c.sets.empty() && c.foods.empty() && c.foodOrder.empty(), "清空：确认之后套装和猫饭都没了");
    CHECK(c.activeSet == -1 && c.food == -1, "清空：当前选中也归零（%d / %d）", c.activeSet, c.food);
    CHECK(c.nextSetId == 1 && c.nextFoodId == 1, "清空：编号重新从 1 起（%d / %d）", c.nextSetId, c.nextFoodId);
    CHECK(m.reqSave, "清空之后要存盘（reqSave）");
    CHECK(m.reqApplyEquip, "清空之后撤销尚未执行的立即换装请求");
}


    // 主页的「重开」按 A
    ui::OnOpen(m);
    for (int i = 0; i < 4; ++i) Frame();
    Press(ImGuiKey_GamepadFaceDown);
    CHECK(m.reqRestart && m.reqClose, "主页直接按 A：重开并关面板");

    // ---- 装载态 / Dormant：A 只在装载态切换选择，Y 两种状态都能编辑 ----
    {
        m.reqClose = false;
        m.reqRestart = false;
        m.reqResetOnly = false;
        cfg.enabled = true;
        cfg.activeSet = -1;
        cfg.food = -1;
        cfg.sets.clear();
        cfg.foods.clear();
        cfg.foodOrder.clear();
        LoadoutSet a1;
        a1.id = cfg.AllocSetId(); a1.name = "L1";
        cfg.sets.push_back(a1);
        LoadoutSet a2;
        a2.id = cfg.AllocSetId(); a2.name = "L2";
        cfg.sets.push_back(a2);
        const int firstId = a1.id;

        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(2, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugPage() == 2 && cfg.activeSet == firstId, "装载态下 A 会套用（%d / 期望 %d）", cfg.activeSet, firstId);

        ui::DebugGo(6, 0);
        for (int i = 0; i < 4; ++i) Frame(); // 等进页默认焦点完成，再选择插件状态。
        ui::DebugFocusState();
        Frame();
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(!cfg.enabled, "设置页能切到卸载态");
        CHECK(cfg.activeSet == firstId, "切到卸载态保留当前套用，重新装载可恢复（%d / 期望 %d）", cfg.activeSet, firstId);

        cfg.foods[7] = FoodPreset();
        cfg.foodOrder = {7};
        cfg.food = 7;
        for (int page : {2, 4}) {
            for (bool selected : {false, true}) {
                cfg.activeSet = page == 2 && !selected ? -1 : firstId;
                cfg.food = selected ? 7 : -1;
                ui::DebugGo(page, 0);
                for (int i = 0; i < 4; ++i) Frame();
                const int active = cfg.activeSet, food = cfg.food;
                m.reqSave = false;
                m.reqApplyEquip = false;
                for (ImGuiKey key : {ImGuiKey_GamepadFaceDown, ImGuiKey_Space}) {
                    Press(key);
                    CHECK(ui::DebugPage() == page && cfg.activeSet == active && cfg.food == food &&
                          !m.reqSave && !m.reqApplyEquip,
                          "Dormant %s列表%s：%s 无操作，不改保留选择", PageName(page),
                          selected ? "已选中行" : "未选中行", key == ImGuiKey_Space ? "空格" : "A");
                }
                Press(selected ? ImGuiKey_GamepadFaceUp : ImGuiKey_Y);
                CHECK(ui::DebugPage() == page + 1 && cfg.activeSet == active && cfg.food == food &&
                      !m.reqSave && !m.reqApplyEquip,
                      "Dormant %s列表%s：Y 仍直接编辑，不改 activeSet/food", PageName(page),
                      selected ? "已选中行" : "未选中行");
            }
        }
        cfg.activeSet = firstId;

        ui::DebugGo(6, 0);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugFocusState();
        Frame();
        Press(ImGuiKey_GamepadDpadUp);
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(cfg.enabled, "能切回装载态");

        ui::DebugGo(2, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugPage() == 2 && cfg.activeSet == -1, "切回装载态后 A 恢复正常切换，先取消原套用（%d）", cfg.activeSet);
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugPage() == 2 && cfg.activeSet == firstId, "切回装载态后 A 能再次套用（%d）", cfg.activeSet);
    }

    TestPalicoControls();
    TestSettingsTransfer();

    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
    std::printf("\n%s（%d 项失败）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
