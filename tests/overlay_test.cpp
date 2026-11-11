// 在游戏外模拟「游戏每帧 Present」，测面板挂载、改分辨率，以及手柄过滤器。
//   overlay_test.exe <输出目录>
#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <xinput.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <cfloat>

#include "../src/config.h"
#include "../src/overlay.h"
#include "../src/padfilter.h"
#include "../src/ui.h"
#include "../src/i18n.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace ui {
void DebugFocusSearch();
void DebugFocusHotkey(bool pad);
void DebugFocusSettingsTransfer(bool importing);
void DebugFocusPickEntry(const wchar_t* name);
bool DebugSettingsConfirm();
}

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) std::printf("  ok   " __VA_ARGS__); \
    else { std::printf("  FAIL " __VA_ARGS__); ++g_fail; } std::printf("\n"); } while (0)

// ---- 模拟游戏：它自己的导入表里有 XInputGetState，padfilter 要能找到并换掉 ----
static DWORD GameReadPad(DWORD i, XINPUT_STATE* s) { return XInputGetState(i, s); }

static Config g_cfg;
static ui::Model g_model;
static int g_draws = 0, g_opened = 0;
static std::wstring g_shot;
static ID3D11Device* g_dev = nullptr;

static void Save(void* tex)
{
    if (g_shot.empty()) return;
    ID3D11Texture2D* bb = static_cast<ID3D11Texture2D*>(tex);
    ID3D11DeviceContext* ctx = nullptr;
    g_dev->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    g_dev->CreateTexture2D(&d, nullptr, &st);
    ctx->CopyResource(st, bb);
    D3D11_MAPPED_SUBRESOURCE map{};
    ctx->Map(st, 0, D3D11_MAP_READ, 0, &map);
    IWICImagingFactory* f = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    IWICStream* s = nullptr; f->CreateStream(&s); s->InitializeFromFilename(g_shot.c_str(), GENERIC_WRITE);
    IWICBitmapEncoder* enc = nullptr; f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    enc->Initialize(s, WICBitmapEncoderNoCache);
    IWICBitmapFrameEncode* fr = nullptr; enc->CreateNewFrame(&fr, nullptr); fr->Initialize(nullptr);
    fr->SetSize(d.Width, d.Height);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    fr->SetPixelFormat(&fmt);
    fr->WritePixels(d.Height, map.RowPitch, map.RowPitch * d.Height, (BYTE*)map.pData);
    fr->Commit(); enc->Commit();
    fr->Release(); enc->Release(); s->Release(); f->Release();
    ctx->Unmap(st, 0); st->Release(); ctx->Release();
    std::wprintf(L"  截图 %ls\n", g_shot.c_str());
    g_shot.clear();
}

// Atlas-only input isolation: platform polling and queued OS messages run before
// NewFramePre. Replay scripted Add* events here so real SourceTracker/navigation
// still consume them, without a physical mouse/gamepad stealing focus or source.
struct AtlasInput { ImGuiKey key; bool down; float wheel; };
static bool g_atlasInput = false;
static std::vector<AtlasInput> g_atlasEvents;
static void AtlasKey(ImGuiKey key, bool down) { g_atlasEvents.push_back({key, down, 0}); }
static void AtlasWheel(float wheel) { g_atlasEvents.push_back({ImGuiKey_None, false, wheel}); }
static void AtlasNewFrame(ImGuiContext* context, ImGuiContextHook*)
{
    if (!g_atlasInput) return;
    ImGuiIO& io = context->IO;
    io.ClearEventsQueue(); // Backend events only; script has not been enqueued yet.
    io.DeltaTime = 1.0f / 60.0f;
    // Win32 clears this without physical hardware. Scripted pad events need
    // the advertised backend capability or ImGui clears KeysData before use.
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    // Respect production's deliberate nav suppression while moving/capturing.
    if (!ui::DebugMoveActive() && !g_model.capturing)
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); // No live cursor hover/auto-focus.
    for (const auto& event : g_atlasEvents) {
        if (event.key != ImGuiKey_None) io.AddKeyEvent(event.key, event.down);
        else io.AddMouseWheelEvent(0, event.wheel);
    }
    g_atlasEvents.clear();
}
static void OnInit(float scale)
{
    ui::Init(scale);
    ImGuiContextHook hook;
    hook.Type = ImGuiContextHookType_NewFramePre;
    hook.Callback = AtlasNewFrame;
    ImGui::AddContextHook(ImGui::GetCurrentContext(), &hook);
}
static void OnOpened() { ui::OnOpen(g_model); ++g_opened; }
static void OnDraw(float w, float h) { ui::Draw(g_model, w, h); ++g_draws; }

