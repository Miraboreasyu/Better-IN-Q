// Isolated Win32/IMM regression with headless FakeImm + optional live Win32 IMM pass.
// Never loads or modifies the game.
#include "../src/overlay.cpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

static int failures = 0;
#define CHECK(x) do { const bool ok = (x); std::printf("%s %s\n", ok ? "ok" : "FAIL", #x); if (!ok) ++failures; } while (0)
static LRESULT CALLBACK GameProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcW(h, m, w, l);
}
static void Drain()
{
    MSG m{};
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}
static void Activate(HWND h)
{
    overlay::g_hwnd = h;
    overlay::g_inited = true;
    overlay::g_dead = false;
    overlay::g_failed = false;
    overlay::g_imeEnabled = true;
    overlay::g_wndHook = true;
    overlay::SetOpen(true);
    overlay::ImeRequest(true, false);
}

// ---- Thread-safe in-memory FakeImm backend (runs cleanly on locked desktops)
struct FakeImmState {
    std::mutex mu;
    uintptr_t nextHandle = 0x10000;
    std::unordered_set<HIMC> alive;
    std::unordered_map<HWND, HIMC> assoc;
    std::unordered_map<HIMC, COMPOSITIONFORM> compForm;
    std::unordered_map<HIMC, BOOL> openStatus;
};
static FakeImmState g_fakeImm;

static void ResetFakeImm()
{
    std::lock_guard<std::mutex> lk(g_fakeImm.mu);
    g_fakeImm.nextHandle = 0x10000;
    g_fakeImm.alive.clear();
    g_fakeImm.assoc.clear();
    g_fakeImm.compForm.clear();
    g_fakeImm.openStatus.clear();
}
static size_t FakeAliveCount()
{
    std::lock_guard<std::mutex> lk(g_fakeImm.mu);
    return g_fakeImm.alive.size();
}
static HIMC WINAPI FakeCreateContext()
{
    std::lock_guard<std::mutex> lk(g_fakeImm.mu);
    HIMC h = reinterpret_cast<HIMC>(g_fakeImm.nextHandle += 0x10);
    g_fakeImm.alive.insert(h);
    return h;
}
static BOOL WINAPI FakeDestroyContext(HIMC h)
{
    std::lock_guard<std::mutex> lk(g_fakeImm.mu);
    if (!g_fakeImm.alive.erase(h)) return FALSE;
    g_fakeImm.compForm.erase(h);
    g_fakeImm.openStatus.erase(h);
    for (auto it = g_fakeImm.assoc.begin(); it != g_fakeImm.assoc.end();) {
        if (it->second == h) it = g_fakeImm.assoc.erase(it);
        else ++it;
    }
    return TRUE;
}
static HIMC WINAPI FakeGetContext(HWND w)
{
    std::lock_guard<std::mutex> lk(g_fakeImm.mu);
    auto it = g_fakeImm.assoc.find(w);
    return it == g_fakeImm.assoc.end() ? nullptr : it->second;
}
static BOOL WINAPI FakeReleaseContext(HWND, HIMC) { return TRUE; }
static HIMC WINAPI FakeAssociateContext(HWND w, HIMC next)
{
    std::lock_guard<std::mutex> lk(g_fakeImm.mu);
    auto it = g_fakeImm.assoc.find(w);
    HIMC prev = it == g_fakeImm.assoc.end() ? nullptr : it->second;
    if (next) g_fakeImm.assoc[w] = next;
    else g_fakeImm.assoc.erase(w);
    return prev;
}
static BOOL WINAPI FakeNotifyIME(HIMC h, DWORD, DWORD, DWORD)
{
    std::lock_guard<std::mutex> lk(g_fakeImm.mu);
    return g_fakeImm.alive.count(h) ? TRUE : FALSE;
}
static BOOL WINAPI FakeSetOpenStatus(HIMC h, BOOL open)
{
    std::lock_guard<std::mutex> lk(g_fakeImm.mu);
    if (!g_fakeImm.alive.count(h)) return FALSE;
    g_fakeImm.openStatus[h] = open;
    return TRUE;
}
static BOOL WINAPI FakeSetCompositionWindow(HIMC h, LPCOMPOSITIONFORM cf)
{
    std::lock_guard<std::mutex> lk(g_fakeImm.mu);
    if (!cf || !g_fakeImm.alive.count(h)) return FALSE;
    g_fakeImm.compForm[h] = *cf;
    return TRUE;
}
static BOOL WINAPI FakeGetCompositionWindow(HIMC h, LPCOMPOSITIONFORM cf)
{
    std::lock_guard<std::mutex> lk(g_fakeImm.mu);
    if (!cf) return FALSE;
    auto it = g_fakeImm.compForm.find(h);
    if (it == g_fakeImm.compForm.end()) return FALSE;
    *cf = it->second;
    return TRUE;
}
static BOOL WINAPI FakeSetCandidateWindow(HIMC h, LPCANDIDATEFORM)
{
    std::lock_guard<std::mutex> lk(g_fakeImm.mu);
    return g_fakeImm.alive.count(h) ? TRUE : FALSE;
}
static LONG WINAPI FakeGetCompositionStringW(HIMC, DWORD, LPVOID, DWORD) { return 0; }

