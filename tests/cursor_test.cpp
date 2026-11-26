// 光标过滤的测试：过滤器编进测试程序自己，被换掉的就是测试程序导入表里的
// SetCursorPos / GetCursorPos / ClipCursor，直接调 ::SetCursorPos 就经过钩子，和游戏一样。
//
// 默认使用内存假光标后端完整验证冻结、恢复、窗口裁剪状态机（可在锁屏 / CI 无头环境运行）；
// 若桌面已解锁且未传入 --headless，再额外执行一次真实光标读写冒烟检查。
#include "cursorfilter.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) std::printf("  ok   " __VA_ARGS__); \
    else { std::printf("  FAIL " __VA_ARGS__); ++g_fail; } std::printf("\n"); } while (0)

typedef BOOL(WINAPI* SetPosFn)(int, int);
typedef BOOL(WINAPI* GetPosFn)(LPPOINT);
typedef BOOL(WINAPI* ClipFn)(const RECT*);

static bool Same(const POINT& a, const POINT& b) { return a.x == b.x && a.y == b.y; }
static bool Same(const RECT& a, const RECT& b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

struct FakeCursor {
    POINT pos{300, 200};
    RECT screen{0, 0, 1920, 1080};
    RECT clip{0, 0, 1920, 1080};
};
static FakeCursor g_fake;

static BOOL WINAPI FakeSetPos(int x, int y)
{
    g_fake.pos = {x, y};
    return TRUE;
}
static BOOL WINAPI FakeGetPos(LPPOINT p)
{
    if (!p) return FALSE;
    *p = g_fake.pos;
    return TRUE;
}
static BOOL WINAPI FakeClip(const RECT* r)
{
    g_fake.clip = r ? *r : g_fake.screen;
    return TRUE;
}
static BOOL WINAPI FakeGetClip(LPRECT r)
{
    if (!r) return FALSE;
    *r = g_fake.clip;
    return TRUE;
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

    HMODULE u = ::GetModuleHandleW(L"user32.dll");
    const auto realSet = reinterpret_cast<SetPosFn>(::GetProcAddress(u, "SetCursorPos"));
    const auto realGet = reinterpret_cast<GetPosFn>(::GetProcAddress(u, "GetCursorPos"));
    const auto realClip = reinterpret_cast<ClipFn>(::GetProcAddress(u, "ClipCursor"));

    std::string info;
    CHECK(cursorfilter::Install(info), "Install：%s", info.c_str());
    CHECK(reinterpret_cast<void*>(&::SetCursorPos) != reinterpret_cast<void*>(realSet) &&
          reinterpret_cast<void*>(&::GetCursorPos) != reinterpret_cast<void*>(realGet) &&
          reinterpret_cast<void*>(&::ClipCursor) != reinterpret_cast<void*>(realClip),
          "导入表里的三个函数都换成了钩子");

    // ---- 1. 假光标后端（无头 / 锁屏完整回归） ----
    g_fake = FakeCursor{};
    cursorfilter::TestOps ops{};
    ops.setPos = FakeSetPos;
    ops.getPos = FakeGetPos;
    ops.clip = FakeClip;
    ops.getClip = FakeGetClip;
    ops.virtualScreen = g_fake.screen;
    ops.hasGameWindow = false;
    cursorfilter::SetTestOps(&ops);

    POINT a{}, b{};
    CHECK(::GetCursorPos(&a) && Same(a, {300, 200}), "关着：GetCursorPos 放行读取 (%ld,%ld)", a.x, a.y);
    CHECK(::SetCursorPos(512, 384) && Same(g_fake.pos, {512, 384}), "关着：SetCursorPos 放行写入");
    const RECT full = g_fake.screen;
    const RECT r{full.left, full.top, full.right - 1, full.bottom - 1};
    CHECK(::ClipCursor(&r) && Same(g_fake.clip, r), "关着：ClipCursor 直接生效");
    FakeClip(nullptr);

    // 面板开着：冻结位置并延迟应用游戏移动与锁定
    const POINT start{512, 384};
    cursorfilter::SetBlock(true);
    POINT g{};
    CHECK(::GetCursorPos(&g) && Same(g, start), "开面板那一刻的位置被冻住 (%ld,%ld)", g.x, g.y);

    const POINT t{462, 334};
    CHECK(::SetCursorPos(t.x, t.y), "开着：SetCursorPos 返回成功");
    CHECK(Same(g_fake.pos, start), "开着：假底层光标没被挪 (%ld,%ld)", g_fake.pos.x, g_fake.pos.y);
    CHECK(::GetCursorPos(&g) && Same(g, t), "开着：游戏读到的就是它刚想放的位置 (%ld,%ld)", g.x, g.y);
    const POINT desired{640, 360};
    ::SetCursorPos(desired.x, desired.y);

    CHECK(::ClipCursor(&r), "开着：ClipCursor 返回成功");
    CHECK(Same(g_fake.clip, full), "开着：游戏锁不住光标");

    cursorfilter::SetBlock(false);
    CHECK(Same(g_fake.pos, desired), "关面板：光标停在游戏最后想放的位置 (%ld,%ld)", g_fake.pos.x, g_fake.pos.y);
    CHECK(Same(g_fake.clip, r), "关面板：补上游戏在面板开着时要的锁定");

    // 游戏先锁着，无前景游戏窗口时开面板：先放开到整屏，关面板恢复原锁定
    cursorfilter::SetBlock(true);
    CHECK(Same(g_fake.clip, full), "已锁着时开面板（无游戏窗）：放开");
    cursorfilter::SetBlock(false);
    CHECK(Same(g_fake.clip, r), "关面板：恢复原来的锁定");

    // 游戏先锁着，有游戏窗口时开面板：改锁进游戏窗口客户区，关面板恢复游戏想要的锁定
    ops.hasGameWindow = true;
    ops.gameWindow = {100, 100, 900, 600};
    cursorfilter::SetTestOps(&ops);
    cursorfilter::SetBlock(true);
    CHECK(Same(g_fake.clip, ops.gameWindow), "已锁着时开面板（有游戏窗）：改锁到游戏窗口客户区");
    cursorfilter::SetBlock(false);
    CHECK(Same(g_fake.clip, r), "关面板：从窗口裁剪恢复原游戏锁定");

    // 开着期间游戏自己放开了：关面板后保持放开
    cursorfilter::SetBlock(true);
    ::ClipCursor(nullptr);
    cursorfilter::SetBlock(false);
    CHECK(Same(g_fake.clip, full), "开着期间游戏放开了锁定 → 关面板后不再锁");

    // 关着后再次放行
    CHECK(::GetCursorPos(&a) && ::FakeGetPos(&b) && Same(a, b), "关着：GetCursorPos 再次放行");
    CHECK(::ClipCursor(&r) && Same(g_fake.clip, r), "关着：ClipCursor 再次直接生效");

    cursorfilter::SetTestOps(nullptr);

    // ---- 2. 真实桌面冒烟（仅在未锁屏且未指定 --headless 时执行） ----
    POINT liveProbe{};
    if (!forceHeadless && realGet(&liveProbe) != FALSE) {
        CHECK(::GetCursorPos(&a), "live smoke：真实桌面 GetCursorPos 放行");
        cursorfilter::SetBlock(true);
        CHECK(::GetCursorPos(&b), "live smoke：开面板下 GetCursorPos 可读");
        cursorfilter::SetBlock(false);
    } else {
        std::printf("  info: desktop locked (or --headless), live cursor smoke skipped\n");
    }

    std::printf("\n%s（%d 项失败）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