// Bounds are actual unwrapped rendered groups, not the clip rect: clipping an
// overlong footer must not make this pass. Reuse the existing screenshot atlas.
static void CheckCompactFooter(int height, const char* context)
{
    const float requestedScale = height / 1080.0f;
    const float scale = requestedScale < 0.7f ? 0.7f : (requestedScale > 2.5f ? 2.5f : requestedScale);
    const auto bounds = ui::DebugHintBounds();
    const auto items = ui::DebugHintItems();
    float top = 0, bottom = 0, contentBottom = 0;
    const bool hasLayout = ui::DebugHintLayout(top, bottom, contentBottom);
    CHECK(ui::DebugHintHeight() > 0 && ui::DebugHintHeight() <= 32.0f * scale + 0.5f &&
          hasLayout && top >= contentBottom - 0.5f &&
          top >= 0 && bottom > top && bottom - top <= 32.0f * scale + 0.5f && bottom <= height + 0.5f,
          "overlay compact footer <=32*S outside content context=%s source=%d page=%d lang=%d %dp",
          context, ui::DebugHintSource(), ui::DebugPage(), (int)i18n::Current(), height);
    bool atomicItems = !items.empty(), conciseNavigation = true;
    for (const auto& item : items) {
        atomicItems &= !item.empty() && item.find_first_of("\r\n") == std::string::npos;
        conciseNavigation &= item.find("Arrows") == std::string::npos &&
                             item.find("HJKL") == std::string::npos && item.find("Tab") == std::string::npos;
    }
    CHECK(conciseNavigation,
          "overlay actual footer never advertises Arrows/HJKL/Tab context=%s source=%d page=%d lang=%d %dp",
          context, ui::DebugHintSource(), ui::DebugPage(), (int)i18n::Current(), height);
    CHECK(atomicItems && bounds.rows == ui::DebugHintRows() && bounds.rows == 1 &&
          bounds.availableLeft >= 0 && bounds.availableRight <= ImGui::GetIO().DisplaySize.x + 0.5f &&
          bounds.availableRight > bounds.availableLeft && bounds.textRight > bounds.textLeft &&
          bounds.textLeft >= bounds.availableLeft - 0.5f && bounds.textRight <= bounds.availableRight + 0.5f &&
          bounds.textTop >= top - 0.5f && bounds.textBottom > bounds.textTop && bounds.textBottom <= bottom + 0.5f,
          "overlay actual groups fit in one row context=%s source=%d page=%d lang=%d %dp rows=%d",
          context, ui::DebugHintSource(), ui::DebugPage(), (int)i18n::Current(), height, bounds.rows);
    const auto rows = ui::DebugHintRowBounds();
    CHECK(rows.size() == 1 && rows[0].groupCount == static_cast<int>(items.size()) &&
          rows[0].right > rows[0].left && rows[0].bottom > rows[0].top &&
          rows[0].left >= bounds.availableLeft - 0.5f && rows[0].right <= bounds.availableRight + 0.5f &&
          rows[0].top >= top - 0.5f && rows[0].bottom <= bottom + 0.5f,
          "overlay single unwrapped row contains every actual group context=%s source=%d page=%d lang=%d %dp",
          context, ui::DebugHintSource(), ui::DebugPage(), (int)i18n::Current(), height);
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
          "overlay left/right anchored partitions retain 12*S gap context=%s source=%d page=%d lang=%d %dp",
          context, ui::DebugHintSource(), ui::DebugPage(), (int)i18n::Current(), height);
    const auto icons = ui::DebugHintIcons();
    CHECK(icons.keycaps >= 0 && icons.padButtons >= 0 && icons.shoulders >= 0 && icons.directional >= 0 &&
          icons.specialGlyphs == 0 && icons.maxCenterError >= 0 && icons.maxCenterError <= 1.0f,
          "overlay draw-list controls vertically centered without special glyphs context=%s source=%d page=%d lang=%d %dp",
          context, ui::DebugHintSource(), ui::DebugPage(), (int)i18n::Current(), height);
    CHECK(bounds.separatorRight > bounds.separatorLeft && bounds.separatorLeft >= bounds.availableLeft - 0.5f &&
          bounds.separatorRight <= bounds.availableRight + 0.5f &&
          bounds.sidebarBottom + scale <= bounds.separatorY + 0.01f &&
          bounds.separatorY >= contentBottom - 0.5f && bounds.separatorY + scale <= bounds.textTop + 0.01f,
          "overlay separators do not cross/text clears rule context=%s source=%d page=%d lang=%d %dp",
          context, ui::DebugHintSource(), ui::DebugPage(), (int)i18n::Current(), height);
}

static void Pad(XINPUT_STATE& s, WORD buttons) { std::memset(&s, 0, sizeof(s)); s.Gamepad.wButtons = buttons; }