struct FakeCandidates final : ITfCandidateListUIElement {
 UINT pageSize=9; bool failString=false,failPage=false;
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) override{return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef() override{return 2;} ULONG STDMETHODCALLTYPE Release() override{return 1;}
 HRESULT STDMETHODCALLTYPE GetDescription(BSTR*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetGUID(GUID*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE Show(BOOL) override{return S_OK;}
 HRESULT STDMETHODCALLTYPE IsShown(BOOL* v) override{*v=TRUE;return S_OK;}
 HRESULT STDMETHODCALLTYPE GetUpdatedFlags(DWORD* v) override{*v=0;return S_OK;}
 HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr**) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetCount(UINT* v) override{*v=pageSize*2;return S_OK;}
 HRESULT STDMETHODCALLTYPE GetSelection(UINT* v) override{*v=pageSize+1;return S_OK;}
 HRESULT STDMETHODCALLTYPE GetString(UINT i,BSTR* v) override{if(failString)return E_FAIL;*v=SysAllocString((L"word"+std::to_wstring(i)).c_str());return S_OK;}
 HRESULT STDMETHODCALLTYPE GetPageIndex(UINT* v,UINT cap,UINT* got) override{if(failPage)return E_FAIL;*got=2;if(cap>=2){v[0]=0;v[1]=pageSize;}return S_OK;}
 HRESULT STDMETHODCALLTYPE SetPageIndex(UINT*,UINT) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetCurrentPage(UINT* v) override{*v=1;return S_OK;}
};

static bool RunImeWindowSuite(const overlay::ImmOps& immOps,
                              BOOL(WINAPI* getCompWin)(HIMC, LPCOMPOSITIONFORM),
                              const wchar_t* className, const char* label)
{
    std::printf("-- %s --\n", label);
    const overlay::ImmOps savedImm = overlay::g_imm;
    overlay::g_imm = immOps;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = GameProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    CHECK(RegisterClassExW(&wc) != 0);
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"isolated IME regression", WS_OVERLAPPEDWINDOW,
        0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
    CHECK(h != nullptr);
    if (!h) { overlay::g_imm = savedImm; UnregisterClassW(wc.lpszClassName, wc.hInstance); return false; }

    HIMC original = overlay::ImeContext(h);
    HIMC a = overlay::g_imm.createContext();
    HIMC b = overlay::g_imm.createContext();
    CHECK(a && b);
    if (!a || !b) {
        if (a) overlay::g_imm.destroyContext(a);
        if (b) overlay::g_imm.destroyContext(b);
        DestroyWindow(h);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        overlay::g_imm = savedImm;
        return false;
    }
    overlay::g_imm.associateContext(h, a);
    ImGui::CreateContext();
    CHECK(ImGui_ImplWin32_Init(h));
    SetPropW(h, overlay::kPreviousProc, reinterpret_cast<HANDLE>(GameProc));
    SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(overlay::WndProc));

    Activate(h);
    CHECK(overlay::g_imeOwner == h && overlay::g_imc != nullptr);
    CHECK(overlay::ImeContext(h) == overlay::g_imc);
    CHECK(overlay::g_imcPrev == a);
    overlay::SetOpen(false);
    CHECK(overlay::ImeContext(h) == a);
    CHECK(!overlay::g_imc && !overlay::g_imeOwner);

    Activate(h);
    overlay::ImeRequest(true,true);
    { std::lock_guard<std::mutex> lock(ime_candidates::mutex); ime_candidates::view.composition=L"hei"; }
    SendMessageW(h,WM_KEYDOWN,VK_RETURN,0);
    SendMessageW(h,WM_KEYDOWN,VK_ESCAPE,0);
    CHECK(overlay::GetImeText().composition=="hei");
    overlay::SetOpen(false);
    CHECK(overlay::GetImeText().composition.empty() && ime_candidates::Copy().composition.empty());

    // Preserve the game's latest replacement, not the first saved context.
    Activate(h);
    overlay::g_imm.associateContext(h, b);
    SendMessageW(h, overlay::kImeSync, 0, 0);
    CHECK(overlay::g_imcPrev == b);
    overlay::SetOpen(false);
    CHECK(overlay::ImeContext(h) == b);
    Activate(h);
    overlay::g_imm.associateContext(h, nullptr);
    SendMessageW(h, overlay::kImeSync, 0, 0);
    CHECK(overlay::g_imcPrev == nullptr);
    overlay::SetOpen(false);
    CHECK(overlay::ImeContext(h) == nullptr);

    // If the game already took ownership at close, do not overwrite it.
    overlay::g_imm.associateContext(h, a);
    Activate(h);
    overlay::g_imm.associateContext(h, b);
    overlay::SetOpen(false);
    CHECK(overlay::ImeContext(h) == b);

    // A renderer/worker publishes close, but no later Present is needed.
    Activate(h);
    std::thread closer([] { overlay::SetOpen(false); });
    closer.join();
    CHECK(overlay::g_imc != nullptr); // UI queue intentionally not pumped yet.
    SendMessageW(h, WM_NULL, 0, 0);   // First game message performs restoration.
    CHECK(!overlay::g_imc && overlay::ImeContext(h) == b);
    Drain();
    CHECK(overlay::ImeContext(h) == b);

    // Cleanup messages remain effective even when the renderer is inactive.
    Activate(h);
    overlay::g_inited = false;
    overlay::g_wasOpen = false;
    std::thread inactiveClose([] { overlay::SetOpen(false); });
    inactiveClose.join();
    Drain();
    CHECK(!overlay::g_imc && overlay::ImeContext(h) == b);

    // Stale position messages may not modify a restored game HIMC.
    COMPOSITIONFORM before{};
    before.dwStyle = CFS_POINT;
    before.ptCurrentPos.x = 17;
    before.ptCurrentPos.y = 23;
    overlay::g_imm.setCompositionWindow(b, &before);
    COMPOSITIONFORM saved{};
    const bool readable = getCompWin(b, &saved) != FALSE;
    Activate(h);
    const unsigned oldGeneration = overlay::g_imeGeneration.load();
    overlay::SetOpen(false);
    SendMessageW(h, overlay::kImePos, oldGeneration, MAKELPARAM(200, 210));
    COMPOSITIONFORM after{};
    if (readable) {
        CHECK(getCompWin(b, &after));
        CHECK(after.dwStyle == saved.dwStyle && after.ptCurrentPos.x == saved.ptCurrentPos.x &&
            after.ptCurrentPos.y == saved.ptCurrentPos.y);
    }
    CHECK(overlay::ImeContext(h) == b);

    // Old sync/position messages cannot resurrect a closed ownership generation.
    Activate(h);
    overlay::SetOpen(false);
    Activate(h);
    HIMC current = overlay::g_imc;
    SendMessageW(h, overlay::kImeSync, 0, 0);
    SendMessageW(h, overlay::kImePos, oldGeneration, MAKELPARAM(300, 310));
    CHECK(overlay::g_imc == current && overlay::ImeContext(h) == current);
    overlay::SetOpen(false);

    // Cleanup is scoped to the owner HWND; unrelated windows are untouched.
    HWND other = CreateWindowExW(0, wc.lpszClassName, L"other", WS_OVERLAPPEDWINDOW,
        0, 0, 80, 80, nullptr, nullptr, wc.hInstance, nullptr);
    Activate(h);
    current = overlay::g_imc;
    overlay::ImeOffWindowThread(other);
    CHECK(overlay::g_imeOwner == h && overlay::g_imc == current);
    DestroyWindow(other);
    // Destruction cleans ownership before the HWND becomes invalid.
    DestroyWindow(h);
    CHECK(!overlay::g_imc && !overlay::g_imeOwner);
    CHECK(!overlay::PreviousProc(h));
    overlay::g_inited = false;
    overlay::g_hwnd = nullptr;
    overlay::g_openState.fetch_and(~1ull, std::memory_order_acq_rel);
    overlay::g_imeAttached = false;
    overlay::g_imeWant = false;
    overlay::g_wasOpen = false;
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    (void)original; // Default window context is owned by the system.
    overlay::g_imm.destroyContext(a);
    overlay::g_imm.destroyContext(b);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    overlay::g_imm = savedImm;
    return true;
}