int wmain(int argc, wchar_t** argv)
{
    const std::wstring out = argc > 1 ? argv[1] : L".";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // ---------------- 手柄过滤器 ----------------
    std::printf("== padfilter\n");
    const unsigned short V = XINPUT_GAMEPAD_BACK, M = XINPUT_GAMEPAD_START, A = XINPUT_GAMEPAD_A;
    XINPUT_STATE s;
    Pad(s, A | XINPUT_GAMEPAD_DPAD_UP);
    padfilter::TestFilter(0, &s);
    CHECK(s.Gamepad.wButtons == (A | XINPUT_GAMEPAD_DPAD_UP), "面板关着：手柄状态原样给游戏（不扣留任何键）");

    padfilter::SetBlock(true);
    Pad(s, A | XINPUT_GAMEPAD_DPAD_UP); s.Gamepad.bRightTrigger = 255; s.Gamepad.sThumbLX = 30000;
    padfilter::TestFilter(0, &s);
    CHECK(s.Gamepad.wButtons == 0 && s.Gamepad.bRightTrigger == 0 && s.Gamepad.sThumbLX == 0, "面板开着：全部挡住");
    padfilter::SetBlock(false);
    Pad(s, A); padfilter::TestFilter(0, &s);
    CHECK(s.Gamepad.wButtons == 0, "面板关了但 A 还按着：继续挡，免得漏给游戏");
    Pad(s, 0); padfilter::TestFilter(0, &s);
    Pad(s, A); padfilter::TestFilter(0, &s);
    CHECK(s.Gamepad.wButtons == A, "全部松开过之后：恢复正常");

    std::string info;
    XINPUT_STATE dummy;
    GameReadPad(0, &dummy);   // 让导入表项确实被用到
    // 先另外加载一个别的版本的 XInput（模拟我们自己的快捷键代码加载了 xinput1_4，
    // 而「游戏」导入的是另一个）：上一版就是栽在只认第一个找到的库上
    LoadLibraryW(L"xinput1_4.dll");
    LoadLibraryW(L"xinput9_1_0.dll");
    const bool pf = padfilter::Install(info);
    CHECK(pf, "在本进程的导入表里找到并替换 XInputGetState：%s", info.c_str());
    // 替换之后，「游戏」读手柄要经过过滤器：面板开着时读到的是全 0
    padfilter::SetBlock(true);
    XINPUT_STATE after{};
    const DWORD r = GameReadPad(0, &after);
    CHECK(r != ERROR_SUCCESS || after.Gamepad.wButtons == 0, "替换后游戏的调用经过过滤器（手柄%s接）", r == ERROR_SUCCESS ? "已" : "未");
    padfilter::SetBlock(false);

    // ---------------- 面板挂到 Present ----------------
    std::printf("== overlay\n");
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"binq_test";
    RegisterClassExW(&wc);
    HWND hw = CreateWindowExW(0, wc.lpszClassName, L"MHW", WS_OVERLAPPEDWINDOW, 0, 0, 1936, 1119, nullptr, nullptr, wc.hInstance, nullptr);
    RECT rc; GetClientRect(hw, &rc);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2; sd.BufferDesc.Width = rc.right; sd.BufferDesc.Height = rc.bottom;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hw; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &g_dev, nullptr, &ctx);
    CHECK(SUCCEEDED(hr), "建「游戏」的交换链 %ldx%ld", rc.right, rc.bottom);

    std::string err;
    LoadConfig(L"config\\BetterINQ.ini", g_cfg, err);
    i18n::Set(i18n::FromString(g_cfg.language));   // 面板语言跟着配置走，和 DLL 里一样
    g_model.cfg = &g_cfg;
    g_model.ready = true;
    g_model.version = "0.2";
    overlay::Callbacks cb;
    cb.init = OnInit; cb.opened = OnOpened; cb.draw = OnDraw;
    CHECK(overlay::Install(cb, err), "挂 Present / ResizeBuffers %s", err.c_str());
    overlay::SetDebugAfterDraw(Save);

    auto frame = [&](float r, float g, float b) {
        ID3D11Texture2D* bb = nullptr;
        sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
        ID3D11RenderTargetView* rtv = nullptr;
        g_dev->CreateRenderTargetView(bb, nullptr, &rtv);
        const float col[4] = {r, g, b, 1};
        ctx->ClearRenderTargetView(rtv, col);
        rtv->Release(); bb->Release();
        const int drawsBeforeFrame = g_draws;
        const HRESULT presented = sc->Present(0, 0);
        // Atlas checks need exactly one consumed ImGui frame. Emit diagnostics
        // only on failure rather than printing a success line for every settle.
        if (g_atlasInput && (FAILED(presented) || g_draws != drawsBeforeFrame + 1)) {
            CHECK(false, "atlas Present must draw exactly once hr=%08lx draws=%d->%d page=%d lang=%d",
                  (unsigned long)presented, drawsBeforeFrame, g_draws, ui::DebugPage(), (int)i18n::Current());
        }
    };

    for (int i = 0; i < 3; ++i) frame(0.25f, 0.35f, 0.25f);
    CHECK(g_draws == 0, "面板关着：一次都没画");
    overlay::SetOpen(true);
    for (int i = 0; i < 6; ++i) {
        if (i == 5) g_shot = out + L"\\overlay_1080p.png";
        frame(0.25f, 0.35f, 0.25f);
    }
    CHECK(g_opened == 1 && g_draws == 6, "打开后每帧都画（opened=%d draws=%d）", g_opened, g_draws);

    // 关闭再打开之间没有 Present：必须按新打开周期回调并先门控唤出 A/B。
    {
        const int openedBefore = g_opened;
        ui::DebugGo(6, 0);
        g_model.reqRestart = g_model.reqClose = false;
        overlay::SetOpen(false);
        overlay::SetOpen(true);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceRight, true);
        frame(0.25f, 0.35f, 0.25f);
        CHECK(g_opened == openedBefore + 1 && ui::DebugPage() == 0,
              "两次 Present 间快速关闭重开仍触发 opened 并回主页（%d → %d）", openedBefore, g_opened);
        CHECK(!ImGui::GetKeyData(ImGuiKey_GamepadFaceDown)->Down &&
              !ImGui::GetKeyData(ImGuiKey_GamepadFaceRight)->Down &&
              !g_model.reqRestart && !g_model.reqClose,
              "快速重开已 Arm 门控：唤出 A/B 不会重开任务或关闭面板");
        ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceDown, false);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceRight, false);
        frame(0.25f, 0.35f, 0.25f);
    }

    // 看样子用：设置页拉到底（快捷键那几格），以及从设置页切回主页途中的一帧
    ui::DebugGo(6, 0);
    for (int i = 0; i < 20; ++i) frame(0.25f, 0.35f, 0.25f);
    for (ImGuiWindow* w : GImGui->Windows)
        if (std::strstr(w->Name, "/##content_")) ImGui::SetScrollY(w, w->ScrollMax.y);
    for (int i = 0; i < 3; ++i) {
        if (i == 2) g_shot = out + L"\\overlay_settings.png";
        frame(0.25f, 0.35f, 0.25f);
    }
    // 看样子用：套装列表里按 X / F 弹出的改名框
    ui::DebugGo(2, 0);
    for (int i = 0; i < 6; ++i) frame(0.25f, 0.35f, 0.25f);
    ui::DebugOpenRename();
    for (int i = 0; i < 10; ++i) {
        if (i == 9) g_shot = out + L"\\overlay_rename.png";
        frame(0.25f, 0.35f, 0.25f);
    }
    ui::DebugCloseRename();
    for (int i = 0; i < 4; ++i) frame(0.25f, 0.35f, 0.25f);
    // 看样子用：新建套装时就能起名
    ui::DebugGo(2, 0);
    for (int i = 0; i < 6; ++i) frame(0.25f, 0.35f, 0.25f);
    ui::DebugNewSet(g_model);
    for (int i = 0; i < 10; ++i) {
        if (i == 9) g_shot = out + L"\\overlay_rename_new.png";
        frame(0.25f, 0.35f, 0.25f);
    }
    ui::DebugCloseRename();
    for (int i = 0; i < 6; ++i) frame(0.25f, 0.35f, 0.25f);
    // 看样子用：按左摇杆进入移动模式
    ui::DebugGo(2, 0);
    for (int i = 0; i < 6; ++i) frame(0.25f, 0.35f, 0.25f);
    ui::DebugMove(1, g_cfg.sets.empty() ? 1 : g_cfg.sets[0].id);
    for (int i = 0; i < 6; ++i) {
        if (i == 5) g_shot = out + L"\\overlay_move.png";
        frame(0.25f, 0.35f, 0.25f);
    }
    ui::DebugMove(0, 0);
    for (int i = 0; i < 4; ++i) frame(0.25f, 0.35f, 0.25f);
    // 看样子用：从原版 IN-Q 导入的文件选择框
    ui::DebugOpenPick();
    for (int i = 0; i < 8; ++i) {
        if (i == 7) g_shot = out + L"\\overlay_pick.png";
        frame(0.25f, 0.35f, 0.25f);
    }
    ui::DebugClosePick();
    for (int i = 0; i < 4; ++i) frame(0.25f, 0.35f, 0.25f);
    ui::DebugGo(3, 0);
    for (int i = 0; i < 30; ++i) {
        if (i == 29) g_shot = out + L"\\overlay_setedit.png";
        frame(0.25f, 0.35f, 0.25f);
    }
    // Modal preview: wait for ImGui's dim animation before capturing.
    ui::DebugOpenDelete();
    for (int i = 0; i < 8; ++i) {
        if (i == 7) g_shot = out + L"\\overlay_delete_1080p.png";
        Sleep(40); frame(0.25f, 0.35f, 0.25f);
    }
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); frame(0.25f, 0.35f, 0.25f);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false); frame(0.25f, 0.35f, 0.25f);
    ui::DebugGo(6, 0);
    for (int i = 0; i < 30; ++i) frame(0.25f, 0.35f, 0.25f);
    ui::DebugGo(0, 0);
    for (int i = 0; i < 4; ++i) {
        if (i == 3) g_shot = out + L"\\overlay_anim.png";
        frame(0.25f, 0.35f, 0.25f);
    }
    for (int i = 0; i < 30; ++i) frame(0.25f, 0.35f, 0.25f);
    // 看样子用：假名搜索过滤「换任务」（でんせつ 只该剩下 伝説の黒龍 这一类的任务）
    ui::DebugGo(1, 0);
    for (int i = 0; i < 20; ++i) frame(0.25f, 0.35f, 0.25f);
    ui::DebugSetSearch("でんせつ");
    for (int i = 0; i < 4; ++i) {
        if (i == 3) g_shot = out + L"\\overlay_search.png";
        frame(0.25f, 0.35f, 0.25f);
    }
    ui::DebugSetSearch("");
    ui::DebugGo(0, 0);
    for (int i = 0; i < 10; ++i) frame(0.25f, 0.35f, 0.25f);
    // 进程里还有别人建的 D3D11 交换链（别的模组、工具窗口）：同一张虚表，Present 也会走到钩子里，
    // 但面板只能画在游戏自己的交换链上
    {
        HWND hw2 = CreateWindowExW(0, wc.lpszClassName, L"other", WS_OVERLAPPEDWINDOW, 0, 0, 656, 519, nullptr, nullptr,
                                   wc.hInstance, nullptr);
        DXGI_SWAP_CHAIN_DESC sd2 = sd;
        RECT rc2; GetClientRect(hw2, &rc2);
        sd2.BufferDesc.Width = rc2.right; sd2.BufferDesc.Height = rc2.bottom; sd2.OutputWindow = hw2;
        IDXGISwapChain* sc2 = nullptr; ID3D11Device* dev2 = nullptr; ID3D11DeviceContext* ctx2 = nullptr;
        D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd2, &sc2,
                                      &dev2, nullptr, &ctx2);
        const int before = g_draws;
        for (int i = 0; i < 5; ++i) if (sc2) sc2->Present(0, 0);
        CHECK(sc2 && g_draws == before, "别人的交换链 Present 了 5 次：面板一次都没往上画（draws %d → %d）", before, g_draws);
        frame(0.25f, 0.35f, 0.25f);
        CHECK(g_draws == before + 1, "游戏自己的交换链照常画（draws=%d）", g_draws);
        if (sc2) sc2->Release();
        if (ctx2) ctx2->Release();
        if (dev2) dev2->Release();
        DestroyWindow(hw2);
    }
    // Reproducible footer atlas: every page, language and actual IO source at
    // both supported scales.  Never use a debug setter for the input modality.
    auto hintAtlas = [&](int height) {
        // ResizeBuffers updates the viewport, but the production overlay only
        // initializes UI scale once. Explicitly exercise each atlas scale here.
        // Init clamps to [0.7, 2.5] and deduplicates its context input hook.
        ui::Init(height / 1080.0f);
        const auto oldLang = i18n::Current();
        const bool trickle = ImGui::GetIO().ConfigInputTrickleEventQueue;
        const bool hadGamepad = (ImGui::GetIO().BackendFlags & ImGuiBackendFlags_HasGamepad) != 0;
        const bool hadPadNavigation = (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) != 0;
        ImGui::GetIO().ConfigInputTrickleEventQueue = false;
        g_atlasEvents.clear();
        ImGui::GetIO().ClearInputKeys(); // Reset once, never erase scripted held state per frame.
        ImGui::GetIO().ClearInputMouse();
        g_atlasInput = true;
        auto openAtlas = [&](int page, int index) {
            g_model.reqClose = g_model.reqRestart = false;
            ui::OnOpen(g_model); ui::DebugGo(page, index);
        };
        for (auto language : {i18n::kZh, i18n::kEn, i18n::kJa}) {
            i18n::Set(language);
            for (int page = 0; page < 7; ++page) {
                openAtlas(page, page == 5 ? g_cfg.foods.begin()->first : 0);
                for (int n = 0; n < 45; ++n) frame(0.25f, 0.35f, 0.25f);
                for (int source = 0; source < 3; ++source) {
                    if (source == 0) AtlasKey(ImGuiKey_GamepadR2, true);
                    else if (source == 1) AtlasKey(ImGuiKey_Q, true);
                    else AtlasWheel(-1);
                    frame(0.25f, 0.35f, 0.25f);
                    CHECK(ui::DebugHintSource() == source && !ui::DebugHintText().empty(),
                          "footer atlas real IO source=%d page=%d lang=%d %dp", source, page, language, height);
                    CheckCompactFooter(height, "page atlas");
                    if (page == 2 || page == 4) {
                        const auto icons = ui::DebugHintIcons();
                        bool wasd = false, rename = false, secondaryCorrect = true;
                        for (const auto& item : ui::DebugHintItems()) {
                            wasd |= item.find("WASD") != std::string::npos;
                            rename |= item.find(std::string(source == 0 ? "X " : "F ") + i18n::Tr("改名", "Rename", "改名")) != std::string::npos;
                            if (item.find("LB/RB") != std::string::npos)
                                secondaryCorrect &= source == 0 && item.find(i18n::Tr("换位", "Shift", "並替")) != std::string::npos;
                            if (item.find("L3 ") != std::string::npos)
                                secondaryCorrect &= source == 0 && item.find(i18n::Tr("移动", "Move", "移動")) != std::string::npos;
                        }
                        CHECK((source == 2 || rename) && secondaryCorrect,
                              "overlay list rename retained and optional controls truthful source=%d %dp lang=%d page=%d", source, height, language, page);
                        if (source == 1)
                            CHECK(wasd && icons.keycaps > 0 &&
                                  icons.padButtons == 0 && icons.shoulders == 0,
                                  "overlay keyboard list WASD/keycaps %dp lang=%d page=%d", height, language, page);
                        else if (source == 0)
                            CHECK(icons.padButtons > 0 && icons.directional > 0,
                                  "overlay pad list circles/D-pad %dp lang=%d page=%d", height, language, page);
                    }
                    if (source == 0) AtlasKey(ImGuiKey_GamepadR2, false);
                    else if (source == 1) AtlasKey(ImGuiKey_Q, false);
                    if (page == 2 && ((height == 1080 && language == i18n::kEn) ||
                                      (height == 720 && source == 1)))
                        g_shot = out + L"\\\\overlay_hints_" + std::to_wstring(height) + L"_" +
                            std::to_wstring((int)language) + L"_page" + std::to_wstring(page) + L"_source" +
                            std::to_wstring(source) + L".png";
                    frame(0.25f, 0.35f, 0.25f);
                }
            }
            // Backup transfer contexts use real requests/navigation and a real
            // temporary file, while main remains the sole transfer IO owner.
            {
                wchar_t temp[MAX_PATH] = {}, unique[MAX_PATH] = {};
                GetTempPathW(MAX_PATH, temp); GetTempFileNameW(temp, L"biq", 0, unique);
                DeleteFileW(unique); CreateDirectoryW(unique, nullptr);
                const std::wstring fixture = std::wstring(unique) + L"\\settings.biq";
                HANDLE file = CreateFileW(fixture.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
                const std::wstring oldDir = g_model.backupsDir;
                g_model.backupsDir = unique;
                openAtlas(6, 0);
                for (int n = 0; n < 45; ++n) frame(0.25f, 0.35f, 0.25f);
                ui::DebugFocusSettingsTransfer(true);
                for (int n = 0; n < 6; ++n) frame(0.25f, 0.35f, 0.25f);
                if (language == i18n::kEn && height == 1080) {
                    g_shot = out + L"\\overlay_settings_backup.png";
                    frame(0.25f, 0.35f, 0.25f);
                }
                AtlasKey(ImGuiKey_GamepadFaceDown, true); frame(0.25f, 0.35f, 0.25f);
                AtlasKey(ImGuiKey_GamepadFaceDown, false);
                for (int n = 0; n < 6; ++n) frame(0.25f, 0.35f, 0.25f);
                CheckCompactFooter(height, "backup file picker");
                ui::DebugFocusPickEntry(L"settings.biq");
                for (int n = 0; n < 6; ++n) frame(0.25f, 0.35f, 0.25f);
                AtlasKey(ImGuiKey_GamepadFaceDown, true); frame(0.25f, 0.35f, 0.25f);
                AtlasKey(ImGuiKey_GamepadFaceDown, false);
                for (int n = 0; n < 6; ++n) frame(0.25f, 0.35f, 0.25f);
                CHECK(ui::DebugSettingsConfirm() && !g_model.reqImportSettings,
                      "overlay backup selection requires explicit confirmation %dp lang=%d", height, language);
                for (int source = 0; source < 3; ++source) {
                    if (source == 0) AtlasKey(ImGuiKey_GamepadR2, true);
                    else if (source == 1) AtlasKey(ImGuiKey_Q, true);
                    else AtlasWheel(-1);
                    frame(0.25f, 0.35f, 0.25f);
                    CheckCompactFooter(height, "backup overwrite confirmation");
                    if (source == 0) AtlasKey(ImGuiKey_GamepadR2, false);
                    else if (source == 1) AtlasKey(ImGuiKey_Q, false);
                    frame(0.25f, 0.35f, 0.25f);
                }
                if (language == i18n::kEn && height == 1080) {
                    g_shot = out + L"\\overlay_backup_confirm.png";
                    frame(0.25f, 0.35f, 0.25f);
                }
                AtlasKey(ImGuiKey_Escape, true); frame(0.25f, 0.35f, 0.25f);
                AtlasKey(ImGuiKey_Escape, false); frame(0.25f, 0.35f, 0.25f);
                CHECK(!g_model.reqImportSettings && !g_model.reqClose && ui::DebugPage() == 6,
                      "overlay backup Esc does not leak to page %dp lang=%d", height, language);
                DeleteFileW(fixture.c_str()); RemoveDirectoryW(unique);
                g_model.backupsDir = oldDir;
            }
            // Contexts that override the page's regular hints.
            for (int context = 0; context < 5; ++context) {
                openAtlas(context == 2 ? 3 : context >= 3 ? 6 : 2, 0);
                for (int n = 0; n < 45; ++n) frame(0.25f, 0.35f, 0.25f);
                if (context == 0) ui::DebugMove(1, g_cfg.sets[0].id);
                else if (context == 1) ui::DebugOpenRename();
                else if (context == 2) ui::DebugOpenDelete();
                else ui::DebugStartCapture(g_model, 0, context == 4);
                for (int n = 0; n < 12; ++n) frame(0.25f, 0.35f, 0.25f);
                CheckCompactFooter(height, "move/rename/delete/capture");
                if (language == i18n::kEn && height == 720)
                    g_shot = out + L"\\\\overlay_hintcontext_" + std::to_wstring(height) + L"_" +
                        std::to_wstring((int)language) + L"_" + std::to_wstring(context) + L".png";
                frame(0.25f, 0.35f, 0.25f);
                AtlasKey(ImGuiKey_Escape, true); frame(0.25f, 0.35f, 0.25f);
                AtlasKey(ImGuiKey_Escape, false); frame(0.25f, 0.35f, 0.25f);
            }
        }
        if (height == 1080) {
            // A small coherent Chinese/pad story, driven by real IO rather than
            // DebugMove/DebugOpenRename: default -> LS move -> X rename -> Y edit.
            i18n::Set(i18n::kZh);
            auto pressAtlas = [&](ImGuiKey key) {
                AtlasKey(key, true); frame(0.25f, 0.35f, 0.25f);
                AtlasKey(key, false); frame(0.25f, 0.35f, 0.25f);
                for (int n = 0; n < 12; ++n) frame(0.25f, 0.35f, 0.25f);
            };
            auto contains = [&](const char* text) {
                for (const auto& item : ui::DebugHintItems())
                    if (item.find(text) != std::string::npos) return true;
                return false;
            };
            auto shot = [&](const wchar_t* name) {
                CheckCompactFooter(height, "Chinese real-pad context story");
                g_shot = out + L"\\\\" + name;
                frame(0.25f, 0.35f, 0.25f);
            };
            openAtlas(2, 0);
            for (int n = 0; n < 45; ++n) frame(0.25f, 0.35f, 0.25f);
            pressAtlas(ImGuiKey_GamepadR2);
            const auto normalItems = ui::DebugHintItems();
            CHECK(contains("X 改名"), "Chinese pad default list advertises actual X rename");
            shot(L"overlay_story_1080_zh_pad_default.png");
            pressAtlas(ImGuiKey_GamepadL3);
            shot(L"overlay_story_1080_zh_pad_move.png");
            CHECK(ui::DebugMoveActive() && ui::DebugHintItems() != normalItems &&
                  contains("放下") && contains("移动") && contains("结束"),
                  "real LS activates move with Move/Drop/Finish hints, not default actions");
            pressAtlas(ImGuiKey_GamepadFaceRight);
            CHECK(!ui::DebugMoveActive() && ui::DebugPage() == 2 && !g_model.reqClose,
                  "real B finishes LS move without closing list");
            pressAtlas(ImGuiKey_GamepadFaceLeft);
            shot(L"overlay_story_1080_zh_pad_rename.png");
            CHECK(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && contains("输入名字") &&
                  contains("确定") && !contains("X 改名"), "real X opens rename and replaces list hints");
            pressAtlas(ImGuiKey_Escape);
            pressAtlas(ImGuiKey_GamepadFaceUp);
            shot(L"overlay_story_1080_zh_pad_edit.png");
            CHECK(ui::DebugPage() == 3 && ui::DebugHintItems() != normalItems,
                  "real Y enters editor with editor-specific hints");
            {
                const Config beforePalicoStory = g_cfg;
                const auto beforePalicoNames = g_model.palicoNames;
                g_cfg.palicoEnabled = true;
                g_model.palicoNames = {{2, "Mikan Hammer", 2}, {7, "Coral Orchestra", 7}};
                auto& palicoSet = g_cfg.sets.front();
                palicoSet.palicoLevel = 10;
                palicoSet.palicoGadgetLevel = 15;
                palicoSet.palicoLoadout = 7;
                for (int n = 0; n < 6; ++n) pressAtlas(ImGuiKey_GamepadDpadDown);
                pressAtlas(ImGuiKey_GamepadFaceDown);
                pressAtlas(ImGuiKey_GamepadDpadRight);
                CHECK(palicoSet.palicoLevel == 20 && ui::DebugPage() == 3,
                      "overlay real gamepad adjusts Palico level by +10");
                shot(L"overlay_story_1080_zh_pad_palico_controls.png");
                g_cfg = beforePalicoStory;
                g_model.palicoNames = beforePalicoNames;
            }
            ui::DebugOpenDelete();
            for (int n = 0; n < 12; ++n) frame(0.25f, 0.35f, 0.25f);
            shot(L"overlay_story_1080_zh_pad_delete.png");
            CHECK(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && contains("关闭") &&
                  !contains("X 改名"), "delete popup shows its own confirm/close context");
            pressAtlas(ImGuiKey_Escape);
            // Meals must be reached through its real sidebar target, not by
            // assuming a row index is a stable meal preset ID.
            const Config beforeMealStory = g_cfg;
            g_cfg = Config();
            LoadoutSet mealSet; mealSet.id = 17; mealSet.name = "猫饭导航回归"; mealSet.food = -1;
            g_cfg.sets.push_back(mealSet); g_cfg.activeSet = 17;
            g_cfg.foods[41] = FoodPreset(); g_cfg.foods[73] = FoodPreset();
            g_cfg.foods[41].name = "第一份猫饭"; g_cfg.foods[73].name = "第二份猫饭";
            g_cfg.foodOrder = {41, 73}; g_cfg.food = 41;
            openAtlas(0, 0);
            for (int n = 0; n < 45; ++n) frame(0.25f, 0.35f, 0.25f);
            pressAtlas(ImGuiKey_GamepadDpadLeft);
            pressAtlas(ImGuiKey_GamepadDpadDown); pressAtlas(ImGuiKey_GamepadDpadDown);
            AtlasKey(ImGuiKey_GamepadDpadRight, true); frame(0.25f, 0.35f, 0.25f);
            CHECK(contains("Y 编辑") && contains("X 改名"),
                  "overlay first draw after sidebar Right already shows meal actions");
            AtlasKey(ImGuiKey_GamepadDpadRight, false); frame(0.25f, 0.35f, 0.25f);
            CHECK(ui::DebugPage() == 4 && !ui::DebugSidebarFocused() && contains("Y 编辑") && contains("X 改名"),
                  "actual sidebar Meals content exposes Y edit and X rename");
            shot(L"overlay_story_1080_zh_pad_food.png");
            pressAtlas(ImGuiKey_GamepadL3);
            shot(L"overlay_story_1080_zh_pad_food_move.png");
            CHECK(ui::DebugMoveActive() && ui::DebugMoveId() == 41 && contains("放下") && contains("移动") &&
                  ui::DebugHintIcons().stickPress > 0,
                  "actual food LS press shows moving stable ID and press-down geometry");
            pressAtlas(ImGuiKey_GamepadFaceRight);
            pressAtlas(ImGuiKey_GamepadFaceUp);
            CHECK(ui::DebugPage() == 5, "actual food Y enters meal editor after LS finish");
            // 合法性提示：故意摆一份食堂做不出的（勇气 3 级 + 忍耐 3 级 = 要 12 个食材）。
            {
                auto& bad = g_cfg.foods[41];
                bad.hp = 5; bad.stamina = 2; bad.attack = 3; bad.defense = 0; bad.elemRes = 0;
                bad.skill[0] = 29; bad.skill[1] = 32; bad.skill[2] = -1;
                for (int n = 0; n < 3; ++n) frame(0.25f, 0.35f, 0.25f);
                shot(L"overlay_story_1080_zh_pad_meal_legality.png");
                pressAtlas(ImGuiKey_GamepadFaceRight);   // 回列表：看红色感叹号
                for (int n = 0; n < 4; ++n) frame(0.25f, 0.35f, 0.25f);
                shot(L"overlay_story_1080_zh_pad_food_illegal.png");
            }
            g_cfg.sets[0].food = 41;
            for (int fid : {41, 73}) {
                auto& food = g_cfg.foods[fid]; food.hp = 5; food.stamina = 2;
                food.skill[0] = 32; food.skill[1] = 9; food.skill[2] = 39;
            }
            openAtlas(2, 0);
            for (int n = 0; n < 45; ++n) frame(0.25f, 0.35f, 0.25f);
            pressAtlas(ImGuiKey_GamepadFaceUp);
            pressAtlas(ImGuiKey_GamepadDpadDown); pressAtlas(ImGuiKey_GamepadDpadDown); pressAtlas(ImGuiKey_GamepadDpadDown);
            g_model.reqSave = g_model.reqApplyEquip = false;
            auto checkMealPreview = [&](bool popup, int fid) {
                const auto b = ui::DebugFoodPreviewBounds();
                const auto text = ui::DebugFoodPreviewText();
                CHECK(b.visible && b.fromPopup == popup && b.foodId == fid && b.left >= 0 && b.top >= 0 &&
                      b.right > b.left && b.bottom > b.top && b.right <= 1920.5f && b.bottom <= 1080.5f &&
                      text.find("体力+50 耐力+50") != std::string::npos &&
                      text.find("大毅力 · 防御大 · 短催") != std::string::npos,
                      "overlay actual meal preview has bounded foreground card, bonuses and three skills popup=%d id=%d", popup, fid);
                CHECK(g_cfg.sets[0].food == 41 && !g_model.reqSave && !g_model.reqApplyEquip,
                      "overlay inspection does not apply meal or request save");
            };
            checkMealPreview(false, 41);
            shot(L"overlay_story_1080_zh_pad_meal_preview.png");
            pressAtlas(ImGuiKey_GamepadFaceDown);
            pressAtlas(ImGuiKey_GamepadDpadDown);
            checkMealPreview(true, 73);
            shot(L"overlay_story_1080_zh_pad_meal_candidate.png");
            pressAtlas(ImGuiKey_GamepadFaceRight);
            CHECK(g_cfg.sets[0].food == 41 && !g_model.reqSave, "overlay candidate preview B cancels without applying");
            g_cfg = beforeMealStory;
            openAtlas(0, 0);
        }
        if (height == 720) {
            i18n::Set(i18n::kEn);
            for (int context = 0; context < 5; ++context) {
                openAtlas(context == 0 ? 1 : context == 1 ? 0 : context == 2 ? 2 : 6, 0);
                for (int n = 0; n < 45; ++n) frame(0.25f, 0.35f, 0.25f);
                const bool enabled = g_cfg.enabled;
                if (context == 0) ui::DebugFocusSearch();
                else if (context == 1) AtlasKey(ImGuiKey_GamepadDpadLeft, true);
                else if (context == 2) g_cfg.enabled = false;
                else if (context == 3) ui::DebugFocusHotkey(false);
                else {
                    for (auto* w : GImGui->Windows) if (w->Active && std::strstr(w->Name, "##content")) {
                        ImGui::SetFocusID(w->GetID("##op"), w); ImGui::FocusWindow(w);
                        ImGui::SetNavCursorVisible(true); break;
                    }
                }
                for (int n = 0; n < 4; ++n) frame(0.25f, 0.35f, 0.25f);
                CheckCompactFooter(height, "search/sidebar/dormant/hotkey/slider");
                if (context == 1) AtlasKey(ImGuiKey_GamepadDpadLeft, false);
                g_shot = out + L"\\\\overlay_hints_special_720_" + std::to_wstring(context) + L".png";
                frame(0.25f, 0.35f, 0.25f);
                g_cfg.enabled = enabled;
            }
        }
        g_atlasInput = false;
        g_atlasEvents.clear();
        if (hadGamepad) ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;
        else ImGui::GetIO().BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
        if (hadPadNavigation) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        else ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        ImGui::GetIO().ConfigInputTrickleEventQueue = trickle;
        i18n::Set(oldLang); g_model.reqClose = g_model.reqRestart = false; ui::OnOpen(g_model);
        for (int n = 0; n < 45; ++n) frame(0.25f, 0.35f, 0.25f);
    };
    hintAtlas(1080);
    const int drawsBefore = g_draws;

    // 改分辨率：游戏切窗口大小时会调 ResizeBuffers，之前的后台缓冲引用必须先放掉，否则会失败
    hr = sc->ResizeBuffers(0, 1280, 720, DXGI_FORMAT_UNKNOWN, 0);
    CHECK(SUCCEEDED(hr), "ResizeBuffers 成功（hr=%08lx）", (unsigned long)hr);
    for (int i = 0; i < 4; ++i) {
        if (i == 3) g_shot = out + L"\\overlay_720p.png";
        frame(0.30f, 0.30f, 0.40f);
    }
    CHECK(g_draws == drawsBefore + 4, "改分辨率后照常画（draws=%d）", g_draws);
    const auto deleteLang = i18n::Current();
    const std::string deleteName = g_cfg.sets[0].name;
    for (auto language : {i18n::kZh, i18n::kEn, i18n::kJa}) {
        i18n::Set(language);
        g_cfg.sets[0].name.clear();
        const char* ch = language == i18n::kZh ? "长" : language == i18n::kJa ? "あ" : "W";
        for (int i = 0; i < 50; ++i) g_cfg.sets[0].name += ch;
        ui::DebugGo(3, 0);
        for (int i = 0; i < 20; ++i) frame(0.30f, 0.30f, 0.40f);
        ui::DebugOpenDelete();
        for (int i = 0; i < 8; ++i) {
            if (i == 7) g_shot = out + (language == i18n::kZh ? L"\\overlay_delete_720p_zh.png" :
                                       language == i18n::kEn ? L"\\overlay_delete_720p_en.png" : L"\\overlay_delete_720p_ja.png");
            Sleep(40); frame(0.30f, 0.30f, 0.40f);
        }
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); frame(0.30f, 0.30f, 0.40f);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false); frame(0.30f, 0.30f, 0.40f);
    }
    i18n::Set(deleteLang); g_cfg.sets[0].name = deleteName;
    hintAtlas(720);
    const int beforeClose = g_draws;
    overlay::SetOpen(false);
    frame(0, 0, 0);
    CHECK(g_draws == beforeClose, "关掉后不再画");

    sc->Release(); ctx->Release(); g_dev->Release();
    DestroyWindow(hw);
    std::printf("\n%s（%d 项失败）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