static bool IsHeadlessRequested(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--headless") == 0) return true;
    const char* env = std::getenv("BETTERINQ_HEADLESS");
    return env && env[0] == '1' && env[1] == '\0';
}

int main(int argc, char** argv)
{
    const bool forceHeadless = IsHeadlessRequested(argc, argv);

    FakeCandidates fake;
    ime_candidates::active=true;
    CHECK(ime_candidates::ReadList(&fake,42));
    auto candidates=ime_candidates::Copy();
    CHECK(candidates.candidates.size()==9 && candidates.candidates[0]==L"word9" && candidates.selected==1);
    BOOL show=TRUE;
    ime_candidates::sink.BeginUIElement(999,&show);
    CHECK(show==TRUE); // unavailable/unrelated UI must not be suppressed
    ime_candidates::sink.EndUIElement(41);
    CHECK(ime_candidates::Copy().candidates.size()==9);
    ime_candidates::sink.EndUIElement(42);
    CHECK(ime_candidates::Copy().candidates.empty());
    for(UINT size:{5u,7u,12u,20u}) {
        fake.pageSize=size;
        CHECK(ime_candidates::ReadList(&fake,42));
        auto page=ime_candidates::Copy();
        CHECK(page.candidates.size()==size && page.candidates.front()==L"word"+std::to_wstring(size) && page.selected==1);
    }
    fake.failString=true;
    CHECK(!ime_candidates::ReadList(&fake,42) && ime_candidates::Copy().candidates.empty());
    fake.failString=false;fake.failPage=true;
    CHECK(!ime_candidates::ReadList(&fake,42));
    fake.failPage=false;fake.pageSize=257;
    CHECK(!ime_candidates::ReadList(&fake,42)); // oversized pages fall back instead of truncating
    ime_candidates::Stop();
    CHECK(!ime_candidates::active && ime_candidates::Copy().composition.empty());

    // 1. Always run full IME window ownership/lifecycle regression on headless FakeImm.
    ResetFakeImm();
    overlay::ImmOps fakeOps{};
    fakeOps.getContext = FakeGetContext;
    fakeOps.releaseContext = FakeReleaseContext;
    fakeOps.createContext = FakeCreateContext;
    fakeOps.destroyContext = FakeDestroyContext;
    fakeOps.associateContext = FakeAssociateContext;
    fakeOps.notifyIme = FakeNotifyIME;
    fakeOps.setOpenStatus = FakeSetOpenStatus;
    fakeOps.setCompositionWindow = FakeSetCompositionWindow;
    fakeOps.setCandidateWindow = FakeSetCandidateWindow;
    fakeOps.getCompositionStringW = FakeGetCompositionStringW;
    CHECK(RunImeWindowSuite(fakeOps, FakeGetCompositionWindow, L"BetterINQ_ime_test_fake", "headless-fake-imm"));
    CHECK(FakeAliveCount() == 0);

    // 2. When desktop is unlocked (and --headless not set), also run against real Win32 IMM32.
    HIMC probe = forceHeadless ? nullptr : ::ImmCreateContext();
    if (probe) {
        ::ImmDestroyContext(probe);
        overlay::ImmOps realOps{};
        CHECK(RunImeWindowSuite(realOps, ::ImmGetCompositionWindow, L"BetterINQ_ime_test_live", "live-win32-imm"));
    } else {
        std::printf("info: ImmCreateContext unavailable (desktop locked or --headless), live Win32 IMM pass skipped\n");
    }

    std::printf("IME failures: %d\n", failures);
    return failures ? 1 : 0;
}
