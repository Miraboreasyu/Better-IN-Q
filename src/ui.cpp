#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui.h"
#include "ui_hints.h"

#include "canteen_rules.h"
#include "gamedata.h"
#include "i18n.h"
#include "log.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "input.h"

#include <windows.h>
#include <xinput.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ui {
namespace {

// ---------------------------------------------------------------------------
//  配色：怪猎菜单的深褐黑半透明底 + 金色。层次靠字号和亮度，不靠颜色种类。
// ---------------------------------------------------------------------------
const ImU32 kGold     = IM_COL32(226, 194, 122, 255);
const ImU32 kGoldHot  = IM_COL32(238, 211, 148, 255);
const ImU32 kGoldDown = IM_COL32(201, 164, 92, 255);
const ImU32 kInk      = IM_COL32(26, 20, 11, 255);     // 金底上的字
const ImU32 kTitle    = IM_COL32(246, 236, 210, 255);
const ImU32 kText     = IM_COL32(237, 230, 214, 255);
const ImU32 kMuted    = IM_COL32(150, 140, 118, 255);
const ImU32 kFaint    = IM_COL32(111, 102, 86, 255);
const ImU32 kWarn     = IM_COL32(232, 140, 110, 255);
const ImU32 kBad      = IM_COL32(226, 86, 78, 255);    // 非法：列表和预览里的红色感叹号
const ImU32 kSoft     = IM_COL32(237, 230, 214, 26);   // 次要按钮底
const ImU32 kSoftHot  = IM_COL32(237, 230, 214, 44);
ImVec4 V(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

float S = 1.0f;
ImFont* fReg = nullptr;
ImFont* fBold = nullptr;
float szBody, szSmall, szTitle, szPage, szBtn;

enum Page { kHome, kQuests, kSets, kSetEdit, kFood, kFoodEdit, kSettings };
Page g_page = kHome;
float g_contentH[8] = {};   // 每页上一帧量出来的内容高度（0 = 还没量过）
// 进页后把焦点放到默认项。连续两帧都设：面板刚弹出那一帧 ImGui 自己的「窗口默认焦点」
// 会在帧尾把焦点挪走，第二帧再设一次才稳。
int g_focus = 2;
bool g_focusUsed = false;
int g_clearStage = 0;              // 设置页「清空」的两层确认走到第几层（0 = 没弹）
bool g_focusClearAll = false;
bool g_focusState = false;         // 测试用：下一帧把焦点放到设置页的「插件状态」上      // 测试用：下一帧把焦点放到「清空」按钮上
int g_editSet = -1;         // 正在编辑的全套（cfg.sets 下标）
int g_editFood = 0;         // 正在编辑的猫饭编号
// 改名：列表里选中一项后按 X（手柄）或 F（键盘）弹输入框
enum RenameKind { kRenameNone, kRenameSet, kRenameFood };
RenameKind g_renameKind = kRenameNone;
int g_renameId = 0;              // 套装 = cfg.sets 下标；猫饭 = 编号
std::string g_mouseDownRow;      // 鼠标按下的那一行（Selectable 要松开才回 true，先记住是谁）
const int kMaxNameChars = 50;      // 名字最多 50 个 Unicode 字符，中英文一样
char g_renameBuf[kMaxNameChars * 4 + 1] = {}; // UTF-8 每个字符最多 4 字节，另留结尾零
bool g_renameFocus = false;      // 刚打开：这一帧把光标放进输入框
bool g_renameOpen = false;       // 上一帧弹着（用来发现被 B / Esc 关掉）
bool g_renameCloseReq = false;   // 预览用：下一帧关掉
bool g_debugRename = false;      // 预览用：下一帧弹出来
std::string g_renameErr;
bool g_renameCreate = false;      // 新建时直接弹出来起名（空 = 用默认名）
int g_afterRename = 0;            // 起完名去哪个编辑页：1 = 套装，2 = 猫饭
// 排序：按下左摇杆把选中的那一项放进「移动」模式，上下键换位置，再按一次放下
enum MoveKind { kMoveNone, kMoveSet, kMoveFood };
MoveKind g_moveKind = kMoveNone;
int g_moveId = 0;
float g_listScroll = 0;            // 套装/猫饭列表这一帧的滚动位置（测试用）
int g_focusId = 0;                // 放下后把焦点还给刚移动的那一项
int g_reorderDir = 0;             // NewFramePost 消费的快捷排序脉冲，每帧清空
int g_listFocusId = 0;            // 上帧真正聚焦的稳定条目 ID（不是 vector 下标）
ImGuiID g_listFocusNavId = 0;
bool g_reorderBlocked[4] = {};   // 文本/弹窗/录键中按住的键必须先松开
int g_questTarget = -1;     // 给谁选任务：-1 = 全局，>=0 = cfg.sets[i]
char g_search[96] = {};
bool g_debugFocusSearch = false;
bool g_scrollToSel = false; // 进任务页时在 BeginChild 前一次性定位，不用多帧追赶 ScrollMax
bool g_groupOpen[16] = {};
bool g_questInit = true;      // 刚进「换任务」：只展开当前任务所在的那一组
bool g_sideFocusedLast = false;

Page g_questFrom = kHome;   // 从哪一页进的「换任务」（返回时回那里）
const char* const kNone = "—";   // 「不额外选择」：套装编辑页下拉的第一项，以及没选时显示的值
ImVec2 g_panelMin, g_panelMax; // Delete modal is centered on the panel, not the viewport.
bool g_openDelete = false; // One-shot debug request, cleared on open.
bool g_popupLastFrame = false;   // 上一帧有没有开着的弹窗（下拉选项、删除确认）
ImGuiID g_sideCurId = 0;    // 侧栏里当前页那一格（十字键从内容区往左就落到它）
int g_sideFocusIdx = -1;    // 这一帧焦点在侧栏第几格（测试用）
float g_animHc = 0;         // 内容区高度的动画值（0 = 下一帧直接到位）
float g_frameH = 0;         // 这一帧画出来的外框高度（测试用）
// Real letter inputs, never synthetic arrow events (shortcut capture sees physical keys only).
bool g_letterMove[4] = {}; // indexed by ImGuiDir
bool g_letterBlocked[4][2] = {};


// Operation-strip state is independent of NavInputSource (text/clicks need not navigate).
hints::SourceTracker g_hintSource;
std::string g_hintText;
int g_hintRows = 0;
DebugHintGeometry g_hintGeometry;
std::vector<DebugHintRowGeometry> g_hintRowBounds;
DebugHintIconGeometry g_hintIcons;
std::vector<std::string> g_hintItems;
float g_hintSidebarBottom = 0;
float g_hintHeight = 0, g_hintTop = 0, g_hintBottom = 0, g_hintContentBottom = 0;
bool g_hintListRow = false, g_hintListFocused = false, g_hintListSelected = false;
bool g_hintHotkey = false, g_hintHotkeyClear = false;
bool g_hintAction = false, g_hintSlider = false, g_hintTextField = false;
ImGuiID g_numberEdit = 0;
bool g_hintNumber = false;
DebugFoodPreviewGeometry g_foodPreview;
std::string g_foodPreviewText;
ImVec2 g_foodPreviewAnchorMin, g_foodPreviewAnchorMax;
ImVec2 g_foodPreviewOwnerMin, g_foodPreviewOwnerMax;
void QueueFoodPreview(const Config& c, int id, bool fromPopup);
void DrawFoodPreview(const Config& c);

bool HintTarget() {
    if (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) return false;
    return g_hintSource.source == hints::Source::Mouse ? ImGui::IsItemHovered() : ImGui::IsItemFocused();
}
void HintItem() { if (HintTarget()) g_hintAction = true; }
// Call after default/restored focus is applied, not inside Row before it.
void HintListRow(bool selected) {
    if (!HintTarget()) return;
    g_hintListRow = true;
    g_hintListFocused = ImGui::IsItemFocused();
    g_hintListSelected = selected;
    g_hintAction = true;
}
void TrackHintInput(ImGuiContext& g) {
    // Only events actually consumed this frame, in their original order. No repeats,
    // held-button polling, or release events may steal the device from a new input.
    for (const ImGuiInputEvent& e : g.InputEventsTrail) {
        switch (e.Type) {
        case ImGuiInputEventType_Key:
            if (!ImGui::IsMouseKey(e.Key.Key))
                g_hintSource.Key(ImGui::IsGamepadKey(e.Key.Key) ? hints::Source::Gamepad : hints::Source::Keyboard,
                                 (int)e.Key.Key, e.Key.Down, e.Key.AnalogValue);
            break;
        case ImGuiInputEventType_Text: if (e.Text.Char) g_hintSource.Character(); break;
        case ImGuiInputEventType_MouseButton: g_hintSource.MouseButton(e.MouseButton.Down); break;
        case ImGuiInputEventType_MouseWheel: g_hintSource.Wheel(e.MouseWheel.WheelX, e.MouseWheel.WheelY); break;
        case ImGuiInputEventType_MousePos: g_hintSource.MousePosition(e.MousePos.PosX, e.MousePos.PosY); break;
        default: break;
        }
    }
}

// 按 B / 点返回箭头回到哪一页（主页面返回自己 = 没有上一级）
Page Back(Page p)
{
    switch (p) {
    case kQuests: return g_questFrom;
    case kSetEdit: return kSets;
    case kFoodEdit: return kFood;
    default: return p;
    }
}

// 侧栏上高亮哪一页
Page MainOf(Page p)
{
    switch (p) {
    case kQuests: return g_questFrom == kSetEdit ? kSets : kHome;
    case kSetEdit: return kSets;
    case kFoodEdit: return kFood;
    default: return p;
    }
}

// 主页显示的、重开时真正会进的任务：当前套装指定了任务就用套装的（跟 inq::ApplyOptions 同一口径）
int EffectiveQuest(const Config& c)
{
    if (!c.enabled) return 0;   // 卸载态：不预先接任务
    if (const LoadoutSet* s = c.FindSetById(c.activeSet))
        if (gamedata::ValidQuestId(s->quest)) return s->quest;
    return c.questId;
}

void Go(Page p)
{
    // 换页就退出移动模式：不然在移动中按 B 回到上一页，再进来还停在「移动中」，
    // 而且 L3 会被当成「放下」，再也进不去移动（用户报过这个）
    g_moveKind = kMoveNone;
    g_moveId = 0;
    g_reorderDir = g_listFocusId = 0;
    g_listFocusNavId = 0;
    g_numberEdit = false;
    g_page = p;
    g_focus = 2;
    if (p == kQuests) { g_search[0] = 0; g_scrollToSel = true; g_questInit = true; }
}

bool NavVisible() { return GImGui->NavCursorVisible; }

// 刚画完的这个控件拿默认焦点（进页后头两帧）。
// 不用 SetKeyboardFocusHere：它按 Tab 键的逻辑找「下一个可输入的控件」，会跳过按钮，
// 落到后面的滑条上还会把滑条切成输入框。
void FocusDefault()
{
    if (g_focus <= 0) return;
    --g_focus;
    g_focusUsed = true;
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisible(true);
    // ImGui 上下移动时会保持一个横向位置，没有就靠最左。进页时定成默认项的中线：
    // 从通栏的「重开」往下落到正下方中间那个，而不是最左边
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const float cx = GImGui->LastItemData.NavRect.GetCenter().x - w->DC.CursorStartPos.x;
    w->RootWindowForNav->NavPreferredScoringPosRel[0] = ImVec2(cx, FLT_MAX);
}

unsigned long long g_dbgRadioId = 0; int g_dbgRadioFocused = 0; unsigned long long g_dbgNavId = 0; unsigned long long g_dbgBackId = 0; unsigned long long g_dbgRadio0Id = 0;
// 把焦点直接放到刚画完的那个控件上（调试/测试用，绕开 FocusDefault 的进页计数）
void FocusItem()
{
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisible(true);
}

void Text(ImFont* f, float size, ImU32 col, const char* s, bool wrap = false)
{
    ImGui::PushFont(f, size);
    ImGui::PushStyleColor(ImGuiCol_Text, V(col));
    if (wrap) ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(s);
    if (wrap) ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

ImVec2 TextSize(ImFont* f, float size, const char* s) { return f->CalcTextSizeA(size, FLT_MAX, 0.0f, s); }

void CenterText(ImDrawList* dl, ImFont* f, float size, ImVec2 center, ImU32 col, const char* s)
{
    const ImVec2 ts = TextSize(f, size, s);
    dl->AddText(f, size, ImVec2(std::floor(center.x - ts.x * 0.5f), std::floor(center.y - ts.y * 0.5f)), col, s);
}

// 不画 ImGui 默认的方形焦点框，自定义控件自己画
void BeginCustom() { ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0)); }
void EndCustom() { ImGui::PopStyleColor(); }
bool Focused() { return ImGui::IsItemFocused() && NavVisible(); }

// ---------------------------------------------------------------------------
//  线条图标（在 c 为中心、半边长 r 的方块里画）
// ---------------------------------------------------------------------------
enum Icon { kIcRefresh, kIcSearch, kIcShield, kIcBowl, kIcFlag, kIcGear, kIcBack, kIcPlus };

void DrawIcon(ImDrawList* dl, Icon ic, ImVec2 c, float r, ImU32 col)
{
    const float t = std::fmax(1.6f, r * 0.16f);
    auto P = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
    switch (ic) {
    case kIcRefresh: {
        dl->PathArcTo(c, r * 0.72f, -1.25f, 4.15f, 28);
        dl->PathStroke(col, 0, t);
        const ImVec2 tip = ImVec2(c.x + std::cos(-1.25f) * r * 0.72f, c.y + std::sin(-1.25f) * r * 0.72f);
        dl->AddTriangleFilled(ImVec2(tip.x - r * 0.05f, tip.y - r * 0.30f), ImVec2(tip.x + r * 0.33f, tip.y + r * 0.02f),
                              ImVec2(tip.x - r * 0.12f, tip.y + r * 0.26f), col);
        break;
    }
    case kIcSearch:
        dl->AddCircle(P(-0.15f, -0.15f), r * 0.52f, col, 24, t);
        dl->AddLine(P(0.24f, 0.24f), P(0.72f, 0.72f), col, t * 1.2f);
        break;
    case kIcShield: {
        const ImVec2 pts[] = {P(-0.62f, -0.70f), P(0.0f, -0.86f), P(0.62f, -0.70f), P(0.62f, 0.02f),
                              P(0.0f, 0.82f), P(-0.62f, 0.02f)};
        dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, t);
        break;
    }
    case kIcBowl:
        dl->AddLine(P(-0.80f, -0.05f), P(0.80f, -0.05f), col, t);
        dl->PathArcTo(P(0.0f, -0.05f), r * 0.80f, 0.0f, 3.14159f, 20);
        dl->PathStroke(col, 0, t);
        dl->AddLine(P(-0.30f, 0.84f), P(0.30f, 0.84f), col, t);
        dl->AddLine(P(-0.25f, -0.35f), P(-0.25f, -0.75f), col, t * 0.8f);
        dl->AddLine(P(0.15f, -0.35f), P(0.15f, -0.80f), col, t * 0.8f);
        break;
    case kIcFlag: {
        dl->AddLine(P(-0.55f, -0.80f), P(-0.55f, 0.85f), col, t);
        const ImVec2 pts[] = {P(-0.55f, -0.78f), P(0.65f, -0.50f), P(-0.55f, -0.12f)};
        dl->AddPolyline(pts, 3, col, ImDrawFlags_Closed, t);
        break;
    }
    case kIcGear:
        dl->AddCircle(c, r * 0.48f, col, 24, t);
        dl->AddCircle(c, r * 0.16f, col, 12, t);
        for (int i = 0; i < 8; ++i) {
            const float a = i * 3.14159f / 4.0f;
            dl->AddLine(ImVec2(c.x + std::cos(a) * r * 0.50f, c.y + std::sin(a) * r * 0.50f),
                        ImVec2(c.x + std::cos(a) * r * 0.80f, c.y + std::sin(a) * r * 0.80f), col, t * 1.3f);
        }
        break;
    case kIcBack:
        dl->AddLine(P(0.25f, -0.55f), P(-0.25f, 0.0f), col, t);
        dl->AddLine(P(-0.25f, 0.0f), P(0.25f, 0.55f), col, t);
        break;
    case kIcPlus:
        dl->AddLine(P(-0.6f, 0.0f), P(0.6f, 0.0f), col, t);
        dl->AddLine(P(0.0f, -0.6f), P(0.0f, 0.6f), col, t);
        break;
    }
}

// ---------------------------------------------------------------------------
//  控件
// ---------------------------------------------------------------------------
// 胶囊按钮。一页最多一个主按钮（金色实心），其余是淡底金字；删除这类是不带底的淡红字。
enum BtnStyle { kPrimary, kSecondary, kDanger, kDangerSolid };

bool PillButton(const char* id, const char* label, float w, float h, BtnStyle style, Icon icon = (Icon)-1)
{
    BeginCustom();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    EndCustom();
    HintItem();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rr = h * 0.5f;
    if (style == kPrimary) dl->AddRectFilled(a, b, act ? kGoldDown : (hov ? kGoldHot : kGold), rr);
    else if (style == kSecondary) dl->AddRectFilled(a, b, (hov || act) ? kSoftHot : kSoft, rr);
    else if (style == kDangerSolid)
        dl->AddRectFilled(a, b, act ? IM_COL32(186, 80, 60, 255) : (hov ? IM_COL32(226, 112, 90, 255) : IM_COL32(210, 96, 74, 255)), rr);
    else if (hov || act) dl->AddRectFilled(a, b, IM_COL32(232, 140, 110, 22), rr);
    if (Focused()) dl->AddRect(a - ImVec2(4, 4) * S, b + ImVec2(4, 4) * S, kGold, rr + 4 * S, 0, 2.0f * S);
    const float fs = style == kPrimary ? szBtn : szBody;
    ImFont* font = style == kDanger ? fReg : fBold;
    const ImVec2 ts = TextSize(font, fs, label);
    const float iconW = (int)icon >= 0 ? fs * 1.15f : 0.0f;
    const float x0 = (a.x + b.x - ts.x - iconW) * 0.5f;
    const float cy = (a.y + b.y) * 0.5f;
    const ImU32 col = style == kPrimary ? kInk : style == kDanger ? kWarn : style == kDangerSolid ? IM_COL32(255, 247, 238, 255) : kGold;
    if ((int)icon >= 0) DrawIcon(dl, icon, ImVec2(x0 + fs * 0.42f, cy), fs * 0.48f, col);
    dl->AddText(font, fs, ImVec2(std::floor(x0 + iconW), std::floor(cy - ts.y * 0.5f)), col, label);
    return pressed;
}

// 一排芯片单选（猫饭的档位）：手柄左右移动、A 选中，比拖滑条顺手
bool SegRow(const char* id, const char* label, int* v, int count, const char* const* labels, bool takeFocus)
{
    ImGui::PushID(id);
    const float w = ImGui::GetContentRegionAvail().x;
    const float rowH = 48 * S, h = 36 * S, gap = 6 * S;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(fReg, szBody, ImVec2(p.x + 14 * S, p.y + (rowH - szBody) * 0.5f), kMuted, label);
    const float x0 = p.x + w * 0.30f;
    const float segW = (w * 0.70f - 6 * S - gap * (count - 1)) / count;
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(x0 + i * (segW + gap), p.y + (rowH - h) * 0.5f));
        ImGui::PushID(i);
        BeginCustom();
        if (ImGui::InvisibleButton("##seg", ImVec2(segW, h), ImGuiButtonFlags_EnableNav)) { *v = i; changed = true; }
        EndCustom();
    HintItem();
        if (takeFocus && i == *v) FocusDefault();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const bool on = i == *v, hov = ImGui::IsItemHovered();
        dl->AddRectFilled(a, b, on ? kGold : (hov ? kSoftHot : kSoft), h * 0.5f);
        if (Focused()) dl->AddRect(a - ImVec2(3, 3) * S, b + ImVec2(3, 3) * S, kGold, h * 0.5f + 3 * S, 0, 2.0f * S);
        CenterText(dl, on ? fBold : fReg, szSmall, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), on ? kInk : kText, labels[i]);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
    return changed;
}

// 圆形图标按钮 + 下方短标签（主页那一排）
bool RoundButton(const char* id, Icon icon, const char* label, float colW)
{
    const float d = 54 * S;
    const float h = d + 10 * S + szSmall + 4 * S;
    BeginCustom();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(colW, h), ImGuiButtonFlags_EnableNav);
    EndCustom();
    HintItem();
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 c(a.x + colW * 0.5f, a.y + d * 0.5f);
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(c, d * 0.5f, (hov || act) ? kSoftHot : kSoft, 40);
    if (Focused()) dl->AddCircle(c, d * 0.5f + 4 * S, kGold, 40, 2.0f * S);
    DrawIcon(dl, icon, c, d * 0.26f, kGold);
    CenterText(dl, fReg, szSmall, ImVec2(c.x, a.y + d + 10 * S + szSmall * 0.5f), Focused() ? kText : kMuted, label);
    return pressed;
}

// 普通十字键滚动只交给 ImGui：NewFrame 解析导航结果，Begin 在绘制前应用滚动。
// 不在行绘制后再 SetScrollY 预留整行，否则会在下一帧补滚，形成两段跳动。

// 整行可点的列表项：左边主文字，右边次要文字
bool Row(const char* id, const char* left, const char* right, bool selected, float h = 0, bool quiet = false, const char* subtitle = nullptr,
         const char* mark = nullptr)
{
    if (h <= 0) h = 46 * S;
    ImGui::PushID(id);
    // quiet：不画 ImGui 自己的导航框（移动模式里我们自己描金边，两层框叠着难看）
    if (quiet) ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));
    const bool pressed = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_None, ImVec2(0, h));
    if (quiet) ImGui::PopStyleColor();
    HintItem();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float rightW = 0;
    if (right && *right) {
        const ImVec2 ts = TextSize(fReg, szSmall, right);
        rightW = ts.x + 16 * S;
        dl->AddText(fReg, szSmall, ImVec2(b.x - 14 * S - ts.x, std::floor(a.y + (h - ts.y) * 0.5f)), kMuted, right);
    }
    dl->PushClipRect(a, ImVec2(b.x - rightW - 8 * S, b.y), true);
    float x = a.x + 14 * S;
    if (selected) {
        dl->AddCircleFilled(ImVec2(x + 3 * S, (a.y + b.y) * 0.5f), 3.5f * S, kGold);
        x += 14 * S;
    }
    const float markW = (mark && *mark) ? TextSize(fReg, szSmall, mark).x + 5 * S : 0.0f;
    const float avail = std::fmax(1.0f, b.x - rightW - 8 * S - x - markW);
    const ImVec2 full = TextSize(fReg, szBody, left);
    // 猫饭三技能放主行，名称/属性放次行；按实际宽度缩小主行，不能让第三技能被名称挤掉。
    const float textSize = subtitle && full.x > avail ? szBody * avail / full.x : szBody;
    const ImVec2 ts = TextSize(fReg, textSize, left);
    const float textY = subtitle ? a.y + 5 * S : a.y + (h - ts.y) * 0.5f;
    if (markW > 0) dl->AddText(fReg, szSmall, ImVec2(x, std::floor(textY + (textSize - szSmall) * 0.5f)), kBad, mark);
    x += markW;
    dl->AddText(fReg, textSize, ImVec2(x, std::floor(textY)), selected ? kGold : kText, left);
    if (subtitle) dl->AddText(fReg, szSmall, ImVec2(x, std::floor(a.y + h - szSmall - 5 * S)), kMuted, subtitle);
    dl->PopClipRect();
    ImGui::PopID();
    return pressed;
}

// 开关行：左边文字（下面可带一行小字说明），右边开关
bool ToggleRow(const char* id, const char* label, const char* note, bool* v)
{
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = (note && *note) ? 58 * S : 46 * S;
    BeginCustom();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    EndCustom();
    HintItem();
    if (pressed) *v = !*v;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered() || Focused()) dl->AddRectFilled(a, b, IM_COL32(237, 230, 214, 12), 12 * S);
    if (Focused()) dl->AddRect(a, b, kGold, 12 * S, 0, 1.5f * S);
    const float ty = (note && *note) ? a.y + 8 * S : a.y + (h - szBody) * 0.5f;
    dl->AddText(fReg, szBody, ImVec2(a.x + 14 * S, ty), kText, label);
    if (note && *note) dl->AddText(fReg, szSmall, ImVec2(a.x + 14 * S, ty + szBody + 4 * S), kMuted, note);
    const float sw = 46 * S, sh = 26 * S;
    const ImVec2 sa(b.x - 14 * S - sw, (a.y + b.y - sh) * 0.5f);
    dl->AddRectFilled(sa, sa + ImVec2(sw, sh), *v ? kGold : IM_COL32(237, 230, 214, 46), sh * 0.5f);
    const float kx = *v ? sa.x + sw - sh * 0.5f : sa.x + sh * 0.5f;
    dl->AddCircleFilled(ImVec2(kx, sa.y + sh * 0.5f), sh * 0.5f - 3 * S, *v ? kInk : kText, 24);
    return pressed;
}

// 单选行
bool RadioRow(const char* id, const char* label, const char* note, bool on)
{
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = (note && *note) ? 58 * S : 46 * S;
    BeginCustom();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    EndCustom();
    HintItem();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered() || Focused()) dl->AddRectFilled(a, b, IM_COL32(237, 230, 214, 12), 12 * S);
    if (Focused()) dl->AddRect(a, b, kGold, 12 * S, 0, 1.5f * S);
    const ImVec2 c(a.x + 24 * S, (a.y + b.y) * 0.5f);
    dl->AddCircle(c, 9 * S, on ? kGold : kMuted, 24, 1.8f * S);
    if (on) dl->AddCircleFilled(c, 5 * S, kGold, 20);
    const float tx = a.x + 44 * S;
    const float ty = (note && *note) ? a.y + 8 * S : a.y + (h - szBody) * 0.5f;
    dl->AddText(fReg, szBody, ImVec2(tx, ty), on ? kTitle : kText, label);
    if (note && *note) dl->AddText(fReg, szSmall, ImVec2(tx, ty + szBody + 4 * S), kMuted, note);
    if (!g_dbgRadio0Id) g_dbgRadio0Id = ImGui::GetItemID();
    g_dbgRadioId = ImGui::GetItemID();
    g_dbgRadioFocused = ImGui::IsItemFocused();
    g_dbgNavId = GImGui->NavId;
    return pressed;
}

// 「名称 …… 值 ›」的行，按下打开选择弹窗
bool ValueRow(const char* id, const char* label, const char* value)
{
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 46 * S;
    BeginCustom();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    EndCustom();
    HintItem();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered() || Focused()) dl->AddRectFilled(a, b, IM_COL32(237, 230, 214, 12), 12 * S);
    if (Focused()) dl->AddRect(a, b, kGold, 12 * S, 0, 1.5f * S);
    dl->AddText(fReg, szBody, ImVec2(a.x + 14 * S, a.y + (h - szBody) * 0.5f), kMuted, label);
    const ImVec2 ts = TextSize(fReg, szBody, value);
    dl->AddText(fReg, szBody, ImVec2(b.x - 34 * S - ts.x, a.y + (h - szBody) * 0.5f), kText, value);
    DrawIcon(dl, kIcBack, ImVec2(b.x - 18 * S, (a.y + b.y) * 0.5f), -7 * S, kMuted);   // r<0 = 镜像成 ›
    return pressed;
}

// One navigation stop. A/Enter enters adjustment; B/Esc leaves it without
// leaving the page. Claim directions while editing so native nav cannot escape.
bool NumberRow(const char* id, const char* label, int& value, int maximum)
{
    const float w = ImGui::GetContentRegionAvail().x, h = 58 * S;
    const ImGuiID item = ImGui::GetID(id);
    BeginCustom();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    EndCustom();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    bool editing = g_numberEdit == item;
    if (pressed) {
        g_numberEdit = editing ? 0 : item;
        editing = g_numberEdit == item;
        if (editing) { ImGui::SetActiveID(item, ImGui::GetCurrentWindow()); ImGui::SetFocusID(item, ImGui::GetCurrentWindow()); }
        else ImGui::ClearActiveID();
    }
    int delta = 0;
    if (editing) {
        ImGui::KeepAliveID(item);
        GImGui->ActiveIdUsingNavDirMask = (1 << ImGuiDir_COUNT) - 1;
        auto key = [&](ImGuiKey pad, ImGuiKey arrow, ImGuiDir dir) {
            return ImGui::IsKeyPressed(pad, true) || ImGui::IsKeyPressed(arrow, true) || g_letterMove[dir];
        };
        if (key(ImGuiKey_GamepadDpadUp, ImGuiKey_UpArrow, ImGuiDir_Up)) ++delta;
        if (key(ImGuiKey_GamepadDpadDown, ImGuiKey_DownArrow, ImGuiDir_Down)) --delta;
        if (key(ImGuiKey_GamepadDpadRight, ImGuiKey_RightArrow, ImGuiDir_Right)) delta += 10;
        if (key(ImGuiKey_GamepadDpadLeft, ImGuiKey_LeftArrow, ImGuiDir_Left)) delta -= 10;
        ImGui::NavMoveRequestCancel();
    }
    // Mouse chevrons remain direct +/-1 controls, with no separate nav stops.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (mouse.x > b.x - 42 * S) delta += mouse.y < (a.y + b.y) * .5f ? 1 : -1;
    }
    const int before = value;
    if (delta) value = (std::max)(0, (std::min)(maximum, value + delta));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered || Focused() || editing) dl->AddRectFilled(a, b, IM_COL32(237, 230, 214, 12), 12 * S);
    if (Focused() || editing) dl->AddRect(a, b, kGold, 12 * S, 0, (editing ? 2.0f : 1.5f) * S);
    dl->AddText(fReg, szBody, ImVec2(a.x + 14 * S, a.y + (h - szBody) * .5f), kMuted, label);
    const std::string text = value ? std::to_string(value) : i18n::Tr("不改", "Unchanged", "変更なし");
    const ImVec2 ts = TextSize(fReg, szBody, text.c_str());
    dl->AddText(fReg, szBody, ImVec2(b.x - 50 * S - ts.x, a.y + (h - szBody) * .5f), kText, text.c_str());
    const float x = b.x - 22 * S, cy = (a.y + b.y) * .5f;
    for (int sign : {-1, 1}) {
        const float y = cy + sign * 13 * S;
        const ImVec2 points[] = {ImVec2(x - 6 * S, y - sign * 3 * S), ImVec2(x, y + sign * 3 * S), ImVec2(x + 6 * S, y - sign * 3 * S)};
        dl->AddPolyline(points, 3, editing ? kGold : kMuted, 0, 2 * S);
    }
    if (HintTarget() || editing) g_hintNumber = true;
    return value != before;
}

// 页标题。子页带返回箭头：箭头加标题整块可以用鼠标点（手柄用 B，所以不进十字键导航）
void PageHeader(const char* title, const char* sub, bool back)
{
    if (back) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 ts = TextSize(fBold, szPage, title);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        BeginCustom();
        if (ImGui::InvisibleButton("##back", ImVec2(ts.x + 34 * S, ts.y))) Go(Back(g_page));
        g_dbgBackId = ImGui::GetItemID();
        EndCustom();
    HintItem();
        ImGui::PopItemFlag();
        const bool hov = ImGui::IsItemHovered();
        DrawIcon(dl, kIcBack, ImVec2(p.x + 6 * S, p.y + ts.y * 0.5f), 9 * S, hov ? kGold : kMuted);
        dl->AddText(fBold, szPage, ImVec2(p.x + 24 * S, p.y), hov ? kGold : kTitle, title);
    } else {
        Text(fBold, szPage, kTitle, title);
    }
    if (sub && *sub) Text(fReg, szSmall, kMuted, sub);
    ImGui::Dummy(ImVec2(0, 10 * S));
}

// 焦点项被滚动区挡住时把它拉回视野。用于各种单列清单：不依赖 ImGui 的
// 自动滚动（横向导航把焦点挪走后它就停了），按当前裁剪区实际边界调整。
void FollowFocusedRow()
{
    // 只在“方向导航正在移动焦点”时跟随。鼠标滚轮浏览时不能把焦点项拽回来。
    ImGuiContext& g = *GImGui;
    if (!ImGui::IsItemFocused() || !(g.NavMoveSubmitted || g.NavMoveForwardToNextFrame)) return;
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    if (b.y > w->ClipRect.Max.y) ImGui::SetScrollY(ImGui::GetScrollY() + (b.y - w->ClipRect.Max.y));
    else if (a.y < w->ClipRect.Min.y) ImGui::SetScrollY(ImGui::GetScrollY() - (w->ClipRect.Min.y - a.y));
}

// 选择弹窗：返回选中的下标（没选返回 -1）
int PickPopup(const char* id, const std::vector<std::string>& items, int current,
              const Config* previewConfig = nullptr, const std::vector<int>* previewIds = nullptr)
{
    int picked = -1;
    ImGui::SetNextWindowSizeConstraints(ImVec2(320 * S, 0), ImVec2(420 * S, 460 * S));
    if (ImGui::BeginPopup(id)) {
        ImGuiWindow* popupWindow = ImGui::GetCurrentWindow();
        ImGuiStorage* pickStore = ImGui::GetStateStorage();
        const ImGuiID pickKey = ImGui::GetID("#pickFocus");
        ImGuiID s_pickFocus = (ImGuiID)(uintptr_t)pickStore->GetVoidPtr(pickKey);
        if (previewConfig) { g_foodPreview = {}; g_foodPreviewText.clear(); }
        // Hover wins over the existing nav focus when inspecting candidates.
        int previewIndex = -1;
        ImVec2 previewMin, previewMax;
        bool previewHovered = false;
        for (int i = 0; i < (int)items.size(); ++i) {
            ImGui::PushID(i);
            if (Row("it", items[i].c_str(), nullptr, i == current, 40 * S)) { picked = i; ImGui::CloseCurrentPopup(); }
            if (ImGui::IsItemFocused()) {
                s_pickFocus = ImGui::GetItemID();
                pickStore->SetVoidPtr(pickKey, (void*)(uintptr_t)s_pickFocus);
            }
            FollowFocusedRow();
            if (i == current && ImGui::IsWindowAppearing()) {
                // SetFocusID alone leaves the popup's queued NavInitResult for
                // the first item alive; native next-frame init would replace
                // the selected candidate and make Down act from the wrong row.
                ImGui::SetItemDefaultFocus();
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetScrollHereY(0.4f);
            }
            if (previewConfig && previewIds && i < (int)previewIds->size()) {
                const bool hovered = ImGui::IsItemHovered();
                if (hovered || (!previewHovered && ImGui::IsItemFocused())) {
                    previewIndex = i;
                    previewHovered = hovered;
                    previewMin = ImGui::GetItemRectMin(); previewMax = ImGui::GetItemRectMax();
                }
            }
            ImGui::PopID();
        }
        if (previewIndex >= 0 && picked < 0) {
            g_foodPreviewAnchorMin = previewMin; g_foodPreviewAnchorMax = previewMax;
            QueueFoodPreview(*previewConfig, (*previewIds)[previewIndex], true);
        }
        // 横向导航可能把焦点丢到弹窗外；丢出去后上下就不再滚动这个清单。
        // 每一帧都把焦点锁回清单里最后选中的那一项。
        if (s_pickFocus && (GImGui->NavWindow != popupWindow || GImGui->NavId == 0) &&
            (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ||
             ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false))) {
            ImGui::FocusWindow(popupWindow);
            ImGui::SetFocusID(s_pickFocus, popupWindow);
            ImGui::NavMoveRequestCancel();
            ImGui::SetNavCursorVisible(true);
        }
        ImGui::EndPopup();
    }
    return picked;
}

std::string QuestLabel(int id, const char* zeroText)
{
    if (id == 0) return zeroText;
    const QuestInfo* q = gamedata::FindQuest(id);
    if (q) return gamedata::QuestName(*q);
    return i18n::Fmt(i18n::Tr("任务 %d", "Quest %d", "クエスト %d"), id);
}

// 猫饭加成 / 属性耐性的档位（0~3）。中文和日文都是「无小中大」，英文用游戏里的 S/M/L。
const char* LevelName(int v)
{
    static const char* const zh[] = {"无", "小", "中", "大"};
    static const char* const en[] = {"none", "S", "M", "L"};
    static const char* const ja[] = {"なし", "小", "中", "大"};
    if (v < 0) v = 0;
    if (v > 3) v = 3;
    return i18n::Tr(zh[v], en[v], ja[v]);
}

std::string FoodSummary(const FoodPreset& f)
{
    const int stam = canteen::Neutral(f.hp, f.stamina, f.attack, f.defense, f.elemRes, f.skill) ? 0 : 50;
    std::string s = i18n::Fmt(i18n::Tr("体力+%d 耐力+%d", "HP+%d Stam+%d", "体力+%d スタミナ+%d"),
                              f.hp * 10, stam);   // 耐力食堂固定 +50，空饭才写 0
    if (f.attack > 0 && f.attack <= 3) s += std::string(i18n::Tr(" 攻击", " ATK ", " 攻撃")) + LevelName(f.attack);
    if (f.defense > 0 && f.defense <= 3) s += std::string(i18n::Tr(" 防御", " DEF ", " 防御")) + LevelName(f.defense);
    if (f.elemRes > 0 && f.elemRes <= 3) s += std::string(i18n::Tr(" 耐性", " RES ", " 耐性")) + LevelName(f.elemRes);
    return s;
}

std::string FoodSkillsSummary(const FoodPreset& f)
{
    std::string s;
    for (int id : f.skill) {
        if (!s.empty()) s += " · ";
        s += gamedata::FoodSkillShortNameOf(id);
    }
    return s;
}

// 猫饭预设的「合法性」：食堂能不能照着做出来。纯推算，不读游戏内存也不改任何东西。
canteen::Result FoodLegality(const FoodPreset& f)
{
    if (canteen::Neutral(f.hp, f.stamina, f.attack, f.defense, f.elemRes, f.skill)) { canteen::Result ok; ok.verdict = canteen::Verdict::Legal; return ok; }
    return canteen::Check(f.hp, 2, f.attack, f.defense, f.elemRes, f.skill);   // 吃了必是耐力 +50
}

std::string FoodLegalityText(const canteen::Result& r)
{
    switch (r.reason) {
    case canteen::Reason::Range:
        return i18n::Tr("数值超出食堂能给的范围", "values outside the canteen's range",
                        "食堂で出せる範囲外の数値");
    case canteen::Reason::Duplicate:
        return i18n::Tr("同一个技能选了两遍", "the same skill is chosen twice", "同じスキルが重複している");
    case canteen::Reason::SameGroup:
        return i18n::Tr("同一组技能只能出一个", "only one skill per group", "同じ組のスキルは 1 つまで");
    case canteen::Reason::Daily:
        if (r.dailyCount > 1)
            return i18n::Fmt(i18n::Tr("要靠 %d 个每日技能，只有当天正好有才合法",
                                      "needs %d daily skills; legal only that day",
                                      "毎日スキル %d 個賴み。その日だけ合法"),
                             r.dailyCount);
        return i18n::Fmt(i18n::Tr("要靠每日技能「%s」，只有当天有它才合法",
                                  "needs daily skill \"%s\"; legal only that day",
                                  "毎日スキル「%s」頼み。その日だけ合法"),
                         gamedata::FoodSkillNameOf(r.dailySkill));
    case canteen::Reason::Stamina:
        return i18n::Tr("食堂固定给耐力 +50", "the canteen always gives stamina +50",
                        "食堂のスタミナは常に +50");
    case canteen::Reason::Exclusive:
        return i18n::Fmt(i18n::Tr("「%s」和「%s」不能同时出现",
                                  "\"%s\" and \"%s\" never come together",
                                  "「%s」と「%s」は同時に取れない"),
                         gamedata::FoodSkillNameOf(50), gamedata::FoodSkillNameOf(43));
    case canteen::Reason::BuffCrowd:
        return i18n::Tr("数值和技能的食材要求冲突，凑不到同一桌",
                        "buffs and skills cannot share ingredients",
                        "強化とスキルの食材が両立しない");
    case canteen::Reason::TooMany:
        if (r.needed > 6)
            return i18n::Fmt(i18n::Tr("食堂做不出：至少要 %d 个食材，一餐只有 6 个",
                                      "needs %d ingredients; a meal has 6",
                                      "再現不可：食材が最低 %d 個必要（1 食 6 個まで）"),
                             r.needed);
        return i18n::Tr("食堂做不出这份组合", "the canteen cannot make this combination",
                        "この組み合わせは食堂で再現できない");
    default:
        break;
    }
    return std::string();   // 合法（含「靠每日技能」）不显示任何东西
}

// 猫饭在界面上的名字：起了名字就用名字，没起就显示序号
std::string FoodLabel(const Config& c, int id)
{
    auto it = c.foods.find(id);
    if (it == c.foods.end())
        return i18n::Fmt(i18n::Tr("猫饭 %d（缺失）", "Meal %d (missing)", "猫飯 %d（見つかりません）"), id);
    if (!it->second.name.empty())
        return it->second.deleted ? it->second.name + i18n::Tr("（已删除）", " (deleted)", "（削除済み）")
                                  : it->second.name;
    if (it != c.foods.end() && it->second.deleted)
        return i18n::Fmt(i18n::Tr("猫饭 %d（已删除）", "Meal %d (deleted)", "猫飯 %d（削除済み）"), id);
    return i18n::Fmt(i18n::Tr("猫饭 %d", "Meal %d", "猫飯 %d"), id);
}

// 「现在实际会吃哪一份」：套用中的套装自己指定了猫饭就用它的，否则用全局那份。
// 列表的选中、标题文案、编辑页的说明都按它算，跟 inq::ApplyOptions 的口径一致。
int EffFoodId(const Config& c)
{
    if (!c.enabled) return -1;   // 卸载态：不套猫饭预设，不干预游戏手动猫饭
    if (const LoadoutSet* act = c.FindSetById(c.activeSet)) {
        // 墓碑也算：内容还在，inq::ApplyOptions 照样会吃它，界面对齐同一个口径
        if (act->food > 0 && c.foods.count(act->food)) return act->food;
    }
    return c.food;
}

// 这份猫饭还在不在列表里（删掉的只留编号墓碑，列表里看不到、也不能再编辑）
bool FoodAlive(const Config& c, int id)
{
    auto it = c.foods.find(id);
    return it != c.foods.end() && !it->second.deleted;
}

// 这个编号还吃不吃得到东西：墓碑也吃得到（内容留着），只是列表里不再出现
bool FoodUsable(const Config& c, int id) { return id > 0 && c.foods.count(id) != 0; }

void QueueFoodPreview(const Config& c, int id, bool fromPopup)
{
    // No selected override means the global meal; missing IDs have no invented data.
    if (id <= 0) id = c.food;
    if (id <= 0 || c.foods.find(id) == c.foods.end()) return;
    if (!fromPopup) {
        g_foodPreviewAnchorMin = ImGui::GetItemRectMin();
        g_foodPreviewAnchorMax = ImGui::GetItemRectMax();
    }
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    g_foodPreviewOwnerMin = fromPopup ? w->Pos : g_panelMin;
    g_foodPreviewOwnerMax = fromPopup ? w->Pos + w->Size : g_panelMax;
    g_foodPreview.foodId = id;
    g_foodPreview.fromPopup = fromPopup;
    g_foodPreview.visible = true;
}

void DrawFoodPreview(const Config& c)
{
    if (!g_foodPreview.visible || g_page != kSetEdit) { g_foodPreview = {}; g_foodPreviewText.clear(); return; }
    const auto it = c.foods.find(g_foodPreview.foodId);
    if (it == c.foods.end()) { g_foodPreview = {}; return; }
    const std::string title = FoodLabel(c, it->first);
    const std::string summary = FoodSummary(it->second);
    const std::string skills = FoodSkillsSummary(it->second);
    const bool bad = FoodLegality(it->second).verdict == canteen::Verdict::Impossible;
    g_foodPreviewText = title + "\n" + summary + "\n" + skills;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 screenMin = vp->Pos + ImVec2(8 * S, 8 * S);
    const ImVec2 screenMax = vp->Pos + vp->Size - ImVec2(8 * S, 8 * S);
    const float pad = 12 * S, gap = 5 * S, size = 14 * S;
    const float width = std::fmin(282 * S, screenMax.x - screenMin.x);
    const float wrap = std::fmax(1.0f, width - pad * 2);
    const ImVec2 titleSize = fBold->CalcTextSizeA(size, FLT_MAX, wrap, title.c_str());
    const ImVec2 summarySize = fReg->CalcTextSizeA(size, FLT_MAX, wrap, summary.c_str());
    const ImVec2 skillSize = fReg->CalcTextSizeA(size, FLT_MAX, wrap, skills.c_str());
    const float height = pad * 2 + titleSize.y + summarySize.y + skillSize.y + gap * 2;
    float x = g_foodPreviewOwnerMax.x + 10 * S;
    if (x + width > screenMax.x) x = g_foodPreviewOwnerMin.x - 10 * S - width;
    x = std::fmax(screenMin.x, std::fmin(x, screenMax.x - width));
    float y = g_foodPreviewAnchorMin.y;
    y = std::fmax(screenMin.y, std::fmin(y, screenMax.y - height));
    // Very narrow viewports may have no exterior side space: prefer above/below
    // the anchor instead of covering the currently inspected control.
    if (x < g_foodPreviewAnchorMax.x && x + width > g_foodPreviewAnchorMin.x &&
        y < g_foodPreviewAnchorMax.y && y + height > g_foodPreviewAnchorMin.y) {
        if (g_foodPreviewAnchorMin.y - height - 8 * S >= screenMin.y)
            y = g_foodPreviewAnchorMin.y - height - 8 * S;
        else y = std::fmin(screenMax.y - height, g_foodPreviewAnchorMax.y + 8 * S);
    }
    const ImVec2 a(x, y), b(x + width, y + height);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(a, b, IM_COL32(29, 24, 17, 248), 10 * S);
    dl->AddRect(a, b, IM_COL32(226, 194, 122, 190), 10 * S, 0, S);
    ImVec2 p = a + ImVec2(pad, pad);
    dl->AddText(fBold, size, p, kGold, title.c_str(), nullptr, wrap);
    p.y += titleSize.y + gap;
    dl->AddText(fReg, size, p, kText, summary.c_str(), nullptr, wrap);
    p.y += summarySize.y + gap;
    if (bad) {
        const float markW = fReg->CalcTextSizeA(size, FLT_MAX, 0.0f, "!").x + 4 * S;
        dl->AddText(fReg, size, p, kBad, "!");
        dl->AddText(fReg, size, p + ImVec2(markW, 0), kText, skills.c_str(), nullptr, wrap - markW);
    } else dl->AddText(fReg, size, p, kText, skills.c_str(), nullptr, wrap);
    g_foodPreview.left = a.x; g_foodPreview.right = b.x;
    g_foodPreview.top = a.y; g_foodPreview.bottom = b.y;
    g_foodPreview.anchorLeft = g_foodPreviewAnchorMin.x; g_foodPreview.anchorRight = g_foodPreviewAnchorMax.x;
    g_foodPreview.anchorTop = g_foodPreviewAnchorMin.y; g_foodPreview.anchorBottom = g_foodPreviewAnchorMax.y;
}

// "123" → 123
bool ParseDigits(const std::string& s, int& v)
{
    if (s.empty() || s.size() > 9) return false;
    v = 0;
    for (char ch : s) {
        if (ch < '0' || ch > '9') return false;
        v = v * 10 + (ch - '0');
    }
    return true;
}

// 配置里的装备套装写法 → 「编号 名字」。配置里存的就是编号；不是编号就原样显示，
// 免得让人以为配置被改了。
std::string EquipLabel(const std::vector<LoadoutName>& names, const std::string& v)
{
    if (v.empty()) return v;
    int n = 0;
    if (ParseDigits(v, n)) {
        for (const auto& e : names)
            if (e.store == n) return std::to_string(e.number) + (e.name.empty() ? "" : "  " + e.name);
    }
    return v;
}

// 道具：配置里存 "#存放格"（排过序也不会套错），也认显示编号。
std::string ItemLabel(const std::vector<LoadoutName>& names, const std::string& v)
{
    if (v.empty()) return v;
    if (v[0] == '#') {
        int st = 0;
        if (!ParseDigits(v.substr(1), st)) return v;
        for (const auto& it : names)
            if (it.store == st) return std::to_string(it.number) + (it.name.empty() ? "" : "  " + it.name);
        return v;
    }
    int n = 0;
    if (ParseDigits(v, n)) {
        for (const auto& it : names)
            if (it.number == n) return std::to_string(it.number) + (it.name.empty() ? "" : "  " + it.name);
    }
    return v;
}

// 名字首尾的空格去掉（中间的不动，中文名字里偶尔有空格是对的）
std::string TrimName(const char* s)
{
    std::string r = s ? s : "";
    std::size_t b = r.find_first_not_of(" \t");
    if (b == std::string::npos) return std::string();
    std::size_t e = r.find_last_not_of(" \t");
    return r.substr(b, e - b + 1);
}

// 名字有几个字：UTF-8 里只数每个字符的首字节，一个汉字算 1 个字
int CountChars(const char* s)
{
    int n = 0;
    for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; ++p)
        if ((*p & 0xC0) != 0x80) ++n;
    return n;
}

// 只保留前 50 个合法 Unicode 字符；配置中有畸形 UTF-8 时保留合法前缀。
// 每个字符验证后才拷贝，因此结果永远 <= 50 * 4 字节，且不会留下半个字符。
std::string ClampName(const char* utf8)
{
    const unsigned char* p = (const unsigned char*)(utf8 ? utf8 : "");
    std::string name;
    for (int count = 0; *p && count < kMaxNameChars; ++count) {
        const unsigned char lead = *p;
        const int bytes = lead < 0x80 ? 1 : (lead >= 0xC2 && lead <= 0xDF ? 2 :
                          (lead >= 0xE0 && lead <= 0xEF ? 3 : (lead >= 0xF0 && lead <= 0xF4 ? 4 : 0)));
        if (!bytes) break;
        unsigned codepoint = lead & (bytes == 1 ? 0x7F : (0x7F >> bytes));
        bool valid = true;
        for (int i = 1; i < bytes; ++i) {
            if ((p[i] & 0xC0) != 0x80) { valid = false; break; }
            codepoint = (codepoint << 6) | (p[i] & 0x3F);
        }
        if (!valid || (bytes == 2 && codepoint < 0x80) || (bytes == 3 && codepoint < 0x800) ||
            (bytes == 4 && codepoint < 0x10000) || (codepoint >= 0xD800 && codepoint <= 0xDFFF) ||
            codepoint > 0x10FFFF) break;
        name.append((const char*)p, bytes);
        p += bytes;
    }
    return name;
}

// 在 ImGui 的编辑状态里同步截断，不能只改外部缓冲区（下一帧会被内部旧值覆盖）。
int RenameInputCallback(ImGuiInputTextCallbackData* data)
{
    const std::string name = ClampName(data->Buf);
    if ((int)name.size() < data->BufTextLen)
        data->DeleteChars((int)name.size(), data->BufTextLen - (int)name.size());
    return 0;
}

void OpenRename(Model& m, RenameKind kind, int id)
{
    g_renameCreate = false;
    g_afterRename = 0;
    if (!m.cfg) return;
    std::string cur;
    if (kind == kRenameSet) {
        if (id < 0 || id >= (int)m.cfg->sets.size()) return;
        cur = m.cfg->sets[id].name;
    } else if (kind == kRenameFood) {
        cur = FoodLabel(*m.cfg, id);
    } else {
        return;
    }
    g_renameKind = kind;
    g_renameId = id;
    g_renameErr.clear();
    cur = ClampName(cur.c_str());
    std::memcpy(g_renameBuf, cur.c_str(), cur.size() + 1);
    g_renameFocus = true;
    g_renameOpen = false;
}

// 新建时先建出来（默认名），再弹框让用户直接起名；确认或取消都进编辑页
void OpenRenameCreate(Model& m, RenameKind kind, int id)
{
    if (!m.cfg) return;
    g_renameKind = kind;
    g_renameId = id;
    g_renameCreate = true;
    g_renameErr.clear();
    g_renameBuf[0] = 0;
    g_renameFocus = true;
    g_renameOpen = false;
}

// 移动模式下把选中的那一项往上 / 往下挪一格。套装是换 vector 里的位置（编号跟着走），
// 猫饭只改显示顺序（foodOrder），编号不动
void ReorderItem(Model& m, MoveKind kind, int id, int dir)
{
    Config& c = *m.cfg;
    if (kind == kMoveSet) {
        int idx = -1;
        for (int i = 0; i < (int)c.sets.size(); ++i) if (c.sets[i].id == id) { idx = i; break; }
        if (idx < 0) return;
        const int to = idx + dir;
        if (to < 0 || to >= (int)c.sets.size()) return;
        std::swap(c.sets[idx], c.sets[to]);
        if (g_editSet == idx) g_editSet = to;
        else if (g_editSet == to) g_editSet = idx;
        if (to == 0) ImGui::SetScrollY(0.0f);                                 // 挪到两端直接贴边
        else if (to + 1 == (int)c.sets.size()) ImGui::SetScrollY(ImGui::GetScrollMaxY());
        m.reqSave = true;
    } else if (kind == kMoveFood) {
        std::vector<int> ids = FoodRowIds(c);
        int idx = -1;
        for (int i = 0; i < (int)ids.size(); ++i) if (ids[i] == id) { idx = i; break; }
        if (idx < 0) return;
        const int to = idx + dir;
        if (to < 0 || to >= (int)ids.size()) return;
        std::swap(ids[idx], ids[to]);
        c.foodOrder = ids;
        if (to == 0) ImGui::SetScrollY(0.0f);
        else if (to + 1 == (int)ids.size()) ImGui::SetScrollY(ImGui::GetScrollMaxY());
        m.reqSave = true;
    }
}

std::string SetSummary(const LoadoutSet& s, const Model& m)
{
    std::string r;
    auto add = [&](const std::string& t) { if (!r.empty()) r += " · "; r += t; };
    if (!s.equip.empty()) add(std::string(i18n::Tr("装备 ", "Equip ", "装備 ")) + EquipLabel(m.equipNames, s.equip));
    if (!s.item.empty()) add(std::string(i18n::Tr("道具 ", "Items ", "アイテム ")) + ItemLabel(m.itemNames, s.item));
    if (s.food > 0) add(FoodLabel(*m.cfg, s.food));
    return r.empty() ? i18n::Tr("空套装", "empty set", "空のセット") : r;
}

std::string HotkeyText(const std::string& spec)
{
    std::string r;
    for (char ch : spec) {
        if (ch == '+') r += " + ";
        else if (ch == '|') r += i18n::Tr("  或  ", " / ", " または ");
        else if (ch != ' ') r += ch;
    }
    return r;
}

// ---------------------------------------------------------------------------
//  各页
// ---------------------------------------------------------------------------
int ActiveSetIndex(const Config& c)
{
    if (!c.enabled) return -1;   // 卸载态：不预先套用套装
    for (int i = 0; i < (int)c.sets.size(); ++i) if (c.sets[i].id == c.activeSet) return i;
    return -1;
}

void PageHome(Model& m)
{
    Config& c = *m.cfg;
    const float w = ImGui::GetContentRegionAvail().x;
    const int setIdx = ActiveSetIndex(c);
    const LoadoutSet* set = setIdx >= 0 ? &c.sets[setIdx] : nullptr;
    const int quest = EffectiveQuest(c);

    ImGui::Dummy(ImVec2(0, 4 * S));
    Text(fReg, szSmall, kMuted, i18n::Tr("当前任务", "Current quest", "現在のクエスト"));
    ImGui::Dummy(ImVec2(0, 2 * S));
    Text(fBold, szTitle, kTitle, QuestLabel(quest, i18n::Tr("刚才打的那个任务", "the last quest you ran", "さっき戦ったクエスト")).c_str(), true);
    // 副标题：重开时会套用什么（套装、道具套装、猫饭）
    std::string sub;
    auto add = [&](const std::string& t) { sub += (sub.empty() ? "" : " · ") + t; };
    if (set) add(i18n::Fmt(i18n::Tr("套装「%s」", "set \"%s\"", "セット「%s」"), set->name.c_str()));
    if (set && !set->equip.empty()) add(i18n::Fmt(i18n::Tr("装备「%s」", "equip \"%s\"", "装備「%s」"), EquipLabel(m.equipNames, set->equip).c_str()));
    if (set && !set->item.empty()) add(i18n::Fmt(i18n::Tr("道具「%s」", "items \"%s\"", "アイテム「%s」"), ItemLabel(m.itemNames, set->item).c_str()));
    const int food = EffFoodId(c);
    if (food > 0) add(FoodLabel(c, food));
    if (!sub.empty()) Text(fReg, szSmall, kMuted, sub.c_str(), true);
    ImGui::Dummy(ImVec2(0, 24 * S));

    if (!c.enabled) {
        Text(fReg, szSmall, kWarn, i18n::Tr("现在是卸载态：功能全停，只保留面板（设置页可以切回装载态）",
                                            "Unloaded: features off, panel only (see Settings)",
                                            "停止中：機能なし、パネルのみ（設定で戻せます）"), true);
        ImGui::Dummy(ImVec2(0, 6 * S));
    }
    if (!m.ready) {
        Text(fReg, szSmall, kWarn, i18n::Tr("重开功能没装上，详情见 BetterINQ.log",
                                            "restart hook missing; see BetterINQ.log",
                                            "再開機能を組み込めませんでした。BetterINQ.log を見てください"), true);
        ImGui::Dummy(ImVec2(0, 6 * S));
    }
    if (PillButton("##restart", i18n::Tr("重开", "Restart", "再開"), w, 58 * S, kPrimary, kIcRefresh)) { m.reqRestart = true; m.reqClose = true; }
    FocusDefault();
    if (!c.hkRestart.empty()) {
        const std::string hk = HotkeyText(c.hkRestart);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        CenterText(ImGui::GetWindowDrawList(), fReg, szSmall, ImVec2(p.x + w * 0.5f, p.y + szSmall * 0.8f), kFaint, hk.c_str());
        ImGui::Dummy(ImVec2(0, szSmall * 1.6f));
    }
    ImGui::Dummy(ImVec2(0, 18 * S));

    const float col = w / 3.0f;
    if (RoundButton("##q", kIcSearch, i18n::Tr("换任务", "Quests", "クエスト"), col)) {
        // 套装里指定了任务的话，重开进的是套装的任务 —— 那就直接改套装里的
        g_questTarget = (set && set->quest) ? setIdx : -1;
        g_questFrom = kHome;
        Go(kQuests);
    }
    ImGui::SameLine(0, 0);
    if (RoundButton("##s", kIcShield, i18n::Tr("换套装", "Loadouts", "装備セット"), col)) Go(kSets);
    ImGui::SameLine(0, 0);
    if (RoundButton("##f", kIcBowl, i18n::Tr("换猫饭", "Meals", "猫飯"), col)) Go(kFood);

    // 次要动作：只把当前任务重置掉，回据点，不接任务也不出发（原 IN-Q 的 Reset only）。
    // 放在下面一排圆按钮之后：重开是主角，这一行是「这次只想要个干净的任务」时才用。
    ImGui::Dummy(ImVec2(0, 14 * S));
    if (PillButton("##resetonly", i18n::Tr("只重置任务", "Reset quest only", "クエストのリセットのみ"),
                   w, 42 * S, kSecondary)) { m.reqResetOnly = true; m.reqClose = true; }
    if (!c.hkResetOnly.empty()) {
        const std::string hk = HotkeyText(c.hkResetOnly);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        CenterText(ImGui::GetWindowDrawList(), fReg, szSmall, ImVec2(p.x + w * 0.5f, p.y + szSmall * 0.8f), kFaint, hk.c_str());
        ImGui::Dummy(ImVec2(0, szSmall * 1.6f));
    } else {
        // 没有快捷键就把这句话放这儿：和「重开」的区别只有一句
        const ImVec2 p = ImGui::GetCursorScreenPos();
        CenterText(ImGui::GetWindowDrawList(), fReg, szSmall, ImVec2(p.x + w * 0.5f, p.y + szSmall * 0.8f), kFaint,
                   i18n::Tr("重置完回据点，不接任务也不出发", "resets to base without accepting",
                            "リセットして拠点に戻る（受注・出発なし）"));
        ImGui::Dummy(ImVec2(0, szSmall * 1.6f));
    }
}

// 可展开的分组行：左边箭头，右边这一组的任务数
bool GroupRow(const char* id, const char* label, int count, bool open)
{
    const float h = 44 * S;
    ImGui::PushID(id);
    const bool pressed = ImGui::Selectable("##grp", false, ImGuiSelectableFlags_None, ImVec2(0, h));
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(a.x + 18 * S, (a.y + b.y) * 0.5f);
    const float r = 5 * S, t = 1.8f * S;
    if (open) {   // ⌄
        dl->AddLine(ImVec2(c.x - r, c.y - r * 0.5f), ImVec2(c.x, c.y + r * 0.5f), kGold, t);
        dl->AddLine(ImVec2(c.x, c.y + r * 0.5f), ImVec2(c.x + r, c.y - r * 0.5f), kGold, t);
    } else {      // ›
        dl->AddLine(ImVec2(c.x - r * 0.5f, c.y - r), ImVec2(c.x + r * 0.5f, c.y), kMuted, t);
        dl->AddLine(ImVec2(c.x + r * 0.5f, c.y), ImVec2(c.x - r * 0.5f, c.y + r), kMuted, t);
    }
    const ImVec2 ts = TextSize(fBold, szBody, label);
    dl->AddText(fBold, szBody, ImVec2(a.x + 36 * S, std::floor(a.y + (h - ts.y) * 0.5f)), open ? kTitle : kText, label);
    char n[16];
    std::snprintf(n, sizeof(n), "%d", count);
    const ImVec2 ns = TextSize(fReg, szSmall, n);
    dl->AddText(fReg, szSmall, ImVec2(b.x - 14 * S - ns.x, std::floor(a.y + (h - ns.y) * 0.5f)), kFaint, n);
    ImGui::PopID();
    return pressed;
}

void PageQuests(Model& m)
{
    Config& c = *m.cfg;
    const bool forSet = g_questTarget >= 0 && g_questTarget < (int)c.sets.size();
    int& target = forSet ? c.sets[g_questTarget].quest : c.questId;
    const std::string sub = forSet
        ? i18n::Fmt(i18n::Tr("改的是套装「%s」里的任务",
                             "editing the quest of set \"%s\"",
                             "セット「%s」のクエストを変更中"),
                    c.sets[g_questTarget].name.c_str())
        : std::string();
    PageHeader(i18n::Tr("换任务", "Quests", "クエスト"), forSet ? sub.c_str() : nullptr, true);

    ImGui::SetNextItemWidth(-1);
    ImGui::PushFont(fReg, szBody);
    if (g_debugFocusSearch) {
        // Resolve the ID in its real submission scope. SetKeyboardFocusHere alone
        // can resolve to another item in this NavFlattened child when the list's
        // manual default-focus request is also pending.
        const ImGuiID id = ImGui::GetID("##search");
        ImGui::SetFocusID(id, ImGui::GetCurrentWindow());
        GImGui->NavActivateId = id;
        GImGui->NavActivateFlags = ImGuiActivateFlags_PreferInput;
        g_debugFocusSearch = false;
    }
    ImGui::InputTextWithHint("##search",
                             i18n::Tr("任务名、拼音、假名、罗马音或 ID",
                                      "name, pinyin, kana, romaji or ID",
                                      "名前・ピンイン・かな・ローマ字・ID"),
                             g_search, sizeof(g_search));
    const ImGuiID searchId = ImGui::GetItemID();
    ImGuiWindow* searchWindow = ImGui::GetCurrentWindow();
    const ImRect searchRect = GImGui->LastItemData.NavRect;
    if (HintTarget()) g_hintTextField = true;
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 4 * S));

    const float listH = ImGui::GetContentRegionAvail().y;
    auto pick = [&](int id) { target = id; m.reqSave = true; Go(Back(kQuests)); };

    int gn = 0, qn = 0;
    const QuestGroup* groups = gamedata::Groups(gn);
    const QuestInfo* qs = gamedata::Quests(qn);
    if (gn > 16) gn = 16;
    if (g_questInit) {
        g_questInit = false;
        for (bool& o : g_groupOpen) o = false;
        int open = -1;
        if (const QuestInfo* tq = gamedata::FindQuest(target))
            if (tq->group >= 0 && tq->group < gn) open = tq->group;
        // 还没有选中任务（全局是「当前任务」/ 套装是「不指定」）时，把第一组打开：不然整页只剩 7 个
        // 折叠的分组标题，手柄上下走几下就到底了，看着就像「列表里没任务、卡住」。
        for (int i = 0; open < 0 && i < qn; ++i)
            if (qs[i].group >= 0 && qs[i].group < gn) open = qs[i].group;
        if (open >= 0) g_groupOpen[open] = true;
    }

    const std::string needle = g_search;
    const bool searching = !needle.empty();
    // 全局任务可以是 0 =「当前任务」（重开刚打完 / 正在打的那个）；套装里的 0 =「不指定，跟全局走」。
    // 两边都得能选回 0，不然给套装挑过一次任务之后就再也退不回「不指定」了。搜索时它和搜的字无关，不列
    const bool zeroRow = !searching;
    // 先确定完整布局，再 BeginChild：第一帧就有正确 ScrollMax，目标只设置一次。
    std::vector<int> byGroup[16];
    for (int i = 0; i < qn; ++i)
        if (qs[i].group >= 0 && qs[i].group < gn && gamedata::QuestMatches(qs[i], needle)) byGroup[qs[i].group].push_back(i);
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    float contentH = zeroRow ? 46 * S + spacing : 0.0f;
    float selectedY = 0.0f, selectedH = 46 * S;
    for (int gi = 0; gi < gn; ++gi) {
        if (byGroup[gi].empty()) continue;
        contentH += 2 * S + spacing + 44 * S + spacing;
        if (!(searching || g_groupOpen[gi])) continue;
        for (int i : byGroup[gi]) {
            if (qs[i].id == target) { selectedY = contentH; selectedH = 42 * S; }
            contentH += 42 * S + spacing;
        }
    }
    ImGui::SetNextWindowContentSize(ImVec2(0, std::fmax(0.0f, contentH - spacing)));
    if (g_scrollToSel) {
        const float padding = ImGui::GetStyle().WindowPadding.y;
        ImGui::SetNextWindowScroll(ImVec2(-1, std::fmax(0.0f, selectedY + padding - (listH - selectedH) * 0.3f)));
        g_scrollToSel = false;
    }
    // Selectable 会向两侧扩半个 ItemSpacing；无内边距时 NavRect 超出 InnerRect，
    // ImGui 的手柄导航会重置 NavId 并夹回打分区域，导致在列表边缘走不出去。
    // 保留横向内边距，避免原先靠绘制后的整行补滚掩盖这个问题。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * S, 0));
    ImGui::BeginChild("##qlist", ImVec2(0, listH), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGuiWindow* listWindow = ImGui::GetCurrentWindow();
    ImGuiID firstListId = 0;
    ImRect firstListRect;
    auto rememberFirst = [&]() {
        if (!firstListId) { firstListId = ImGui::GetItemID(); firstListRect = GImGui->LastItemData.NavRect; }
    };
    if (zeroRow) {
        const char* zeroLabel = forSet
            ? i18n::Tr("不指定（跟全局任务走）", "no quest (use the global one)", "指定しない（全体を使う）")
            : i18n::Tr("当前任务", "Current quest", "現在のクエスト");
        if (Row("q0", zeroLabel, nullptr, target == 0)) pick(0);
        rememberFirst();
        if (target == 0) FocusDefault();
    }
    // 焦点先落在选中项上；一项都没选中就落在第一组标题上（键盘上下才有个起点）
    const bool hasSel = gamedata::FindQuest(target) || (zeroRow && target == 0);

    for (int gi = 0; gi < gn; ++gi) {
        if (byGroup[gi].empty()) continue;
        const bool open = searching || g_groupOpen[gi];
        char gid[8];
        std::snprintf(gid, sizeof(gid), "g%d", gi);
        ImGui::Dummy(ImVec2(0, 2 * S));
        if (GroupRow(gid, gamedata::GroupName(groups[gi]), (int)byGroup[gi].size(), open) && !searching) g_groupOpen[gi] = !g_groupOpen[gi];
        rememberFirst();
        if (!hasSel && gi == 0) FocusDefault();
        if (!open) continue;
        ImGui::Indent(10 * S);
        for (int i : byGroup[gi]) {
            const QuestInfo& q = qs[i];
            char idbuf[16];
            std::snprintf(idbuf, sizeof(idbuf), "%d", q.id);
            const bool sel = (q.id == target);
            if (Row(idbuf, gamedata::QuestName(q), idbuf, sel, 42 * S)) pick(q.id);
            if (sel) FocusDefault();
        }
        ImGui::Unindent(10 * S);
    }
    if (searching && std::all_of(byGroup, byGroup + gn, [](const std::vector<int>& v) { return v.empty(); }))
        Text(fReg, szSmall, kMuted, i18n::Tr("没找到", "nothing found", "見つかりません"));
    // The flattened scrolling child clips native Up scoring to its own list.
    // Bridge only its actual first submitted item, not arbitrary Up presses or
    // an active text caret. All keyboard/pad/letter navigation uses this request.
    ImGuiContext& nav = *GImGui;
    ImGuiID requestSource = nav.NavId;
    // Gamepad-only native scoring projects a partly clipped NavRect into the
    // child InnerRect and clears NavId (Selectable expands past the top edge).
    // NavLastIds still identifies the original request source: use it only for
    // this exact list/search boundary, never to redirect an arbitrary Up.
    if (!requestSource && nav.NavMoveSubmitted && nav.NavInputSource == ImGuiInputSource_Gamepad &&
        (nav.NavWindow == listWindow || nav.NavWindow == searchWindow))
        requestSource = nav.NavWindow->NavLastIds[ImGuiNavLayer_Main];
    auto cancelBoundaryMove = [&]() {
        nav.NavInitRequest = nav.NavInitRequestFromMove = false;
        nav.NavInitResult.ID = 0;
        // The declared internal API also recomputes NavAnyRequest after init
        // cancellation; NavUpdateAnyRequestFlag itself is translation-unit private.
        ImGui::NavMoveRequestCancel();
    };
    if (g_page == kQuests && firstListId && requestSource == firstListId &&
        nav.NavMoveSubmitted && nav.NavMoveDir == ImGuiDir_Up &&
        nav.ActiveId == 0 && !nav.IO.WantTextInput && !m.capturing &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        cancelBoundaryMove();
        ImGui::SetFocusID(searchId, searchWindow);
        searchWindow->NavRectRel[ImGuiNavLayer_Main] = ImRect(searchRect.Min - searchWindow->Pos, searchRect.Max - searchWindow->Pos);
        ImGui::SetNavCursorVisible(true);
        ImGui::SetScrollY(0.0f);
        g_focus = 0;
        g_hintTextField = true;
    } else if (g_page == kQuests && firstListId && requestSource == searchId &&
        nav.NavMoveSubmitted && nav.NavMoveDir == ImGuiDir_Down &&
        nav.ActiveId == 0 && !nav.IO.WantTextInput && !m.capturing &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        cancelBoundaryMove();
        ImGui::SetFocusID(firstListId, listWindow);
        listWindow->NavRectRel[ImGuiNavLayer_Main] = ImRect(firstListRect.Min - listWindow->Pos, firstListRect.Max - listWindow->Pos);
        ImGui::SetNavCursorVisible(true);
        ImGui::SetScrollY(0.0f);
        g_focus = 0;
        g_hintTextField = false;
    }
    ImGui::EndChild();
}

// 垃圾桶图标：盖子、提手、桶身、两道竖纹
void DrawTrash(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
    const float t = 1.8f * S;
    dl->AddLine(ImVec2(c.x - r * 0.8f, c.y - r * 0.55f), ImVec2(c.x + r * 0.8f, c.y - r * 0.55f), col, t);
    dl->AddRect(ImVec2(c.x - r * 0.28f, c.y - r * 0.85f), ImVec2(c.x + r * 0.28f, c.y - r * 0.55f), col, 1.5f * S, 0, t);
    dl->AddRect(ImVec2(c.x - r * 0.58f, c.y - r * 0.38f), ImVec2(c.x + r * 0.58f, c.y + r * 0.85f), col, 3 * S,
                ImDrawFlags_RoundCornersBottom, t);
    dl->AddLine(ImVec2(c.x - r * 0.2f, c.y - r * 0.12f), ImVec2(c.x - r * 0.2f, c.y + r * 0.58f), col, t);
    dl->AddLine(ImVec2(c.x + r * 0.2f, c.y - r * 0.12f), ImVec2(c.x + r * 0.2f, c.y + r * 0.58f), col, t);
}

// 一行居中的字
void CenterLine(ImFont* f, float size, ImU32 col, const char* s)
{
    const float cw = ImGui::GetContentRegionAvail().x;
    const ImVec2 ts = TextSize(f, size, s);
    if (ts.x > cw) { Text(f, size, col, s, true); return; }   // 太长：换行，靠左
    const ImVec2 q = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(f, size, ImVec2(std::floor(q.x + (cw - ts.x) * 0.5f), q.y), col, s);
    ImGui::Dummy(ImVec2(cw, ts.y));
}

// 编辑页底部的「删除」：先弹一张确认卡片（盖在面板正中，背后压暗）。
// 焦点默认在「取消」上，免得手柄连按 A 误删；B / Esc 也是取消。确认删除返回 true
bool DeleteButton(const std::string& what)
{
    const bool trigger = PillButton("##del", i18n::Tr("删除", "Delete", "削除"), ImGui::GetContentRegionAvail().x, 46 * S, kDanger);
    if (trigger || g_openDelete) {
        g_openDelete = false;
        ImGui::OpenPopup("confirm_del");
    }
    bool yes = false;
    ImGui::SetNextWindowPos(ImVec2((g_panelMin.x + g_panelMax.x) * 0.5f, (g_panelMin.y + g_panelMax.y) * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(372 * S, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26 * S, 26 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 20 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f * S);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.125f, 0.105f, 0.08f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.89f, 0.76f, 0.48f, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.02f, 0.015f, 0.01f, 0.5f));
    const bool open = ImGui::BeginPopupModal("confirm_del", nullptr,
                                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
    if (open) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cw = ImGui::GetContentRegionAvail().x;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float ir = 24 * S;
        const ImVec2 ic(p.x + cw * 0.5f, p.y + ir);
        dl->AddCircleFilled(ic, ir, IM_COL32(232, 140, 110, 36));
        DrawTrash(dl, ic, 12 * S, kWarn);
        ImGui::Dummy(ImVec2(cw, ir * 2 + 16 * S));
        CenterLine(fBold, szBody * 1.15f, kTitle, i18n::Fmt(i18n::Tr("删除「%s」？", "Delete \"%s\"?", "「%s」を削除しますか？"), what.c_str()).c_str());
        ImGui::Dummy(ImVec2(0, 6 * S));
        CenterLine(fReg, szSmall, kMuted, i18n::Tr("删除后无法恢复", "This cannot be undone.", "削除すると元に戻せません"));
        ImGui::Dummy(ImVec2(0, 22 * S));
        const float gap = 12 * S, bw = std::floor((cw - gap) * 0.5f), bh = 44 * S;
        const bool cancel = PillButton("##no", i18n::Tr("取消", "Cancel", "キャンセル"), bw, bh, kSecondary);
        // 焦点放在「取消」上，并且显示焦点框：ImGui 在焦点框藏着时不认手柄 A
        //（用鼠标点的「删除」再拿手柄按 A，不显示的话就没反应）
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
            ImGui::SetNavCursorVisible(true);
        }
        ImGui::SameLine(0, gap);
        const bool confirm = PillButton("##yes", i18n::Tr("删除", "Delete", "削除"), bw, bh, kDangerSolid);
        // Cancellation wins even when conflicting inputs arrive in the same frame.
        const bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        yes = confirm && !cancel && !back && !ImGui::IsWindowAppearing();
        if (yes || cancel || back) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    return yes;
}

// 设置页的「清空」：两层确认。第一层问要不要清，第二层才是真的清——
// 清空会连套装、猫饭、以及它们的编号一起归零，误按一次代价太大。
void ClearAllButton(Model& m)
{
    Config& c = *m.cfg;
    int& stage = g_clearStage;   // 0 = 没弹，1 = 第一层，2 = 最后一层
    if (PillButton("##clearall", i18n::Tr("清空所有套装和猫饭", "Clear all sets and meals", "セットと猫飯をすべて消す"),
                   ImGui::GetContentRegionAvail().x, 46 * S, kDanger)) {
        stage = 1;
        ImGui::OpenPopup("confirm_clear");
    }
    if (g_focusClearAll) { g_focusClearAll = false; FocusItem(); }
    if (ImGui::BeginPopup("confirm_clear")) {
        if (stage <= 1) {
            Text(fReg, szBody, kText,
                 i18n::Tr("要清空所有套装和猫饭吗？", "Clear every set and meal?", "セットと猫飯をすべて消しますか？"));
            ImGui::Dummy(ImVec2(0, 4 * S));
            Text(fReg, szSmall, kFaint,
                 i18n::Tr("清完之后可以用上面的「从原版 IN-Q 导入」重新搬一份",
                          "import a fresh copy afterwards",
                          "このあと上の「旧 IN-Q から取り込む」で入れ直せます"),
                 true);
        } else {
            Text(fReg, szBody, kWarn,
                 i18n::Tr("清空之后恢复不了，确定吗？", "This cannot be undone.",
                          "元に戻せません。本当に消しますか？"));
            ImGui::Dummy(ImVec2(0, 4 * S));
            Text(fReg, szSmall, kFaint,
                 i18n::Tr("套装、猫饭和它们的编号都会归零", "sets, meals and counters all reset",
                          "セット・猫飯とその番号がすべて消えます"),
                 true);
        }
        ImGui::Dummy(ImVec2(0, 6 * S));
        const float bw = 220 * S;
        if (stage <= 1) {
            if (PillButton("##clear_next", i18n::Tr("继续", "Continue", "続ける"), bw, 40 * S, kDanger)) stage = 2;
            ImGui::Dummy(ImVec2(0, 4 * S));
            if (PillButton("##clear_cancel", i18n::Tr("取消", "Cancel", "キャンセル"), bw, 40 * S, kSecondary)) {
                stage = 0;
                ImGui::CloseCurrentPopup();
            }
        } else {
            // 按钮 ID 与第一层保持一致：不然内容一换，手柄焦点就丢了，还得再推一下摇杆
            if (PillButton("##clear_next", i18n::Tr("清空", "Clear", "すべて消す"), bw, 40 * S, kDanger)) {
                c.ClearLoadouts();      // 套装/猫饭/编号/当前选中一起归零
                m.reqApplyEquip = true; // 同时撤销尚在等待安全落点的旧装备请求
                m.reqSave = true;
                stage = 0;
                ImGui::CloseCurrentPopup();
                Log("ui: 清空了所有套装和猫饭");
            }
            ImGui::Dummy(ImVec2(0, 4 * S));
            if (PillButton("##clear_cancel", i18n::Tr("取消", "Cancel", "キャンセル"), bw, 40 * S, kSecondary)) {
                stage = 0;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

// 移动模式下的自动滚动：项挪到「下一行会被挡住」时才滚一格。
// 边界用这一帧实际被裁到哪（内容区高度可能比面板真正能看见的部分大，拿内容区当边界
// 会永远判成界外，往下就一路滚到底）；每次只滚一行，上下对称，来回挪能精确回到原位。
void ScrollMovingIntoView(float rowTop, float rowH)
{
    const ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 clipMin = dl->GetClipRectMin();
    const ImVec2 clipMax = dl->GetClipRectMax();
    const float rowBottom = rowTop + rowH;
    const float pad = 4.0f;   // 贴着边界（差 0~3 px）也算要滚，否则收尾会差半格
    if (rowBottom > clipMax.y - pad) ImGui::SetScrollY(ImGui::GetScrollY() + rowH);
    else if (rowTop < clipMin.y + pad) ImGui::SetScrollY(ImGui::GetScrollY() - rowH);
    g_listScroll = ImGui::GetScrollY();   // 给回归测试读
}

void PageSets(Model& m)
{
    Config& c = *m.cfg;
    if (g_reorderDir && g_listFocusId) {
        ReorderItem(m, kMoveSet, g_listFocusId, g_reorderDir);
        g_focusId = g_listFocusId; // 包括列表边缘：焦点不能落到邻行或新建按钮
    }
    g_listFocusId = 0;
    g_listFocusNavId = 0;
    const int activeIdx = ActiveSetIndex(c);
    const std::string sub = activeIdx >= 0
        ? i18n::Fmt(i18n::Tr("重开时套用「%s」", "applied on restart: \"%s\"", "再開時に適用「%s」"),
                    c.sets[activeIdx].name.c_str())
        : std::string();
    PageHeader(i18n::Tr("套装", "Loadouts", "装備セット"), activeIdx >= 0 ? sub.c_str() : nullptr, false);
    // 左摇杆按下：把选中的这一套放进移动模式，上下键换位置，再按一次放下
    const bool moving = g_moveKind == kMoveSet && c.FindSetById(g_moveId) != nullptr;
    if (!moving && g_moveKind == kMoveSet) { g_moveKind = kMoveNone; g_moveId = 0; }
    if (moving) {
        // repeat = true：按住不放也要一直换位置（否则只有第一下有效），
        // 之前这里写 false，长按就变成了「Focus 在列表里上下走」
        const bool up = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, true) || ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) || g_letterMove[ImGuiDir_Up];
        const bool down = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, true) || ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) || g_letterMove[ImGuiDir_Down];
        if (up || down) ReorderItem(m, g_moveKind, g_moveId, up ? -1 : 1);
        const bool put = ImGui::IsKeyPressed(ImGuiKey_GamepadL3, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) ||
                        ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
                        ImGui::IsKeyPressed(ImGuiKey_E, false);
        const bool cancel = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (put || cancel) {
            g_moveKind = kMoveNone;
            g_focusId = g_moveId;
            ImGui::SetNavCursorVisible(true);
        }
    }
    // 移动中列表项不参与十字键导航：否则上下键一边换位置一边把 Focus 挪到邻行
    if (moving) ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    for (int i = 0; i < (int)c.sets.size(); ++i) {
        ImGui::PushID(c.sets[i].id);
        const std::string rowId = "set" + std::to_string(c.sets[i].id);
        const bool isMoving = moving && g_moveId == c.sets[i].id;
        const float rowTop = ImGui::GetCursorScreenPos().y;   // 行被完全裁掉时 GetItemRect 不可靠，先记下行的顶
        const bool acted = Row(rowId.c_str(), c.sets[i].name.c_str(),
                               isMoving ? "" : SetSummary(c.sets[i], m).c_str(), i == activeIdx, 54 * S, isMoving);
        const ImGuiID rid = ImGui::GetItemID();   // 这一行的 ID：放下后把焦点还回来要用
        if (ImGui::IsItemClicked()) g_mouseDownRow = rowId;
        if (isMoving) {
            // 正在移动的那一项：整行淡金底 + 金边，右边写「移动中」
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            dl->AddRectFilled(a, b, IM_COL32(226, 194, 122, 30), 12 * S);
            dl->AddRect(a - ImVec2(2, 2) * S, b + ImVec2(2, 2) * S, kGold, 12 * S, 0, 2.0f * S);
            const char* tagTxt = i18n::Tr("移动中", "moving", "移動中");
            const ImVec2 ts = TextSize(fReg, szSmall, tagTxt);
            dl->AddText(fReg, szSmall, ImVec2(b.x - 14 * S - ts.x, std::floor(a.y + (b.y - a.y - ts.y) * 0.5f)), kGold, tagTxt);
        }
        if (!moving && acted) {
            g_mouseDownRow.clear();
            if (c.enabled) {
                const bool wasActive = c.activeSet == c.sets[i].id;
                c.activeSet = wasActive ? -1 : c.sets[i].id;
                m.reqApplyEquip = true;
                m.reqSave = true;
                Log("ui: 套装 %s id=%d", wasActive ? "取消套用" : "套用", c.sets[i].id);
            }
        }
        // 选中一项后按 X（手柄）或 F（键盘）改名
        if (!moving && ImGui::IsItemFocused() &&
            (ImGui::IsKeyPressed(ImGuiKey_F, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)))
            OpenRename(m, kRenameSet, i);
        // Y 只进入编辑，不改变套用状态。
        if (!moving && ((ImGui::IsItemFocused() &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) || ImGui::IsKeyPressed(ImGuiKey_Y, false))) ||
            (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)))) {
            g_editSet = i;
            Go(kSetEdit);
        }
        // 按左摇杆：进入移动模式（哪一项被选中就移动哪一项）
        if (!moving && ImGui::IsItemFocused() &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadL3, false) || ImGui::IsKeyPressed(ImGuiKey_E, false))) {
            Log("ui: 套装进入移动模式 id=%d", c.sets[i].id);
            g_moveKind = kMoveSet;
            g_moveId = c.sets[i].id;
        }
        if (!moving && i == (activeIdx >= 0 ? activeIdx : 0)) FocusDefault();
        if (isMoving) ScrollMovingIntoView(rowTop, 54 * S);
        if (!moving && g_focusId == c.sets[i].id) {
            ScrollMovingIntoView(rowTop, 54 * S);
            g_focusId = 0;
            ImGui::SetFocusID(rid, ImGui::GetCurrentWindow());
            ImGui::SetNavCursorVisible(true);
        }
        if (!moving) HintListRow(i == activeIdx);
        if (!moving && ImGui::IsItemFocused()) {
            g_listFocusId = c.sets[i].id;
            g_listFocusNavId = rid;
        }
        ImGui::PopID();
        if (!moving) FollowFocusedRow();
    }
    if (moving) ImGui::PopItemFlag();
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) g_mouseDownRow.clear();
    ImGui::Dummy(ImVec2(0, 8 * S));
    const bool newSet = PillButton("##newset", i18n::Tr("新建套装", "New loadout", "新規セット"), ImGui::GetContentRegionAvail().x, 46 * S, kSecondary, kIcPlus);
    if (c.sets.empty()) FocusDefault();
    ImGui::Dummy(ImVec2(0, 2 * S));
    if (newSet) {
        LoadoutSet s;
        s.id = c.AllocSetId();                 // 编号只增不减：删掉的套装编号不会再分出去
        s.name = std::string(i18n::Tr("套装", "Loadout", "セット")) + std::to_string((int)c.sets.size() + 1);
        c.sets.push_back(s);
        g_editSet = (int)c.sets.size() - 1;
        m.reqSave = true;
        g_afterRename = 1;                 // 起完名（或取消）直接进编辑页
        OpenRenameCreate(m, kRenameSet, g_editSet);
    }
}

void PageSetEdit(Model& m)
{
    Config& c = *m.cfg;
    if (g_editSet < 0 || g_editSet >= (int)c.sets.size()) { Go(kSets); return; }
    LoadoutSet& s = c.sets[g_editSet];
    const bool active = s.id == c.activeSet;
    PageHeader(s.name.c_str(), active ? i18n::Tr("装备立即更换，其余重开时套用", "gear now; rest on restart", "装備は即時、その他は再開時に適用") : nullptr, true);

    // 装备（存编号：游戏里装备套装的存放格号，1..224）
    std::vector<std::string> equips = {kNone};
    int curE = 0;
    for (const auto& e : m.equipNames) {
        equips.push_back(std::to_string(e.number) + "  " + e.name);
        int n = 0;
        if (ParseDigits(s.equip, n) && e.store == n) curE = (int)equips.size() - 1;
    }
    if (ValueRow("##eq", i18n::Tr("装备套装", "Equip loadout", "装備セット"),
                 s.equip.empty() ? kNone : EquipLabel(m.equipNames, s.equip).c_str()))
        ImGui::OpenPopup("pick_eq");
    FocusDefault();
    int p = PickPopup("pick_eq", equips, curE);
    if (p >= 0) {
        s.equip = p == 0 ? std::string() : std::to_string(m.equipNames[p - 1].store);
        if (active) m.reqApplyEquip = true;
        m.reqSave = true;
    }

    // 道具（存 "#存放格"：名字在游戏里可能重名/为空，编号又会随排序变，只有存放格是稳的）
    std::vector<std::string> items = {kNone};
    int curI = 0;
    for (const auto& it : m.itemNames) {
        items.push_back(std::to_string(it.number) + "  " + it.name);
        const bool byStore = !s.item.empty() && s.item[0] == '#' && std::atoi(s.item.c_str() + 1) == it.store;
        int n = 0;
        const bool byNumber = s.item.empty() ? false : (ParseDigits(s.item, n) && it.number == n);
        if (byStore || byNumber) curI = (int)items.size() - 1;
    }
    if (ValueRow("##it", i18n::Tr("道具套装", "Item loadout", "アイテムセット"),
                 s.item.empty() ? kNone : ItemLabel(m.itemNames, s.item).c_str()))
        ImGui::OpenPopup("pick_it");
    p = PickPopup("pick_it", items, curI);
    if (p >= 0) {
        s.item = p == 0 ? std::string() : "#" + std::to_string(m.itemNames[p - 1].store);
        m.reqSave = true;
    }

    if (ToggleRow("##lr", i18n::Tr("联动快捷栏", "Link shortcut bar", "ショートカットを連動"), nullptr, &s.linkRadial)) m.reqSave = true;

    // 猫饭
    std::vector<std::string> foods = {kNone};
    std::vector<int> foodIds = {-1};
    int curF = 0;
    for (int fid : c.FoodIds()) {
        foods.push_back(FoodLabel(c, fid));
        foodIds.push_back(fid);
        if (fid == s.food) curF = (int)foods.size() - 1;
    }
    const std::string foodText = s.food > 0 ? FoodLabel(c, s.food) : std::string(i18n::Tr("跟全局那份", "the global meal", "全体の猫飯"));
    if (ValueRow("##fd", i18n::Tr("猫饭", "Meal", "猫飯"), foodText.c_str())) ImGui::OpenPopup("pick_fd");
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        (ImGui::IsItemFocused() || ImGui::IsItemHovered())) QueueFoodPreview(c, s.food, false);
    p = PickPopup("pick_fd", foods, curF, &c, &foodIds);
    if (p >= 0) { s.food = foodIds[p]; m.reqSave = true; }

    if (ValueRow("##qs", i18n::Tr("任务", "Quest", "クエスト"), QuestLabel(s.quest, kNone).c_str())) {
        g_questTarget = g_editSet;
        g_questFrom = kSetEdit;
        Go(kQuests);
    }

    if (c.palicoEnabled) {
        ImGui::Dummy(ImVec2(0, 10 * S));
        Text(fReg, szSmall, kFaint, i18n::Tr("随从猫", "Palico", "オトモ"));
        auto pick = [&](const char* id, const char* label, const std::vector<std::string>& values, int& value, int offset) {
            const int current = (std::max)(0, (std::min)(int(values.size()) - 1, value + offset));
            if (ValueRow(id, label, values[current].c_str())) ImGui::OpenPopup(id);
            const int chosen = PickPopup(id, values, current);
            if (chosen >= 0) { value = chosen - offset; m.reqSave = true; }
        };
        const std::string unchanged = i18n::Tr("不改", "Unchanged", "変更なし");
        std::vector<std::string> deploy{unchanged, i18n::Tr("待命", "Standby", "待機"), i18n::Tr("出战", "Deployed", "同行")};
        pick("##palico_deploy", i18n::Tr("是否出战", "Deployment", "同行設定"), deploy, s.palicoDeploy, 1);
        if (NumberRow("##palico_level", i18n::Tr("随从等级", "Palico level", "オトモレベル"), s.palicoLevel, 50)) m.reqSave = true;
        std::vector<std::string> loadouts{unchanged};
        std::vector<int> loadoutIds{0};
        int current = 0;
        std::string selected = s.palicoLoadout == 0 ? unchanged :
            i18n::Fmt(i18n::Tr("失效套装 #%d", "Unavailable #%d", "無効なセット #%d"), s.palicoLoadout);
        for (const auto& entry : m.palicoNames) {
            if (entry.number <= 0 || entry.number > 24 || entry.name.find_first_not_of(" \t\r\n") == std::string::npos) continue;
            loadouts.push_back(std::to_string(entry.number) + "  " + entry.name);
            loadoutIds.push_back(entry.number);
            if (entry.number == s.palicoLoadout) { current = (int)loadouts.size() - 1; selected = loadouts.back(); }
        }
        if (ValueRow("##palico_loadout", i18n::Tr("随从装备套装", "Palico loadout", "オトモ装備セット"), selected.c_str()))
            ImGui::OpenPopup("pick_palico_loadout");
        const int chosen = PickPopup("pick_palico_loadout", loadouts, current);
        if (chosen >= 0) { s.palicoLoadout = loadoutIds[chosen]; m.reqSave = true; }
        if (NumberRow("##palico_gadget_level", i18n::Tr("道具等级", "Gadget level", "道具レベル"), s.palicoGadgetLevel, 15)) m.reqSave = true;
        Text(fReg, szSmall, kFaint, i18n::Tr("仅据点重开时套用；套装用游戏自己的套用，等级会改存档，写入前自动备份。", "Base restart only. Uses the game's apply; levels edit the save, backed up first.", "拠点での再開時のみ反映。セットはゲーム自身の適用処理を使用。レベルはセーブを変更し、事前に自動バックアップします。"), true);
    }

    // 确认 = 选中这套并请求立即换装备，其余仍在重开时套用（内容随改随存）
    ImGui::Dummy(ImVec2(0, 14 * S));
    if (PillButton("##ok", active ? i18n::Tr("确认", "OK", "決定")
                                 : i18n::Tr("确认并套用", "OK, apply on restart", "決定して適用"),
                   ImGui::GetContentRegionAvail().x, 54 * S, kPrimary)) {
        c.activeSet = s.id;
        m.reqApplyEquip = true;
        m.reqSave = true;
        Go(kSets);
    }
    if (active) {
        ImGui::Dummy(ImVec2(0, 6 * S));
        if (PillButton("##unset", i18n::Tr("取消套用（重开时什么都不套）", "Stop applying it", "適用をやめる"),
                       ImGui::GetContentRegionAvail().x, 46 * S, kSecondary)) {
            c.activeSet = -1;
            m.reqApplyEquip = true;
            m.reqSave = true;
            Go(kSets);
        }
    }
    ImGui::Dummy(ImVec2(0, 4 * S));
    if (DeleteButton(s.name)) {
        if (active) { c.activeSet = -1; m.reqApplyEquip = true; }
        c.sets.erase(c.sets.begin() + g_editSet);
        g_editSet = -1;
        m.reqSave = true;
        Go(kSets);
    }
}

// 猫饭列表：没套用套装（或那套没指定猫饭）时，选中 = 每次进任务都吃它；
// 套装自带猫饭时，重开用的是套装的猫饭，这里只能编辑内容
void PageFood(Model& m)
{
    Config& c = *m.cfg;
    // 套用中的套装自带猫饭时，重开吃的是它那份（见 inq::ApplyOptions），列表只能编辑内容；
    // 没带的话「选中」= 改全局那份。
    const LoadoutSet* act = c.FindSetById(c.activeSet);
    const int setFood = (act && act->food > 0) ? act->food : -1;
    const bool locked = setFood > 0;            // 这套自带猫饭：列表只能编辑，不能选
    const int eff = EffFoodId(c);                // 重开时实际会吃的那一份
    const bool has = FoodUsable(c, eff);
    PageHeader(i18n::Tr("猫饭", "Meals", "猫飯"),
               locked ? i18n::Fmt(i18n::Tr("这套套装自带猫饭「%s」", "this loadout brings \"%s\"", "このセットは「%s」を持参"),
                                  FoodLabel(c, eff).c_str()).c_str()
                      : has ? i18n::Fmt(i18n::Tr("每次进任务都吃「%s」", "eaten on every quest: \"%s\"", "クエストごとに「%s」を食べる"),
                                        FoodLabel(c, eff).c_str()).c_str()
                             : nullptr,
               false);
    // 左摇杆按下：把选中的这份猫饭放进移动模式，上下键换位置（只改显示顺序，编号不动）
    if (g_reorderDir && g_listFocusId) {
        ReorderItem(m, kMoveFood, g_listFocusId, g_reorderDir);
        g_focusId = g_listFocusId;
    }
    g_listFocusId = 0;
    g_listFocusNavId = 0;
    const std::vector<int> ids = FoodRowIds(c);
    const bool moving = g_moveKind == kMoveFood && FoodAlive(c, g_moveId);
    if (!moving && g_moveKind == kMoveFood) { g_moveKind = kMoveNone; g_moveId = 0; }
    if (moving) {
        const bool up = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, true) || ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) || g_letterMove[ImGuiDir_Up];
        const bool down = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, true) || ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) || g_letterMove[ImGuiDir_Down];
        if (up || down) ReorderItem(m, g_moveKind, g_moveId, up ? -1 : 1);
        const bool put = ImGui::IsKeyPressed(ImGuiKey_GamepadL3, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) ||
                        ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
                        ImGui::IsKeyPressed(ImGuiKey_E, false);
        const bool cancel = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (put || cancel) {
            g_moveKind = kMoveNone;
            g_focusId = g_moveId;
            ImGui::SetNavCursorVisible(true);
        }
    }
    if (moving) ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    for (int fid : ids) {
        FoodPreset& f = c.foods[fid];
        ImGui::PushID(fid);
        const std::string name = FoodLabel(c, fid);
        const std::string rowId = "food" + std::to_string(fid);
        const bool isMoving = moving && g_moveId == fid;
        const float rowTop = ImGui::GetCursorScreenPos().y;   // 行被完全裁掉时 GetItemRect 不可靠，先记下行的顶
        const std::string skills = FoodSkillsSummary(f);
        const std::string detail = name + " · " + FoodSummary(f);
        const bool bad = FoodLegality(f).verdict == canteen::Verdict::Impossible;
        const bool acted = Row(rowId.c_str(), skills.c_str(), isMoving ? i18n::Tr("移动中", "moving", "移動中") : "", fid == eff, 54 * S, isMoving, detail.c_str(), bad ? "!" : nullptr);
        const ImGuiID rid = ImGui::GetItemID();
        if (ImGui::IsItemClicked()) g_mouseDownRow = rowId;
        if (isMoving) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            dl->AddRectFilled(a, b, IM_COL32(226, 194, 122, 30), 12 * S);
            dl->AddRect(a - ImVec2(2, 2) * S, b + ImVec2(2, 2) * S, kGold, 12 * S, 0, 2.0f * S);
            const char* tagTxt = i18n::Tr("移动中", "moving", "移動中");
            const ImVec2 ts = TextSize(fReg, szSmall, tagTxt);
            dl->AddText(fReg, szSmall, ImVec2(b.x - 14 * S - ts.x, std::floor(a.y + (b.y - a.y - ts.y) * 0.5f)), kGold, tagTxt);
        }
        if (!moving && acted) {
            g_mouseDownRow.clear();
            if (c.enabled && !locked) {
                const bool wasEaten = c.food == fid;
                c.food = wasEaten ? -1 : fid;
                m.reqSave = true;
                Log("ui: 猫饭 %s id=%d", wasEaten ? "取消" : "选中", fid);
            }
        }
        if (!moving && ImGui::IsItemFocused() &&
            (ImGui::IsKeyPressed(ImGuiKey_F, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)))
            OpenRename(m, kRenameFood, fid);
        // Y 始终直接编辑；套装锁定猫饭时也不改变全局备用选择。
        if (!moving && ((ImGui::IsItemFocused() &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) || ImGui::IsKeyPressed(ImGuiKey_Y, false))) ||
            (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)))) {
            g_editFood = fid;
            Go(kFoodEdit);
        }
        // 按左摇杆：进入移动模式
        if (!moving && ImGui::IsItemFocused() &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadL3, false) || ImGui::IsKeyPressed(ImGuiKey_E, false))) {
            Log("ui: 猫饭进入移动模式 id=%d", fid);
            g_moveKind = kMoveFood;
            g_moveId = fid;
        }
        if (!moving && (has ? fid == eff : ids.empty() == false && fid == ids.front())) FocusDefault();
        if (isMoving) ScrollMovingIntoView(rowTop, 54 * S);
        if (!moving && g_focusId == fid) {
            ScrollMovingIntoView(rowTop, 54 * S);
            g_focusId = 0;
            ImGui::SetFocusID(rid, ImGui::GetCurrentWindow());
            ImGui::SetNavCursorVisible(true);
        }
        if (!moving) HintListRow(fid == eff);
        if (!moving && ImGui::IsItemFocused()) {
            g_listFocusId = fid;
            g_listFocusNavId = rid;
        }
        ImGui::PopID();
        if (!moving) FollowFocusedRow();
    }
    if (moving) ImGui::PopItemFlag();
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) g_mouseDownRow.clear();
    ImGui::Dummy(ImVec2(0, 8 * S));
    const bool newFood = PillButton("##newfood", i18n::Tr("新建猫饭", "New meal", "新規猫飯"), ImGui::GetContentRegionAvail().x, 46 * S, kSecondary, kIcPlus);
    if (c.foods.empty()) FocusDefault();
    ImGui::Dummy(ImVec2(0, 2 * S));
    if (newFood) {
        const int n = c.AllocFoodId();     // 编号只增不减：删掉的猫饭编号不会再分给新的
        c.foods[n] = FoodPreset();
        c.foodOrder.push_back(n);
        g_editFood = n;
        m.reqSave = true;
        g_afterRename = 2;
        OpenRenameCreate(m, kRenameFood, n);
    }
    g_listScroll = ImGui::GetScrollY();   // 普通上下导航也要让回归测试读到真实滚动位置
}

void PageFoodEdit(Model& m)
{
    Config& c = *m.cfg;
    auto it = c.foods.find(g_editFood);
    if (it == c.foods.end() || it->second.deleted) { Go(kFood); return; }
    FoodPreset& f = it->second;
    const bool active = EffFoodId(c) == g_editFood;
    const LoadoutSet* act = c.FindSetById(c.activeSet);
    const bool locked = act && act->food > 0; // 与列表一致：套装自带猫饭时只能编辑，不能改全局选择
    const std::string title = FoodLabel(c, g_editFood);
    PageHeader(title.c_str(), !locked && active ? i18n::Tr("每次进任务都吃这个", "eaten on every quest", "クエストごとにこれを食べる") : nullptr, true);

    static const char* hp[] = {"+0", "+10", "+20", "+30", "+40", "+50"};
    const char* lv[4] = {LevelName(0), LevelName(1), LevelName(2), LevelName(3)};
    if (SegRow("hp", i18n::Tr("体力", "Health", "体力"), &f.hp, 6, hp, true)) m.reqSave = true;
    if (SegRow("at", i18n::Tr("攻击", "Attack", "攻撃"), &f.attack, 4, lv, false)) m.reqSave = true;
    if (SegRow("df", i18n::Tr("防御", "Defense", "防御"), &f.defense, 4, lv, false)) m.reqSave = true;
    if (SegRow("er", i18n::Tr("属性耐性", "Elem. resist", "属性耐性"), &f.elemRes, 4, lv, false)) m.reqSave = true;

    int sn = 0;
    const FoodSkillName* skills = gamedata::FoodSkills(sn);
    std::vector<std::string> names;
    for (int i = 0; i < sn; ++i) names.push_back(gamedata::FoodSkillNameOf(skills[i].id));
    for (int k = 0; k < 3; ++k) {
        char id[16], pid[16];
        std::snprintf(id, sizeof(id), "##sk%d", k);
        std::snprintf(pid, sizeof(pid), "pick_sk%d", k);
        const std::string label = i18n::Fmt(i18n::Tr("技能 %d", "Skill %d", "スキル %d"), k + 1);
        int cur = 0;
        for (int i = 0; i < sn; ++i) if (skills[i].id == f.skill[k]) cur = i;
        if (ValueRow(id, label.c_str(), gamedata::FoodSkillNameOf(f.skill[k]))) ImGui::OpenPopup(pid);
        const int p = PickPopup(pid, names, cur);
        if (p >= 0) { f.skill[k] = skills[p].id; m.reqSave = true; }
    }

    // 食堂做不出时才提一句：合法（含「要靠每日技能」）就什么都不显示，不拦着保存。
    if (const canteen::Result legal = FoodLegality(f); legal.verdict == canteen::Verdict::Impossible) {
        ImGui::Dummy(ImVec2(0, 10 * S));
        CenterLine(fReg, szSmall, kWarn, FoodLegalityText(legal).c_str());
    }

    // 未锁定时确认 = 每次进任务都吃这个；锁定时仅结束编辑，保留全局备用选择。
    ImGui::Dummy(ImVec2(0, 14 * S));
    const bool applies = active && !locked;   // 现在真的在吃这一份
    if (PillButton("##ok", active ? i18n::Tr("确认", "OK", "決定")
                                  : i18n::Tr("确认并吃这份", "OK, eat this one", "決定してこれを食べる"),
                   ImGui::GetContentRegionAvail().x, 54 * S, kPrimary)) {
        if (!locked) {
            c.food = g_editFood;
            m.reqSave = true;
        }
        Go(kFood);
    }
    if (applies) {
        ImGui::Dummy(ImVec2(0, 6 * S));
        if (PillButton("##unset", i18n::Tr("取消预设（保留手动猫饭）", "Clear preset (keep manual meals)", "選択解除（手動の食事は維持）"),
                       ImGui::GetContentRegionAvail().x, 46 * S, kSecondary)) {
            c.food = -1;
            m.reqSave = true;
            Go(kFood);
        }
    }
    ImGui::Dummy(ImVec2(0, 4 * S));
    if (DeleteButton(title)) {
        // 删掉的只是「列表里的这一项」：内容留着、编号也不分给别人。
        // 引用它的套装照旧吃这份（界面会标「已删除」），不会静默改成跟全局走——
        // 那样用户全局没选时就不会套用原先指定的猫饭，而是保留游戏当前状态。
        f.deleted = true;
        c.NormalizeFoodOrder();
        m.reqSave = true;
        Go(kFood);
    }
}

// ---------------------------------------------------------------------------
//  快捷键：选中一格按 A（或点一下）开始录，按下想用的键、全部松开就记住
// ---------------------------------------------------------------------------
struct Capture {
    std::string* spec = nullptr;   // 录完写进哪个快捷键（nullptr = 没在录）
    bool pad = false;              // 录手柄那一组还是键盘那一组
    bool armed = false;            // 开始录之前按着的键（比如刚按下去的 A）都松开了
    unsigned padAcc = 0;           // 这一轮按过的键，全松开时认这一组
    std::vector<int> keyAcc;
    unsigned prevPad = 0;
    double lastInput = 0;          // 太久没动静就取消
    ImRect rect;                   // 正在录的那一格（点别处 = 取消）
};
Capture g_cap;
ImGuiID g_hotkeyFocusId = 0; // 上帧快捷键格焦点；Esc 清除优先于面板返回
bool g_captureHandledEsc = false;
int g_debugFocusHotkey = -1;
std::string g_capWhy;              // 刚才那组为什么不能用（录的过程中显示）

void EndCapture()
{
    g_cap = Capture();
    g_capWhy.clear();
}

// 快捷键写法拆成手柄那组和键盘那组（各取第一组）
void SplitSpec(const std::string& spec, std::string& pad, std::string& key)
{
    pad.clear();
    key.clear();
    for (std::size_t p = 0; p <= spec.size();) {
        std::size_t bar = spec.find('|', p);
        if (bar == std::string::npos) bar = spec.size();
        std::string alt = spec.substr(p, bar - p);
        p = bar + 1;
        alt.erase(std::remove(alt.begin(), alt.end(), ' '), alt.end());
        const Hotkey h = ParseHotkey(alt);
        if (h.alts.size() != 1) continue;
        std::string& to = h.alts[0].keys.empty() ? pad : key;
        if (to.empty()) to = alt;
    }
}

std::string JoinSpec(const std::string& pad, const std::string& key)
{
    if (pad.empty()) return key;
    if (key.empty()) return pad;
    return pad + " | " + key;
}

// 把录到的（或清空的）一组写回快捷键
void SetCapturedPart(Model& m, const std::string& combo)
{
    std::string pad, key;
    SplitSpec(*g_cap.spec, pad, key);
    (g_cap.pad ? pad : key) = combo;
    const std::string next = JoinSpec(pad, key);
    // 面板键是 Dormant 状态唯一恢复入口；它不能被清成空值。
    if (g_cap.spec == &m.cfg->hkPanel && next.empty()) {
        EndCapture();
        return;
    }
    *g_cap.spec = next;
    m.reqSave = true;
    EndCapture();
}

// 每帧调一次：Esc 清除正在录的这一格；Delete / Home 是可录入的普通键。
// 手柄单按 X 仍可清除手柄组。
void UpdateCapture(Model& m)
{
    if (!g_cap.spec) return;
    const double now = ImGui::GetTime();
    const bool padB = (m.padHeld & XINPUT_GAMEPAD_B) && !(g_cap.prevPad & XINPUT_GAMEPAD_B);
    g_cap.prevPad = m.padHeld;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        SetCapturedPart(m, std::string());
        return;
    }
    if (now - g_cap.lastInput > 8.0 ||
        (ImGui::IsMouseClicked(0) && !g_cap.rect.Contains(ImGui::GetIO().MousePos)) ||
        (!g_cap.pad && padB)) {   // 录键盘时按手柄 B = 取消（录手柄时 B 本身可以是组合里的键）
        EndCapture();
        return;
    }
    const bool held = g_cap.pad ? m.padHeld != 0 : !m.keysHeld.empty();
    if (!g_cap.armed) {
        if (!held) { g_cap.armed = true; g_cap.lastInput = now; }
        return;
    }
    if (held) {
        g_cap.lastInput = now;
        if (g_cap.pad) g_cap.padAcc |= m.padHeld;
        else
            for (int vk : m.keysHeld)
                if (std::find(g_cap.keyAcc.begin(), g_cap.keyAcc.end(), vk) == g_cap.keyAcc.end()) g_cap.keyAcc.push_back(vk);
        return;
    }
    if (!g_cap.padAcc && g_cap.keyAcc.empty()) return;   // 还没按
    const bool clear = g_cap.pad && g_cap.padAcc == XINPUT_GAMEPAD_X;
    if (clear) { SetCapturedPart(m, std::string()); return; }
    std::string why;
    const std::string combo = g_cap.pad ? PadComboSpec(g_cap.padAcc, why) : KeyComboSpec(g_cap.keyAcc, why);
    g_cap.padAcc = 0;
    g_cap.keyAcc.clear();
    if (combo.empty()) { g_capWhy = why; return; }      // 不能用：说明原因，接着录
    SetCapturedPart(m, combo);
}

constexpr float kHkLabelW = 0.30f;   // 快捷键行左边标签占的宽度比例

// 一格：某个快捷键的手柄组或键盘组。按 A / 点一下开始录（录的时候再点一下取消）
void HotkeyChip(const char* id, std::string& spec, bool pad, float w, float h, Model& m, bool required = false)
{
    std::string padPart, keyPart;
    SplitSpec(spec, padPart, keyPart);
    const std::string cur = pad ? padPart : keyPart;
    BeginCustom();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    EndCustom();
    HintItem();
    if (g_debugFocusHotkey == (pad ? 0 : 1)) {
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        ImGui::FocusWindow(ImGui::GetCurrentWindow());
        ImGui::GetCurrentContext()->NavCursorVisible = true;
        g_debugFocusHotkey = -1;
    }
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool focused = Focused(), hov = ImGui::IsItemHovered();
    if (HintTarget()) {
        g_hintHotkey = true;
        g_hintHotkeyClear = !cur.empty() && (!required || !(pad ? keyPart : padPart).empty());
    }
    bool mine = g_cap.spec == &spec && g_cap.pad == pad;
    if (pressed) {
        if (mine) EndCapture();
        else if (!g_cap.spec) {
            EndCapture();
            g_cap.spec = &spec;
            g_cap.pad = pad;
            g_cap.lastInput = ImGui::GetTime();
        }
        mine = g_cap.spec == &spec && g_cap.pad == pad;
    }
    if (mine) g_cap.rect = ImRect(a, b);
    if (focused) {
        g_hotkeyFocusId = ImGui::GetItemID();
        // ImGui 在 NewFrame 先处理 Esc；提前占有，避免它先退出内容子窗口并丢失格焦点。
        ImGui::SetKeyOwner(ImGuiKey_Escape, g_hotkeyFocusId);
    }
    if (focused && !g_cap.spec && !g_captureHandledEsc && !cur.empty() &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        const std::string next = pad ? keyPart : padPart;
        if (!required || !next.empty()) {
            spec = next;
            m.reqSave = true;
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rr = 12 * S;
    std::string text;
    ImU32 col = kText;
    if (mine) {
        const float t = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 6.0f);
        dl->AddRectFilled(a, b, IM_COL32(226, 194, 122, (int)(22 + 28 * t)), rr);
        dl->AddRect(a, b, kGold, rr, 0, 2.0f * S);
        if (g_cap.armed) {
            std::vector<int> keys = g_cap.keyAcc;
            for (int vk : m.keysHeld) if (std::find(keys.begin(), keys.end(), vk) == keys.end()) keys.push_back(vk);
            text = pad ? PadComboText(g_cap.padAcc | m.padHeld) : KeyComboText(keys);
        }
        if (text.empty()) text = g_cap.armed ? i18n::Tr("请按键…", "Press a key…", "キーを押してください…")
                                            : i18n::Tr("先松开…", "Release first…", "先に離してください…");
        col = kGold;
    } else {
        dl->AddRectFilled(a, b, hov ? kSoftHot : kSoft, rr);
        if (focused) dl->AddRect(a - ImVec2(3, 3) * S, b + ImVec2(3, 3) * S, kGold, rr + 3 * S, 0, 2.0f * S);
        text = cur.empty() ? i18n::Tr("不设", "Not set", "未設定") : cur;
        col = cur.empty() ? kFaint : kText;
    }
    float fs = szBody;
    if (TextSize(fReg, fs, text.c_str()).x > w - 16 * S) fs = szSmall;   // 长组合缩小字号
    dl->PushClipRect(a, b, true);
    CenterText(dl, fReg, fs, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), col, text.c_str());
    dl->PopClipRect();
}

void HotkeyRow(const char* id, const char* label, std::string& spec, Model& m,
               bool padOk = true, bool required = false)
{
    const float w = ImGui::GetContentRegionAvail().x;
    const float rowH = 50 * S, h = 38 * S, gap = 8 * S;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(fReg, szBody, ImVec2(p.x + 14 * S, p.y + (rowH - szBody) * 0.5f), kMuted, label);
    const float x0 = p.x + w * kHkLabelW;
    const float cw = (w * (1.0f - kHkLabelW) - 6 * S - gap) * 0.5f;
    ImGui::PushID(id);
    if (padOk) {
        ImGui::SetCursorScreenPos(ImVec2(x0, p.y + (rowH - h) * 0.5f));
        HotkeyChip("##pad", spec, true, cw, h, m, required);
    } else {
        // 这一项设不了手柄键：画成不可聚焦的灰块（不建控件，手柄导航也不会停在这）
        const ImVec2 a(x0, p.y + (rowH - h) * 0.5f), b(x0 + cw, a.y + h);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, b, kSoft, 12 * S);
        CenterText(dl, fReg, szSmall, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), kFaint,
                   i18n::Tr("仅键盘", "Keyboard only", "キーボードのみ"));
    }
    ImGui::SetCursorScreenPos(ImVec2(x0 + cw + gap, p.y + (rowH - h) * 0.5f));
    HotkeyChip("##key", spec, false, cw, h, m, required);
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
    ImGui::Dummy(ImVec2(0, 0));
}

// 两列的列名（手柄 / 键盘），和 HotkeyRow 的两格对齐
void HotkeyHeader()
{
    const float w = ImGui::GetContentRegionAvail().x, gap = 8 * S;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float x0 = p.x + w * kHkLabelW;
    const float cw = (w * (1.0f - kHkLabelW) - 6 * S - gap) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    CenterText(dl, fReg, szSmall, ImVec2(x0 + cw * 0.5f, p.y + szSmall * 0.6f), kFaint, i18n::Tr("手柄", "Gamepad", "パッド"));
    CenterText(dl, fReg, szSmall, ImVec2(x0 + cw + gap + cw * 0.5f, p.y + szSmall * 0.6f), kFaint, i18n::Tr("键盘", "Keyboard", "キーボード"));
    ImGui::Dummy(ImVec2(0, szSmall * 1.3f));
}

// ---------------------------------------------------------------------------
//  原版 config.txt 的选择框。用 ImGui 自己的弹窗，不弹系统对话框：
//  游戏几乎是独占全屏的，系统对话框一抢焦点，画面就冻在那儿了。
// ---------------------------------------------------------------------------
namespace {

const char* const kPickId = "###pickconfig";

struct PickEntry {
    std::wstring name;
    bool dir = false;
};

bool g_pickOpen = false;
bool g_pickSettings = false;
std::wstring g_settingsPending;
bool g_settingsConfirmOpen = false;
int g_debugTransferFocus = 0;
ImVec2 g_settingsExportMin, g_settingsExportMax;
std::wstring g_debugPickFocus;
std::wstring g_pickDir;
std::vector<PickEntry> g_pickList;

std::string U8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((std::size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

bool IsTxt(const std::wstring& n)
{
    if (n.size() < 5) return false;
    const std::size_t at = n.size() - 4;
    if (n[at] != L'.') return false;
    wchar_t a = n[at + 1], b = n[at + 2], c = n[at + 3];
    if (a >= L'A' && a <= L'Z') a = (wchar_t)(a - L'A' + L'a');
    if (b >= L'A' && b <= L'Z') b = (wchar_t)(b - L'A' + L'a');
    if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    return a == L't' && b == L'x' && c == L't';
}

bool IsSettingsBackup(const std::wstring& name)
{
    if (name.size() < 5) return false;
    std::wstring ext = name.substr(name.size() - 4);
    for (auto& ch : ext) if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
    return ext == L".biq" || ext == L".bin";
}

std::wstring Join(const std::wstring& dir, const std::wstring& name)
{
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L"\\" + name;
}

std::wstring ParentDir(const std::wstring& d)
{
    std::wstring s = d;
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    const std::size_t p = s.find_last_of(L"\\/");
    if (p == std::wstring::npos) return s;
    if (p <= 2) return s.substr(0, 3);          // 盘符根的上一级还是它自己
    return s.substr(0, p);
}

void ScanDir(const std::wstring& dir)
{
    g_pickDir = dir;
    g_pickList.clear();
    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!std::wcscmp(fd.cFileName, L".") || !std::wcscmp(fd.cFileName, L"..")) continue;
            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!isDir && !(g_pickSettings ? IsSettingsBackup(fd.cFileName) : IsTxt(fd.cFileName))) continue;
            g_pickList.push_back({fd.cFileName, isDir});
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    std::sort(g_pickList.begin(), g_pickList.end(), [](const PickEntry& a, const PickEntry& b) {
        if (a.dir != b.dir) return a.dir;                      // 目录排前面
        return a.name < b.name;
    });
}

std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return ParentDir(buf);
}

void OpenPick(bool settings = false, const std::wstring& dir = std::wstring())
{
    g_pickSettings = settings;
    g_settingsPending.clear();
    ScanDir(dir.empty() ? ExeDir() : dir);      // 从游戏目录起步，往上两层就能到盘符
    g_pickOpen = true;
}

void PickPopup(Model& m)
{
    static bool opened = false;
    if (!g_pickOpen) { opened = false; return; }
    if (opened && !ImGui::IsPopupOpen(kPickId)) { g_pickOpen = false; opened = false; return; }  // 被 B / Esc 关掉
    if (!opened) { ImGui::OpenPopup(kPickId); opened = true; }

    // 弹窗用和面板同一套配色：ImGui 默认的蓝标题栏跟这个界面差太远
    ImGui::SetNextWindowSize(ImVec2(680 * S, 470 * S), ImGuiCond_Appearing);
    const ImVec4 cardBg = V(IM_COL32(30, 26, 21, 250));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, cardBg);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, cardBg);
    ImGui::PushStyleColor(ImGuiCol_Border, V(IM_COL32(201, 164, 92, 90)));
    ImGui::PushStyleColor(ImGuiCol_Text, V(kText));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0.45f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 18 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 18 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22 * S, 18 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    if (!ImGui::BeginPopupModal(kPickId, nullptr,
                                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoMove)) {
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(5);
        return;
    }
    Text(fReg, szBody, kTitle, g_pickSettings
        ? i18n::Tr("选择设置备份 (.biq / .bin)", "Choose settings backup (.biq / .bin)", "設定バックアップを選ぶ (.biq / .bin)")
        : i18n::Tr("选原版 IN-Q 的 config.txt", "Pick the original IN-Q config.txt", "旧 IN-Q の config.txt を選ぶ"));
    ImGui::Dummy(ImVec2(0, 4 * S));
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("%s", U8(g_pickDir).c_str());
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4 * S));

    if (PillButton("##pickup", i18n::Tr("上一级", "Up", "上へ"), 96 * S, 34 * S, kSecondary))
        ScanDir(ParentDir(g_pickDir));
    const DWORD drives = ::GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        char name[4] = {(char)('A' + i), ':', '\\', '\0'};
        char id[8] = {'#', '#', 'd', (char)('a' + i), '\0'};
        ImGui::SameLine(0, 6 * S);
        if (PillButton(id, name, 46 * S, 34 * S, kSecondary)) ScanDir(std::wstring(name, name + 3));
    }
    ImGui::Dummy(ImVec2(0, 6 * S));

    const float listH = ImGui::GetContentRegionAvail().y - 54 * S;   // 给底下的取消键留位置
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {   // Cancel only this modal.
        g_pickOpen = false;
        opened = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(5);
        return;
    }
    ImGui::BeginChild("##list", ImVec2(0, listH > 80 * S ? listH : 80 * S), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_None);
    if (g_pickList.empty()) {
        ImGui::Dummy(ImVec2(0, 6 * S));
        ImGui::PushStyleColor(ImGuiCol_Text, V(kMuted));
        Text(fReg, szSmall, kMuted, g_pickSettings
            ? i18n::Tr("此处没有备份；可用上一级或盘符浏览其他目录", "No backups; browse Up or pick a drive", "バックアップなし。「上へ」やドライブで移動できます")
            : i18n::Tr("这层没有文件夹或 config.txt，点上面的盘符换个地方找",
                       "No folders or config.txt; try a drive above", "ここにフォルダも config.txt もありません"), true);
        ImGui::PopStyleColor();
    }
    for (const PickEntry& e : g_pickList) {
        std::string label = e.dir ? (std::string("> ") + U8(e.name)) : U8(e.name);
        label += "##";
        label += U8(e.name);
        const bool chosen = ImGui::Selectable(label.c_str(), false, 0, ImVec2(0, 30 * S));
        FollowFocusedRow();
        if (g_debugPickFocus == e.name) { g_debugPickFocus.clear(); FocusItem(); ImGui::SetScrollHereY(); }
        if (chosen) {
            if (e.dir) {
                ScanDir(Join(g_pickDir, e.name));
            } else {
                if (g_pickSettings) g_settingsPending = Join(g_pickDir, e.name);
                else {
                    m.inqPath = Join(g_pickDir, e.name);
                    m.reqImportInq = true;
                }
                g_pickOpen = false;
                opened = false;
                ImGui::CloseCurrentPopup();
            }
            break; // ScanDir replaces g_pickList; never advance an invalidated iterator.
        }
    }
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, 6 * S));
    if (PillButton("##pickcancel", i18n::Tr("取消", "Cancel", "キャンセル"), 130 * S, 40 * S, kSecondary)) {
        g_pickOpen = false;
        opened = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(5);
}

// The picker only stages a path. No transfer request exists before confirmation.

void SettingsImportPopup(Model& m)
{
    const char* id = "###settings_import_confirm";
    if (g_settingsPending.empty()) { g_settingsConfirmOpen = false; return; }
    if (!g_settingsConfirmOpen) {
        ImGui::OpenPopup(id);
        g_settingsConfirmOpen = true;
    } else if (!ImGui::IsPopupOpen(id)) {
        g_settingsPending.clear();
        g_settingsConfirmOpen = false;
        return;
    }
    ImGui::SetNextWindowPos((g_panelMin + g_panelMax) * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500 * S, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26 * S, 24 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 20 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, S);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, V(IM_COL32(30, 26, 21, 250)));
    ImGui::PushStyleColor(ImGuiCol_Border, V(IM_COL32(201, 164, 92, 90)));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.02f, 0.015f, 0.01f, 0.5f));
    const bool open = ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
    if (!open) return;
    Text(fBold, szBody * 1.15f, kTitle, i18n::Tr("确定导入吗？", "Import settings?", "取り込みますか？"));
    ImGui::Dummy(ImVec2(0, 12 * S));
    Text(fReg, szBody, kWarn, i18n::Tr(
        "将覆盖全局设置及全部 3 个存档位的套装、猫饭。",
        "Replaces globals and all 3 slots.",
        "全体設定と全3スロットのセット・猫飯を上書きします。"), true);
    ImGui::Dummy(ImVec2(0, 20 * S));
    const float gap = 12 * S, bw = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
    const bool cancel = PillButton("##settingscancel", i18n::Tr("取消", "Cancel", "キャンセル"), bw, 44 * S, kSecondary);
    if (ImGui::IsWindowAppearing()) FocusItem();
    ImGui::SameLine(0, gap);
    const bool confirm = PillButton("##settingsconfirm", i18n::Tr("导入", "Import", "取込"), bw, 44 * S, kDangerSolid);
    const bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    const bool yes = confirm && !cancel && !back && !ImGui::IsWindowAppearing();
    if (yes) { m.settingsPath = g_settingsPending; m.reqImportSettings = true; }
    if (yes || cancel || back) {
        g_settingsPending.clear();
        g_settingsConfirmOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace

// ---------------------------------------------------------------------------
//  改名：X（手柄）/ F（键盘）弹出输入框，确定后写回配置（套装名字就是 ini 的段名）
// ---------------------------------------------------------------------------
void RenamePopup(Model& m)
{
    if (g_renameKind == kRenameNone) {
        g_renameOpen = false;
        return;
    }
    Config& c = *m.cfg;
    static const char* kId = "###rename";
    if (!g_renameOpen) {
        ImGui::OpenPopup(kId);
        g_renameOpen = true;
    } else if (!ImGui::IsPopupOpen(kId)) {
        g_renameKind = kRenameNone;
        g_renameOpen = false;
        g_renameErr.clear();
        g_renameCreate = false;
        const int after = g_afterRename;
        g_afterRename = 0;
        if (after == 1) Go(kSetEdit);
        else if (after == 2) Go(kFoodEdit);
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(460 * S, 0), ImGuiCond_Appearing);
    const ImVec4 cardBg = V(IM_COL32(30, 26, 21, 250));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, cardBg);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, cardBg);
    ImGui::PushStyleColor(ImGuiCol_Border, V(IM_COL32(201, 164, 92, 90)));
    ImGui::PushStyleColor(ImGuiCol_Text, V(kText));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0.45f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 18 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 18 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22 * S, 18 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    if (!ImGui::BeginPopupModal(kId, nullptr,
                                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(5);
        return;
    }

    auto close = [&]() {
        g_renameKind = kRenameNone;
        g_renameOpen = false;
        g_renameErr.clear();
        g_renameCreate = false;
        const int after = g_afterRename;
        g_afterRename = 0;
        ImGui::CloseCurrentPopup();
        if (after == 1) Go(kSetEdit);        // 新建那套 / 那份猫饭，起完名直接进编辑页
        else if (after == 2) Go(kFoodEdit);
    };

    if (g_renameCloseReq) {
        g_renameCloseReq = false;
        close();
        ImGui::EndPopup();
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(5);
        return;
    }

    const bool isSet = g_renameKind == kRenameSet;
    const char* title = g_renameCreate
        ? (isSet ? i18n::Tr("新建套装", "New loadout", "新規セット")
                 : i18n::Tr("新建猫饭", "New meal", "新規猫飯"))
        : (isSet ? i18n::Tr("重命名套装", "Rename loadout", "セット名を変更")
                 : i18n::Tr("重命名猫饭", "Rename meal", "猫飯名を変更"));
    Text(fReg, szBody, kTitle, title);
    ImGui::Dummy(ImVec2(0, 6 * S));
    if (g_renameFocus) {
        ImGui::SetKeyboardFocusHere();
        g_renameFocus = false;
    }
    // 名字长的时候输入框跟着变宽（到上限为止），不挤在一个固定宽度里
    float want = ImGui::CalcTextSize(g_renameBuf).x + 120 * S;
    if (want < 420 * S) want = 420 * S;
    if (want > 760 * S) want = 760 * S;
    ImGui::SetNextItemWidth(want);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, V(IM_COL32(48, 42, 34, 255)));
    ImGui::PushStyleColor(ImGuiCol_Text, V(kText));
    std::string hint;
    if (g_renameCreate) {
        hint = isSet ? c.sets[g_renameId].name
                     : i18n::Fmt(i18n::Tr("猫饭 %d", "Meal %d", "猫飯 %d"), g_renameId);
    }
    const bool enter = ImGui::InputTextWithHint("##name", hint.c_str(), g_renameBuf, sizeof(g_renameBuf),
                                                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackEdit,
                                                RenameInputCallback);
    ImGui::PopStyleColor(2);
    // 输入框外面右下角：还能输几个字
    const int used = CountChars(g_renameBuf);
    const std::string cnt = i18n::Fmt("%d/%d", used, kMaxNameChars);
    const float cw = ImGui::CalcTextSize(cnt.c_str()).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - cw);
    Text(fReg, szSmall, used >= kMaxNameChars ? IM_COL32(214, 122, 92, 255) : kMuted, cnt.c_str());
    ImGui::Dummy(ImVec2(0, 2 * S));
    if (!g_renameErr.empty()) Text(fReg, szSmall, IM_COL32(214, 122, 92, 255), g_renameErr.c_str());
    ImGui::Dummy(ImVec2(0, 8 * S));

    bool ok = enter || PillButton("##rnok", i18n::Tr("确定", "OK", "決定"), 150 * S, 44 * S, kPrimary);
    ImGui::SameLine(0, 10 * S);
    if (PillButton("##rncancel", i18n::Tr("取消", "Cancel", "キャンセル"), 150 * S, 44 * S, kSecondary) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        close();
        ImGui::EndPopup();
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(5);
        return;
    }
    if (ok) {
        const std::string name = TrimName(g_renameBuf);
        if (name.empty() && !g_renameCreate) {
            g_renameErr = i18n::Tr("名字不能为空", "name cannot be empty", "名前を入れてください");
        } else if (isSet) {
            // 名字只是显示用的，重名没关系；套用靠的是编号。新建时留空 = 用默认名
            if (!name.empty()) c.sets[g_renameId].name = name;
            m.reqSave = true;
            close();
        } else {
            auto it = c.foods.find(g_renameId);
            if (it != c.foods.end() && !name.empty()) it->second.name = name;
            m.reqSave = true;
            close();
        }
    }
    ImGui::EndPopup();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(5);
}

void PageSettings(Model& m)
{
    Config& c = *m.cfg;
    PageHeader(i18n::Tr("设置", "Settings", "設定"), nullptr, false);
    // 装载态 / 卸载态：卸载态暂停 IN-Q 重开/套装/猫饭逻辑，只保留面板。
    // 启动时打入的兼容修复/转场常量补丁不动态回写，避免覆盖后装模组。
    Text(fReg, szSmall, kFaint, i18n::Tr("插件状态", "Plugin state", "プラグインの状態"));
    ImGui::Dummy(ImVec2(0, 2 * S));
    if (RadioRow("##st0", i18n::Tr("装载态", "Loaded", "使用中"),
                 i18n::Tr("功能正常生效", "everything on", "機能あり"), c.enabled)) {
        if (!c.enabled) { c.enabled = true; m.reqSave = true; }
    }
    if (RadioRow("##st1", i18n::Tr("卸载态", "Unloaded", "停止中"),
                 i18n::Tr("暂停重开/套装/猫饭，只保留面板", "pause features, keep panel", "再開・セット・食事を停止、パネルのみ"), !c.enabled)) {
        if (c.enabled) { c.enabled = false; m.reqSave = true; }
    }
    if (g_focusState) { g_focusState = false; FocusItem(); }
    ImGui::Dummy(ImVec2(0, 10 * S));


    Text(fReg, szSmall, kFaint, i18n::Tr("语言", "Language", "言語"));
    ImGui::Dummy(ImVec2(0, 2 * S));
    {
        // 语言名不翻译：界面读不懂的时候就靠这行找回来
        static const char* const kNames[3] = {"中文", "English", "日本語"};
        static const char* const kCodes[3] = {"zh", "en", "ja"};
        const int cur = i18n::Current();
        const float lw = (ImGui::GetContentRegionAvail().x - 2 * 8 * S) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine(0, 8 * S);
            const bool on = cur == i;
            if (PillButton(i ? (std::string("##lg") + std::to_string(i)).c_str() : "##lg0", kNames[i], lw, 40 * S,
                           on ? kPrimary : kSecondary)) {
                c.language = kCodes[i];
                i18n::Set(i18n::FromString(c.language));
                m.reqSave = true;
            }
        }
    }
    ImGui::Dummy(ImVec2(0, 10 * S));

    Text(fReg, szSmall, kFaint, i18n::Tr("重开方式", "Restart mode", "再開方法"));
    ImGui::Dummy(ImVec2(0, 2 * S));
    const int mode = c.acceptOnly ? 2 : (c.reset ? 1 : 0);
    if (RadioRow("##m0", i18n::Tr("直接重开", "Restart directly", "直接再開"), nullptr, mode == 0)) { c.reset = false; c.acceptOnly = false; m.reqSave = true; }
    if (mode == 0) FocusDefault();
    if (RadioRow("##m1", i18n::Tr("重置后回据点再接", "Reset, go to base, accept again", "リセットして拠点で再受注"), nullptr, mode == 1)) { c.reset = true; c.acceptOnly = false; m.reqSave = true; }
    if (mode == 1) FocusDefault();
    if (RadioRow("##m2", i18n::Tr("只接任务，不出发", "Accept only, do not depart", "受注のみ、出発しない"), nullptr, mode == 2)) { c.reset = false; c.acceptOnly = true; m.reqSave = true; }
    if (mode == 2) FocusDefault();
    ImGui::Dummy(ImVec2(0, 10 * S));

    if (ToggleRow("##wy", i18n::Tr("翼龙起始", "Start with the wingdrake", "翼竜で開始"),
                  i18n::Tr("普通地图坐翼龙开局，关闭即为在营地开局",
                           "start on the wingdrake; off = camp",
                           "通常マップは翼竜で開始。オフなら拠点から開始"),
                  &c.wyvern)) m.reqSave = true;
    if (ToggleRow("##bo", i18n::Tr("缩短转场黑屏", "Shorten transition fades", "遷移の暗転を短縮"),
                  i18n::Tr("下次开游戏生效", "takes effect on the next launch", "次回起動時に反映"),
                  &c.quickBlackout)) m.reqSave = true;
    ImGui::Dummy(ImVec2(0, 10 * S));

    if (ToggleRow("##palico_enabled", i18n::Tr("随从猫功能", "Palico features", "オトモ機能"),
                  i18n::Tr("默认关闭；开启后可在套装中设置随从猫", "Off by default; enable in loadouts", "初期はオフ。オンでセットのオトモ設定を表示"),
                  &c.palicoEnabled)) m.reqSave = true;
    ImGui::Dummy(ImVec2(0, 10 * S));

    Text(fReg, szSmall, kFaint, i18n::Tr("面板", "Panel", "パネル"));
    ImGui::Dummy(ImVec2(0, 2 * S));
    {
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddText(fReg, szBody, ImVec2(p.x + 14 * S, p.y + 10 * S), kMuted,
                                            i18n::Tr("不透明度", "Opacity", "不透明度"));
        ImGui::SetCursorScreenPos(ImVec2(p.x + w * 0.42f, p.y + 4 * S));
        ImGui::SetNextItemWidth(w * 0.58f - 6 * S);
        ImGui::PushFont(fReg, szBody);
        if (ImGui::SliderInt("##op", &c.panelOpacity, 30, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput))
            m.reqSave = true;
        if (HintTarget() || ImGui::IsItemActive()) g_hintSlider = true;
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 46 * S));
        ImGui::Dummy(ImVec2(0, 0));
    }
    ImGui::Dummy(ImVec2(0, 10 * S));

    Text(fReg, szSmall, kFaint, i18n::Tr("快捷键", "Hotkeys", "ホットキー"));
    ImGui::Dummy(ImVec2(0, 2 * S));
    HotkeyHeader();
    HotkeyRow("##hk1", i18n::Tr("重开", "Restart", "再開"), c.hkRestart, m);
    HotkeyRow("##hk3", i18n::Tr("打开面板", "Open panel", "パネルを開く"), c.hkPanel, m, true, true);
    HotkeyRow("##hk4", i18n::Tr("只重置任务", "Reset only", "リセットのみ"), c.hkResetOnly, m);
    if (ToggleRow("##itemauto", i18n::Tr("进任务自动套用道具", "Auto-apply items on quest entry", "クエスト開始時にアイテムを自動適用"),
                  i18n::Tr("同时跳过“是否切换快捷栏”的确认；开关立即生效，从下一次套用开始",
                           "Also skips the shortcut-bar prompt; takes effect immediately for the next apply",
                           "ショートカットバーの確認も省略。次回の適用からすぐ反映"),
                  &c.itemAutoApply))
        m.reqSave = true;
    if (ToggleRow("##refillon", i18n::Tr("允许补满道具快捷键", "Allow the refill hotkey", "補充ホットキーを有効化"),
                  i18n::Tr("默认关闭，防止救援时误触；关闭后手动补全不会执行",
                           "Off by default; manual refill does nothing while off",
                           "既定は無効。オフのとき手動補充は実行されません"),
                  &c.refillHotkey))
        m.reqSave = true;
    HotkeyRow("##hk5", i18n::Tr("补满道具数量", "Refill items", "アイテム数量を補充"), c.hkRefill, m);
    ImGui::Dummy(ImVec2(0, 4 * S));
    const char* hint = !g_cap.spec
        ? i18n::Tr("选中一格按 A 或点一下，再按想用的键",
                   "select a cell, then press the keys",
                   "セルを選んで（A かクリック）使うキーを押す")
        : g_cap.pad
              ? i18n::Tr("按住想用的手柄键（至少两个），全部松开就记住 · Esc 清除此格",
                         "hold 2+ buttons, release to save · Esc clears cell",
                         "使うボタンを押し続け（2 つ以上）、全部離すと記録 · Esc でこのセルを解除")
              : i18n::Tr("按下想用的键，全部松开就记住 · Esc 清除此格",
                         "press keys, release to save · Esc clears cell",
                         "使うキーを押し、全部離すと記録 · Esc でこのセルを解除");
    Text(fReg, szSmall, g_capWhy.empty() ? kFaint : kWarn, g_capWhy.empty() ? hint : g_capWhy.c_str(), true);
    Text(fReg, szSmall, kFaint, i18n::Tr("不设：选中快捷键格后按 Esc；手柄那格也可单按 X",
                                         "to clear: focus a cell and press Esc (X clears pad)",
                                         "未設定にする：セルを選んで Esc（パッドのセルは X でも解除）"),
         true);
    ImGui::Dummy(ImVec2(0, 14 * S));

    Text(fReg, szSmall, kFaint, i18n::Tr("存档位", "Save slot", "セーブスロット"));
    ImGui::Dummy(ImVec2(0, 2 * S));
    {
        const int slots[3] = {1, 2, 3};
        const float gap = 10 * S;
        const float bw = (ImGui::GetContentRegionAvail().x - gap * 2) / 3;
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine(0, gap);
            const std::string id = "##slot" + std::to_string(slots[i]);
            const std::string label = std::to_string(slots[i]);
            const bool on = m.slot == slots[i];
            if (PillButton(id.c_str(), label.c_str(), bw, 44 * S, on ? kPrimary : kSecondary) && !on) {
                m.slot = slots[i];
                m.reqSlot = true;
            }
        }
    }
    ImGui::Dummy(ImVec2(0, 2 * S));
    Text(fReg, szSmall, kFaint,
         i18n::Tr("游戏里换存档之后来这里切一下；套装和猫饭按存档分开存",
                  "switch here after changing save slot; per-slot data",
                  "ゲームでスロットを変えたらここで切り替え。セットはスロットごとに保存されます"),
         true);
    ImGui::Dummy(ImVec2(0, 14 * S));

    Text(fReg, szSmall, kFaint, i18n::Tr("设置备份", "Settings backup", "設定バックアップ"));
    ImGui::Dummy(ImVec2(0, 2 * S));
    const float transferGap = 10 * S;
    const float transferW = (ImGui::GetContentRegionAvail().x - transferGap) * 0.5f;
    if (PillButton("##exportsettings", i18n::Tr("导出备份", "Export backup", "バックアップ出力"), transferW, 44 * S, kSecondary))
        m.reqExportSettings = true;
    g_settingsExportMin = ImGui::GetItemRectMin();
    g_settingsExportMax = ImGui::GetItemRectMax();
    if (g_debugTransferFocus == 1) { g_debugTransferFocus = 0; FocusItem(); ImGui::SetScrollHereY(); }
    ImGui::SameLine(0, transferGap);
    if (PillButton("##importsettings", i18n::Tr("导入备份", "Import backup", "バックアップ取込"), transferW, 44 * S, kSecondary))
        OpenPick(true, m.backupsDir);
    if (g_debugTransferFocus == 2) { g_debugTransferFocus = 0; FocusItem(); ImGui::SetScrollHereY(); }
    ImGui::Dummy(ImVec2(0, 14 * S));

    Text(fReg, szSmall, kFaint,
         i18n::Tr("从原版 IN-Q 导入", "Import from the original IN-Q", "旧 IN-Q から取り込む"));
    ImGui::Dummy(ImVec2(0, 2 * S));
    if (PillButton("##impinq", i18n::Tr("选择 config.txt…", "Choose config.txt…", "config.txt を選ぶ…"),
                   ImGui::GetContentRegionAvail().x, 44 * S, kSecondary))
        OpenPick();
    ImGui::Dummy(ImVec2(0, 2 * S));
    Text(fReg, szSmall, kFaint,
         i18n::Tr("挑原版 IN-Q 文件夹里的 config.txt，里面的全套会整套搬过来",
                  "pick the config.txt in the old IN-Q folder to copy its loadouts",
                  "旧 IN-Q フォルダの config.txt を選ぶと、セットをまとめて取り込みます"),
         true);
    ImGui::Dummy(ImVec2(0, 14 * S));

    ClearAllButton(m);

    ImGui::Dummy(ImVec2(0, 14 * S));

    Text(fReg, szSmall, kFaint, ("Better-IN-Q " + m.version).c_str());
    Text(fReg, szSmall, kFaint, i18n::Tr("原作者 年糕狗三世", "Original author: 年糕狗三世", "原作者：年糕狗三世"), true);
}

std::string OperationHints(const Model& m)
{
    const bool pad = g_hintSource.source == hints::Source::Gamepad;
    const bool mouse = g_hintSource.source == hints::Source::Mouse;
    std::string text = pad ? i18n::Tr("手柄：", "Gamepad: ", "パッド：")
        : mouse ? i18n::Tr("鼠标：", "Mouse: ", "マウス：") : i18n::Tr("键盘：", "Keyboard: ", "キーボード：");
    auto add = [&](const char* value) { if (text.back() != ' ' && text.back() != ':') text += "  ·  "; text += value; };
    if (g_cap.spec) {
        add(g_cap.pad ? i18n::Tr("按手柄组合后松开以保存", "Press pad combo, release to save", "パッドの組合せを押して離すと保存")
                      : i18n::Tr("按键盘组合后松开以保存", "Press key combo, release to save", "キーの組合せを押して離すと保存"));
        add(i18n::Tr("Esc 清除当前组", "Esc clear group", "Esc 現在の組を消去"));
        if (g_cap.pad) add(i18n::Tr("单按 X 清除", "X alone clears", "X 単独で消去"));
        else if (pad) add(i18n::Tr("B 取消录制", "B cancel capture", "B 記録を中止"));
        if (mouse) add(i18n::Tr("点击格外取消", "Click outside chip to cancel", "枠外クリックで中止"));
        return text;
    }
    if (g_renameKind != kRenameNone) {
        add(i18n::Tr("键盘输入名字", "Type name with keyboard", "キーボードで名前入力"));
        add(pad ? i18n::Tr("A 选择按钮 · B 取消", "A activate button · B cancel", "A ボタン決定 · B 中止")
                : mouse ? i18n::Tr("点击确定 / 取消", "Click OK / Cancel", "決定 / 中止をクリック")
                        : i18n::Tr("Enter 确定 · Esc 取消", "Enter confirm · Esc cancel", "Enter 決定 · Esc 中止"));
        return text;
    }
    if (g_settingsConfirmOpen || g_pickOpen) {
        add(g_settingsConfirmOpen
            ? i18n::Tr("选择取消或覆盖导入", "Cancel or Replace?", "中止か上書き取込を選択")
            : i18n::Tr("浏览目录并选择文件", "Browse folders and choose a file", "フォルダを移動してファイルを選択"));
        add(pad ? i18n::Tr("方向键选择 · A 确认 · B 取消", "D-pad navigate · A ok · B back", "方向キー 選択 · A 決定 · B 中止")
            : mouse ? i18n::Tr("点击选项 · Esc 取消", "Click option · Esc cancel", "項目クリック · Esc 中止")
                    : i18n::Tr("WASD 选择 · Enter / 空格 确认 · Esc 取消", "WASD navigate · Enter activate · Esc cancel", "WASD 選択 · Enter / Space 決定 · Esc 中止"));
        return text;
    }
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        add(pad ? i18n::Tr("方向键选择 · A 确认 · B 关闭弹窗", "D-pad navigate · A ok · B close", "方向キー 選択 · A 決定 · B 閉じる")
                : mouse ? i18n::Tr("点击弹窗选项 / 按钮", "Click popup option / button", "ポップアップの項目 / ボタンをクリック")
                        : i18n::Tr("WASD 选择 · Enter / 空格 确认 · Esc 关闭弹窗", "WASD navigate · Enter activate · Esc close", "WASD 選択 · Enter / Space 決定 · Esc 閉じる"));
        return text;
    }
    if (g_moveKind != kMoveNone) {
        add(pad ? i18n::Tr("↑ / ↓ 移动 · L3 / A 放下 · B 结束移动", "Up/Down move · L3 drop · B done", "↑ / ↓ 移動 · L3 / A 置く · B 移動終了")
                : mouse ? i18n::Tr("移动模式需键盘：↑ / ↓ 移动 · E / Enter / 空格 放下 · Esc 结束移动", "Move mode needs keyboard: Up/Down move · E drop · Esc done", "移動モードはキーボード操作：↑ / ↓ 移動 · E / Enter / Space 置く · Esc 移動終了")
                        : i18n::Tr("↑ / ↓ 移动 · E / Enter / 空格 放下 · Esc 结束移动", "Up/Down move · E drop · Esc done", "↑ / ↓ 移動 · E / Enter / Space 置く · Esc 移動終了"));
        return text;
    }
    const bool typing = GImGui->ActiveId && GImGui->InputTextState.ID == GImGui->ActiveId;
    if (typing) {
        add(pad ? i18n::Tr("键盘输入文字 · B 结束输入", "Type · B done", "キーボードで文字入力 · B 入力終了")
                : i18n::Tr("键盘输入文字 · Esc 结束输入", "Type · Esc done", "キーボードで文字入力 · Esc 入力終了"));
        if (mouse) add(i18n::Tr("点击定位光标", "Click to place cursor", "クリックでカーソル移動"));
        return text;
    }
    if (g_hintListRow && (g_page == kSets || g_page == kFood)) {
        const LoadoutSet* active = m.cfg->FindSetById(m.cfg->activeSet);
        const bool canApply = m.cfg->enabled && (g_page != kFood || !active || active->food <= 0);
        if (canApply) add(pad ? i18n::Tr("A 套用 / 取消", "A apply / unapply", "A 適用 / 解除")
                            : mouse ? i18n::Tr("左键套用 / 取消", "Left-click apply / unapply", "左クリック 適用 / 解除")
                                    : i18n::Tr("Enter / 空格 套用 / 取消", "Enter / Space apply / unapply", "Enter / Space 適用 / 解除"));
        add(pad ? i18n::Tr("Y 编辑 · X 改名 · L3 移动", "Y edit · X rename · L3 move", "Y 編集 · X 名前変更 · L3 移動")
                : mouse ? i18n::Tr("右键编辑", "Right-click edit", "右クリック 編集")
                        : i18n::Tr("Y 编辑 · F 改名 · E 移动", "Y edit · F rename · E move", "Y 編集 · F 名前変更 · E 移動"));
        if (!mouse) add(pad ? i18n::Tr("LB / RB 排序（长按连移）", "LB / RB reorder (hold to repeat)", "LB / RB 並べ替え（長押し連続）")
                           : i18n::Tr("Ctrl + ↑ / ↓ 排序（长按连移）", "Ctrl+Up/Down reorder (hold to repeat)", "Ctrl + ↑ / ↓ 並べ替え（長押し連続）"));
    } else if (g_hintHotkey) {
        add(pad ? i18n::Tr("A 录制快捷键", "A capture hotkey", "A ショートカット記録")
                : mouse ? i18n::Tr("点击录制快捷键", "Click to capture hotkey", "クリックでショートカット記録")
                        : i18n::Tr("Enter / 空格 录制快捷键", "Enter / Space capture hotkey", "Enter / Space ショートカット記録"));
        if (g_hintHotkeyClear && !mouse) add(pad ? i18n::Tr("X 清除当前组", "X clear group", "X 現在の組を消去") : i18n::Tr("Esc 清除当前组", "Esc clear group", "Esc 現在の組を消去"));
    } else if (g_hintNumber) {
        add(pad ? (g_numberEdit ? i18n::Tr("↑ +1 · ↓ -1 · → +10 · ← -10 · B 结束调整", "Up +1 · Down -1 · Right +10 · Left -10 · B done", "↑ +1 · ↓ -1 · → +10 · ← -10 · B 調整終了")
                                            : i18n::Tr("A 开始调整等级", "A adjust level", "A レベル調整"))
                : mouse ? i18n::Tr("点击上下箭头 +1 / -1；Esc 结束调整", "Click chevrons ±1; Esc to finish", "上下矢印クリック +1 / -1；Esc 調整終了")
                        : (g_numberEdit ? i18n::Tr("↑/W +1 · ↓/S -1 · →/D +10 · ←/A -10 · Esc 结束调整", "Up/W +1 · Down/S -1 · Right/D +10 · Left/A -10 · Esc done", "↑/W +1 · ↓/S -1 · →/D +10 · ←/A -10 · Esc 調整終了")
                                        : i18n::Tr("Enter / 空格 开始调整等级", "Enter / Space adjust level", "Enter / Space レベル調整")));
        if (g_numberEdit) return text;
    } else if (g_hintSlider) {
        add(pad ? i18n::Tr("A 开始 / 结束调整 · ← / → 调整", "A starts · Left/Right adjust", "A 調整開始 / 終了 · ← / → 調整")
                : mouse ? i18n::Tr("点击 / 拖动调整不透明度", "Click / drag to adjust opacity", "クリック / ドラッグで不透明度調整")
                        : i18n::Tr("Enter / 空格 开始调整 · ← / → 调整", "Enter starts · Left/Right adjust", "Enter / Space 調整開始 · ← / → 調整"));
    } else if (g_hintTextField) {
        add(pad ? i18n::Tr("A 开始输入（需键盘）", "A start input (keyboard required)", "A 入力開始（キーボードが必要）")
                : mouse ? i18n::Tr("点击输入（需键盘）", "Click to type (keyboard required)", "クリックで入力（キーボードが必要）")
                        : i18n::Tr("Enter 开始输入", "Enter start input", "Enter 入力開始"));
    } else if (g_hintAction) {
        add(pad ? i18n::Tr("A 确认", "A activate", "A 決定")
                : mouse ? i18n::Tr("左键选择 / 操作", "Left-click select / activate", "左クリック 選択 / 決定")
                        : i18n::Tr("Enter / 空格 确认", "Enter / Space activate", "Enter / Space 決定"));
    }
    add(pad ? (g_sideFocusedLast ? i18n::Tr("↑ / ↓ 切页 · → 进入页面", "Up/Down switch page · Right enter", "↑ / ↓ ページ切替 · → ページへ")
                                                  : i18n::Tr("方向键导航", "D-pad navigate", "方向キー 移動"))
            : mouse ? i18n::Tr("点击侧栏切页 · 滚轮滚动", "Click sidebar or wheel to switch page", "サイドバーでページ切替 · ホイール スクロール")
                    : i18n::Tr("WASD 导航", "WASD navigate", "WASD 移動"));
    if (!(g_hintHotkey && !pad && !mouse))
        add(pad ? (g_page == kHome ? i18n::Tr("B 关闭面板", "B close panel", "B パネルを閉じる") : i18n::Tr("B 返回", "B back", "B 戻る"))
                : mouse ? i18n::Tr("Esc 返回 / 关闭", "Esc back / close", "Esc 戻る / 閉じる")
                        : (g_page == kHome ? i18n::Tr("Esc 关闭面板", "Esc close panel", "Esc パネルを閉じる") : i18n::Tr("Esc 返回", "Esc back", "Esc 戻る")));
    return text;
}

// Display groups deliberately use short labels; OperationHints above remains the
// verbose, stable diagnostic description. Each key/action pair is indivisible.
struct HintGroup { std::string key, action; bool right = false; int priority = 0; };
std::vector<HintGroup> CompactHints(const Model& m)
{
    const bool pad = g_hintSource.source == hints::Source::Gamepad;
    const bool mouse = g_hintSource.source == hints::Source::Mouse;
    std::vector<HintGroup> groups;
    auto add = [&](const char* key, const char* zh, const char* en, const char* ja, bool right = false, int priority = 0) {
        groups.push_back({key, i18n::Tr(zh, en, ja), right, priority});
    };
    auto navigate = [&]() {
        add(pad ? "D-pad" : mouse ? "Wheel" : "WASD", mouse ? "滚动" : "导航",
            mouse ? "Scroll" : "Nav", mouse ? "スクロール" : "移動", true);
    };
    auto back = [&](const char* zh, const char* en, const char* ja) {
        add(pad ? "B" : "Esc", zh, en, ja, true);
    };
    if (g_cap.spec) {
        add(g_cap.pad ? "Pad" : "Keys", "松开保存", "Release saves", "離して保存");
        if (g_cap.pad) add("X", "单按清除", "Alone: clear", "単独で消去");
        add("Esc", "清除当前组", "Clear group", "組を消去", true);
        if (!g_cap.pad && pad) back("取消", "Cancel", "中止");
        if (mouse) add("Click", "格外取消", "Outside: cancel", "枠外で中止", true);
        return groups;
    }
    if (g_renameKind != kRenameNone) {
        add("Keyboard", "输入名字", "Type name", "名前入力");
        add(pad ? "A" : mouse ? "Click" : "Enter", "确定", "Confirm", "決定");
        back("取消", "Cancel", "中止");
        return groups;
    }
    if (g_settingsConfirmOpen || g_pickOpen) {
        if (g_settingsConfirmOpen) add(pad ? "A" : mouse ? "Click" : "Enter", "选择操作", "Choose action", "操作選択");
        else add(pad ? "A" : mouse ? "Click" : "Enter", "选择文件", "Pick file", "ファイル選択");
        if (!mouse) navigate();
        back("取消", "Cancel", "中止");
        return groups;
    }
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        add(pad ? "A" : mouse ? "Click" : "Enter", "确认", "Confirm", "決定");
        if (!mouse) navigate();
        back("关闭", "Close", "閉じる");
        return groups;
    }
    if (g_moveKind != kMoveNone) {
        add(pad ? "L3/A" : "Enter", "放下", "Drop", "置く");
        add(pad ? "D-pad" : "↑/↓", "移动", "Move", "移動", true);
        back("结束", "Finish", "終了");
        return groups;
    }
    const bool typing = GImGui->ActiveId && GImGui->InputTextState.ID == GImGui->ActiveId;
    if (typing) {
        add("Keyboard", "输入文字", "Type text", "文字入力");
        back("结束输入", "Finish input", "入力終了");
        return groups;
    }
    if (g_hintListRow && (g_page == kSets || g_page == kFood)) {
        const LoadoutSet* active = m.cfg->FindSetById(m.cfg->activeSet);
        const bool canApply = m.cfg->enabled && (g_page != kFood || !active || active->food <= 0);
        if (canApply) add(pad ? "A" : mouse ? "L-click" : "Enter", g_hintListSelected ? "取消" : "套用",
            g_hintListSelected ? "Off" : "Apply", g_hintListSelected ? "解除" : "適用");
        add(pad ? "Y" : mouse ? "R-click" : "Y", "编辑", "Edit", "編集");
        if (!mouse || g_hintListFocused) {
            add(pad ? "X" : "F", "改名", "Rename", "改名");
            add(pad ? "L3" : "E", "移动", "Move", "移動", false, 1);
            add(pad ? "LB/RB" : "Ctrl+↑/↓", "换位", "Shift", "並替", false, 2);
        }
    } else if (g_hintHotkey) {
        add(pad ? "A" : mouse ? "Click" : "Enter", "录制", "Capture", "記録");
        if (g_hintHotkeyClear && !mouse) add(pad ? "X" : "Esc", "清除", "Clear", "消去");
    } else if (g_hintNumber) {
        if (g_numberEdit) {
            add("↑/↓", "+1 / -1", "+1 / -1", "+1 / -1");
            add("←/→", "-10 / +10", "-10 / +10", "-10 / +10");
            back("结束调整", "Finish adjust", "調整終了");
            return groups;
        }
        add(pad ? "A" : mouse ? "Click" : "Enter", "调整等级", "Adjust level", "レベル調整");
    } else if (g_hintSlider) {
        if (mouse) add("Click/Drag", "不透明度", "Opacity", "不透明度");
        else {
            add(pad ? "A" : "Enter", pad ? "调整模式" : "开始调整", pad ? "Toggle adjust" : "Start adjust", pad ? "調整切替" : "調整開始");
            add("←/→", "调整", "Adjust", "調整");
        }
    } else if (g_hintTextField) {
        add(pad ? "A" : mouse ? "Click" : "Enter", "输入", "Type", "入力");
    } else if (g_hintAction) {
        add(pad ? "A" : mouse ? "L-click" : "Enter", "确认", "Activate", "決定");
    }
    navigate();
    // Esc clears a focused keyboard hotkey chip; it does not navigate back.
    if (!(g_hintHotkey && !pad && !mouse)) {
        if (g_page == kHome) back("关闭", "Close", "閉じる");
        else back("返回", "Back", "戻る");
    }
    return groups;
}

// Semantic strings remain available to diagnostics; every controller/key outline
// and arrow below is geometry, never a symbol-font glyph. Measure and paint share
// this path, so row bounds include the actual icons rather than their old labels.
void HintArrow(ImDrawList* dl, ImVec2 c, int dir, float radius)
{
    const ImVec2 d = dir == 0 ? ImVec2(-1, 0) : dir == 1 ? ImVec2(1, 0)
        : dir == 2 ? ImVec2(0, -1) : ImVec2(0, 1);
    const ImVec2 p(-d.y, d.x);
    dl->AddLine(c - d * radius, c + d * radius, kGold, S);
    dl->AddLine(c + d * radius, c + d * (radius * 0.25f) + p * (radius * 0.7f), kGold, S);
    dl->AddLine(c + d * radius, c + d * (radius * 0.25f) - p * (radius * 0.7f), kGold, S);
}
float HintKey(ImDrawList* dl, const std::string& key, float x, float cy)
{
    const float h = 19 * S, labelSize = 12 * S;
    const bool pad = g_hintSource.source == hints::Source::Gamepad;
    auto label = [&](const std::string& text, float cx, float centerY, float size) {
        if (!dl) return;
        const ImVec2 ts = TextSize(fReg, size, text.c_str());
        dl->AddText(fReg, size, ImVec2(cx - ts.x * 0.5f, centerY - size * 0.5f), kGold, text.c_str());
    };
    if (key == "WASD") {
        const float w = 35 * S, kw = 11 * S, kh = 9 * S;
        const char* letters[] = {"W", "A", "S", "D"};
        for (int i = 0; i < 4; ++i) {
            const float px = x + (i == 0 ? 12 : (i - 1) * 12) * S;
            const float py = cy - h * 0.5f + (i == 0 ? 0 : 10 * S);
            if (dl) {
                dl->AddRect(ImVec2(px, py), ImVec2(px + kw, py + kh), kGold, 1.5f * S, 0, S);
                label(letters[i], px + kw * 0.5f, py + kh * 0.5f, 8 * S);
                ++g_hintIcons.keycaps;
            }
        }
        return w;
    }
    if (key == "D-pad") {
        const ImVec2 c(x + h * 0.5f, cy);
        if (dl) {
            dl->AddRectFilled(ImVec2(c.x - 3 * S, cy - 9 * S), ImVec2(c.x + 3 * S, cy + 9 * S), kSoftHot, S);
            dl->AddRectFilled(ImVec2(c.x - 9 * S, cy - 3 * S), ImVec2(c.x + 9 * S, cy + 3 * S), kSoftHot, S);
            for (int i = 0; i < 4; ++i) {
                const ImVec2 d = i == 0 ? ImVec2(-6, 0) : i == 1 ? ImVec2(6, 0) : i == 2 ? ImVec2(0, -6) : ImVec2(0, 6);
                HintArrow(dl, c + d * S, i, 2 * S);
            }
            ++g_hintIcons.directional;
        }
        return h;
    }
    // Chords and alternatives have separate readable keycaps, not a long badge.
    const size_t separator = key == "Click/Drag" ? std::string::npos : key.find_first_of("/+");
    if (separator != std::string::npos) {
        const float first = HintKey(dl, key.substr(0, separator), x, cy);
        const char* join = key[separator] == '+' ? "+" : "/";
        const float joinW = TextSize(fReg, labelSize, join).x + 4 * S;
        label(join, x + first + joinW * 0.5f, cy, labelSize);
        return first + joinW + HintKey(dl, key.substr(separator + 1), x + first + joinW, cy);
    }
    const int arrow = key == "←" ? 0 : key == "→" ? 1 : key == "↑" ? 2 : key == "↓" ? 3 : -1;
    const bool face = key.size() == 1 && (key == "A" || key == "B" || key == "X" || key == "Y") &&
        (pad || (g_cap.spec && g_cap.pad && key == "X"));
    const bool shoulder = pad && (key == "LB" || key == "RB" || key == "LT" || key == "RT");
    const bool stick = pad && (key == "L3" || key == "R3");
    const bool keyboard = key == "Keyboard" || key == "Keys";
    const bool controller = key == "Pad";
    const bool mouse = key == "Click" || key == "L-click" || key == "R-click" || key == "Wheel" || key == "Click/Drag";
    const float w = stick ? 34 * S : face || arrow >= 0 ? h : keyboard || controller || mouse ? 25 * S
        : std::fmax(h, TextSize(fReg, labelSize, key.c_str()).x + (shoulder ? 10 : 8) * S);
    if (!dl) return w;
    const ImVec2 a(x, cy - h * 0.5f), b(x + w, cy + h * 0.5f), c(x + w * 0.5f, cy);
    if (stick) {
        // LS/RS names the stick; the adjacent arrow into its cap/base makes
        // pressing it distinct from simply tilting it. Entire glyph stays 19S high.
        const ImVec2 named(x + 9 * S, cy);
        dl->AddCircle(named, 8.5f * S, kGold, 20, S);
        label(key == "L3" ? "LS" : "RS", named.x, cy, 10 * S);
        const float sx = x + 27 * S;
        HintArrow(dl, ImVec2(sx, cy - 5 * S), 3, 3 * S);
        dl->AddRectFilled(ImVec2(sx - 4 * S, cy), ImVec2(sx + 4 * S, cy + 3 * S), kGold, S);
        dl->AddLine(ImVec2(sx, cy + 3 * S), ImVec2(sx, cy + 7 * S), kGold, 2 * S);
        dl->AddLine(ImVec2(sx - 5 * S, cy + 8 * S), ImVec2(sx + 5 * S, cy + 8 * S), kGold, S);
        ++g_hintIcons.stickPress;
        ++g_hintIcons.directional;
    } else if (face) {
        dl->AddCircle(c, h * 0.5f - S * 0.5f, kGold, 20, S);
        ++g_hintIcons.padButtons;
        label(key, c.x, cy, labelSize);
    } else if (controller) {
        dl->AddRect(a + ImVec2(0, 3 * S), b - ImVec2(0, 3 * S), kGold, 5 * S, 0, S);
        dl->AddLine(ImVec2(x + 5 * S, cy), ImVec2(x + 11 * S, cy), kGold, S);
        dl->AddLine(ImVec2(x + 8 * S, cy - 3 * S), ImVec2(x + 8 * S, cy + 3 * S), kGold, S);
        dl->AddCircleFilled(ImVec2(x + 19 * S, cy - 2 * S), 1.5f * S, kGold);
        dl->AddCircleFilled(ImVec2(x + 16 * S, cy + 2 * S), 1.5f * S, kGold);
        ++g_hintIcons.directional;
    } else if (keyboard) {
        dl->AddRect(a + ImVec2(0, 2 * S), b - ImVec2(0, 2 * S), kGold, 2 * S, 0, S);
        for (int r = 0; r < 2; ++r) for (int k = 0; k < 4; ++k)
            dl->AddRectFilled(ImVec2(x + (4 + k * 5) * S, cy + (-4 + r * 5) * S),
                              ImVec2(x + (6 + k * 5) * S, cy + (-2 + r * 5) * S), kGold);
        ++g_hintIcons.keycaps;
    } else if (mouse) {
        dl->AddRect(ImVec2(c.x - 6 * S, a.y), ImVec2(c.x + 6 * S, b.y), kGold, 6 * S, 0, S);
        dl->AddLine(ImVec2(c.x - 6 * S, cy - 2 * S), ImVec2(c.x + 6 * S, cy - 2 * S), kGold, S);
        dl->AddLine(ImVec2(c.x, a.y), ImVec2(c.x, cy - 2 * S), kGold, S);
        if (key == "L-click" || key == "R-click") {
            const float bx = c.x + (key == "L-click" ? -3 : 3) * S;
            dl->AddCircleFilled(ImVec2(bx, cy - 5 * S), 2 * S, kGold);
        } else dl->AddLine(ImVec2(c.x, cy - 6 * S), ImVec2(c.x, cy - 3 * S), kGold, 2 * S);
        ++g_hintIcons.directional;
    } else {
        dl->AddRect(a, b, kGold, (shoulder ? 8 : 3) * S, 0, S);
        if (shoulder) ++g_hintIcons.shoulders; else ++g_hintIcons.keycaps;
        if (arrow >= 0) { HintArrow(dl, c, arrow, 4 * S); ++g_hintIcons.directional; }
        else label(key, c.x, cy, labelSize);
    }
    return w;
}

// One baseline only: current actions anchor left, navigation/back anchor right.
// Budget by removing secondary actions, never by clipping, wrapping or shrinking.
void DrawHintGroups(ImDrawList* dl, const Model& m, float left, float right, float top, float bottom)
{
    auto groups = CompactHints(m);
    const float size = 14 * S, rowH = 19 * S, innerGap = 5 * S, gap = 12 * S;
    auto width = [&](const HintGroup& item) {
        return HintKey(nullptr, item.key, 0, 0) + innerGap + TextSize(fReg, size, item.action.c_str()).x;
    };
    auto totalWidth = [&]() {
        float total = groups.empty() ? 0 : gap * static_cast<float>(groups.size() - 1);
        for (const auto& item : groups) total += width(item);
        return total;
    };
    // Apply/Edit/Rename and context-specific essentials are mandatory (0).
    // Extra Shift (2), then Move (1) are dropped in that priority order only
    // when actual font metrics require it; never shrink or wrap essentials.
    while (totalWidth() > right - left && groups.size() > 1) {
        size_t secondary = groups.size();
        int priority = 0;
        for (size_t i = 0; i < groups.size(); ++i)
            if (groups[i].priority > priority) { priority = groups[i].priority; secondary = i; }
        if (secondary == groups.size()) break;
        groups.erase(groups.begin() + secondary);
    }
    std::vector<float> widths;
    float leftW = 0, rightW = 0;
    int leftCount = 0, rightCount = 0;
    g_hintItems.clear();
    g_hintRowBounds.clear();
    g_hintIcons = {};
    for (const auto& item : groups) {
        const float w = width(item);
        widths.push_back(w);
        if (item.right) { if (rightCount++) rightW += gap; rightW += w; }
        else { if (leftCount++) leftW += gap; leftW += w; }
        g_hintItems.push_back(item.key + " " + item.action);
    }
    g_hintRows = groups.empty() ? 0 : 1;
    const float y = (top + bottom - rowH) * 0.5f, cy = y + rowH * 0.5f;
    g_hintGeometry.rows = g_hintRows;
    g_hintGeometry.availableLeft = left;
    g_hintGeometry.availableRight = right;
    g_hintGeometry.leftActionsRight = left + leftW;
    g_hintGeometry.rightActionsLeft = right - rightW;
    g_hintGeometry.leftActionCount = leftCount;
    g_hintGeometry.rightActionCount = rightCount;
    g_hintGeometry.textLeft = leftCount ? left : right - rightW;
    g_hintGeometry.textRight = rightCount ? right : left + leftW;
    g_hintGeometry.textTop = y;
    g_hintGeometry.textBottom = y + rowH;
    DebugHintRowGeometry bounds;
    bounds.left = g_hintGeometry.textLeft; bounds.right = g_hintGeometry.textRight;
    bounds.top = y; bounds.bottom = y + rowH; bounds.groupCount = static_cast<int>(groups.size());
    // These are actual adjacent-group gaps, including the larger middle space.
    const float middleGap = right - rightW - (left + leftW);
    const bool internalGap = leftCount > 1 || rightCount > 1;
    if (leftCount && rightCount) {
        bounds.gapMin = internalGap ? std::fmin(gap, middleGap) : middleGap;
        bounds.gapMax = internalGap ? std::fmax(gap, middleGap) : middleGap;
    } else if (internalGap) bounds.gapMin = bounds.gapMax = gap;
    if (g_hintRows) g_hintRowBounds.push_back(bounds);
    float lx = left, rx = right - rightW;
    for (size_t i = 0; i < groups.size(); ++i) {
        float& x = groups[i].right ? rx : lx;
        const float keyW = HintKey(dl, groups[i].key, x, cy);
        const float textY = cy - size * 0.5f;
        dl->AddText(fReg, size, ImVec2(x + keyW + innerGap, textY), kText, groups[i].action.c_str());
        g_hintIcons.maxCenterError = std::fmax(g_hintIcons.maxCenterError, std::fabs(textY + size * 0.5f - cy));
        x += widths[i] + gap;
    }
}

// Run after NewFrame has consumed real input, before any widgets are scored.
// Installing this on the context covers the overlay and every off-screen/preview caller.
void LetterNavigation(ImGuiContext* ctx, ImGuiContextHook*)
{
    ImGuiContext& g = *ctx;
    ImGuiIO& io = g.IO;
    TrackHintInput(g);
    // 选择清单弹窗（pick_*）是单列，左右没有去处；横向导航会把焦点带出弹窗，
    // 之后列表就不再跟着上下滚。确认类弹窗仍有左右按钮，不能一起吞掉。
    bool listPopup = false;
    for (int i = 0; i < g.OpenPopupStack.Size; ++i) {
        const ImGuiWindow* w = g.OpenPopupStack[i].Window;
        if (w && w->Name && std::strstr(w->Name, "pick")) { listPopup = true; break; }
    }
    if (listPopup && g.NavMoveSubmitted &&
        (g.NavMoveDir == ImGuiDir_Left || g.NavMoveDir == ImGuiDir_Right))
        ImGui::NavMoveRequestCancel();
    // If the host no longer draws the closed panel, no old page hint survives.
    g_hintText.clear();
    g_foodPreview = {};
    g_foodPreviewText.clear();
    g_hintHeight = 0;
    g_hintRows = 0;
    g_hintGeometry = {};
    g_hintRowBounds.clear();
    g_hintIcons = {};
    g_hintItems.clear();
    g_hintListRow = g_hintListFocused = g_hintListSelected = false;
    g_hintHotkey = g_hintHotkeyClear = g_hintAction = g_hintSlider = g_hintTextField = false;
    g_hintNumber = false;
    if (g_page != kSetEdit) g_numberEdit = 0;
    std::fill(g_letterMove, g_letterMove + 4, false);
    g_reorderDir = 0;
    const bool reorderBlocked = g_cap.spec || g_renameKind != kRenameNone || io.WantTextInput ||
        g.OpenPopupStack.Size != 0 || g_popupLastFrame ||
        (g.ActiveId && g.InputTextState.ID == g.ActiveId) || g_moveKind != kMoveNone;
    const ImGuiKey reorderKeys[] = {ImGuiKey_GamepadL1, ImGuiKey_GamepadR1,
                                   ImGuiKey_UpArrow, ImGuiKey_DownArrow};
    const bool listFocused = (g_page == kSets || g_page == kFood) &&
        g_listFocusId && g_listFocusNavId == g.NavId;
    const ImGuiInputFlags reorderRepeat = ImGuiInputFlags_Repeat | ImGuiInputFlags_RepeatRateNavMove;
    for (int i = 0; i < 4; ++i) {
        const ImGuiKey key = reorderKeys[i];
        if (!ImGui::IsKeyDown(key)) g_reorderBlocked[i] = false;
        else if (reorderBlocked) g_reorderBlocked[i] = true;
        const bool chord = i < 2 ? io.KeyMods == 0 : io.KeyMods == ImGuiMod_Ctrl;
        if (!reorderBlocked && !g_reorderBlocked[i] && listFocused && chord &&
            ImGui::IsKeyPressed(key, reorderRepeat, ImGuiKeyOwner_NoOwner) && !g_reorderDir)
            g_reorderDir = (i % 2 == 0) ? -1 : 1;
    }
    if (g_reorderDir) {
        // 原生 Ctrl+方向键的请求已在 NewFrame 建立，必须在任何控件评分前取消。
        ImGui::NavMoveRequestCancel();
        return;
    }
    const bool blocked = g_cap.spec || g_renameKind != kRenameNone || io.WantTextInput || io.MouseClicked[0] ||
        (g.ActiveId != 0 && g.InputTextState.ID == g.ActiveId) ||
        (g.NavActivateId != 0 && (g.NavIdItemFlags & ImGuiItemFlags_Inputable)) || io.KeyMods != 0;
    const ImGuiInputFlags repeat = ImGuiInputFlags_Repeat | ImGuiInputFlags_RepeatRateNavMove;
    const ImGuiKey keys[4][2] = {{ImGuiKey_A, ImGuiKey_H}, {ImGuiKey_D, ImGuiKey_L},
                                {ImGuiKey_W, ImGuiKey_K}, {ImGuiKey_S, ImGuiKey_J}};
    // A held typing/capture key must be released before it can navigate on exit.
    ImGuiDir dir = ImGuiDir_None;
    for (int d = 0; d < 4; ++d) {
        for (int k = 0; k < 2; ++k) {
            const ImGuiKey key = keys[d][k];
            if (!ImGui::IsKeyDown(key)) g_letterBlocked[d][k] = false;
            else if (blocked) g_letterBlocked[d][k] = true;
            if (!blocked && !g_letterBlocked[d][k] &&
                ImGui::IsKeyPressed(key, repeat, ImGuiKeyOwner_NoOwner)) g_letterMove[d] = true;
        }
        if (g_letterMove[d] && !ImGui::IsActiveIdUsingNavDir((ImGuiDir)d)) dir = (ImGuiDir)d;
    }
    if (blocked) return;
    // One source per frame, also while sorting (where native nav is disabled).
    // Clear pulses before Sidebar/MoveItem can consume a conflicting letter.
    for (ImGuiKey key : {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow,
                         ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
                         ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown}) {
        if (ImGui::IsKeyPressed(key, repeat, ImGuiKeyOwner_Any) ||
            ImGui::IsKeyPressed(key, ImGuiInputFlags_Repeat, ImGuiKeyOwner_Any)) {
            std::fill(g_letterMove, g_letterMove + 4, false);
            return;
        }
    }
    if (g.NavMoveSubmitted || g.NavMoveForwardToNextFrame) {
        std::fill(g_letterMove, g_letterMove + 4, false);
        return;
    }
    // Sorting consumes the same repeat pulses without moving the navigation focus.
    if (g_moveKind != kMoveNone || dir == ImGuiDir_None ||
        !(io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) || !g.NavWindow ||
        g.NavWindowingTarget || (g.NavWindow->Flags & ImGuiWindowFlags_NoNavInputs) ||
        g.NavMoveSubmitted || g.NavMoveForwardToNextFrame)
        return; // Native arrows/gamepad/tab requests take priority when mixed.

    g.NavInputSource = ImGuiInputSource_Keyboard;
    ImGui::SetNavCursorVisible(true);
    ImGui::NavMoveRequestSubmit(dir, dir, ImGuiNavMoveFlags_None, ImGuiScrollFlags_None);
    // Submit alone does NOT prepare scoring. Mirror the directional reference/bias
    // setup from NavUpdateCreateMoveRequest, rather than using last frame's rectangle.
    ImGuiWindow* window = g.NavWindow;
    ImRect relative = window->NavRectRel[g.NavLayer];
    if (relative.IsInverted()) relative = ImRect(0, 0, 0, 0);
    ImRect scoring = ImGui::WindowRectRelToAbs(window, relative);
    ImVec2& preferred = window->RootWindowForNav->NavPreferredScoringPosRel[g.NavLayer];
    const ImVec2 offset = window->DC.CursorStartPos;
    if (preferred.x == FLT_MAX) preferred.x = ImMin(scoring.Min.x + 1.0f, scoring.Max.x) - offset.x;
    if (preferred.y == FLT_MAX) preferred.y = scoring.GetCenter().y - offset.y;
    if (dir == ImGuiDir_Up || dir == ImGuiDir_Down) scoring.Min.x = scoring.Max.x = preferred.x + offset.x;
    else scoring.Min.y = scoring.Max.y = preferred.y + offset.y;
    g.NavScoringRect = scoring;
    g.NavScoringNoClipRect = ImRect(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    if (g.NavId == 0) {
        g.NavInitRequest = g.NavInitRequestFromMove = true;
        g.NavInitResult.ID = 0;
    }
}

// ---------------------------------------------------------------------------
//  左侧栏。返回这一帧焦点在不在侧栏上。
// ---------------------------------------------------------------------------
bool Sidebar(float x, float y, float w, float h)
{
    // id 用固定的短串，别用会翻译的文字：切语言时 ImGui 的焦点记忆不该跟着变
    struct It { Page p; Icon ic; const char* id; const char* zh; const char* en; const char* ja; };
    static const It items[] = {{kHome, kIcFlag, "q", "任务", "Quests", "クエスト"},
                               {kSets, kIcShield, "s", "套装", "Loadouts", "装備セット"},
                               {kFood, kIcBowl, "f", "猫饭", "Meals", "猫飯"},
                               {kSettings, kIcGear, "g", "设置", "Settings", "設定"}};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    dl->AddLine(ImVec2(o.x + x + w, o.y + y + 14 * S), ImVec2(o.x + x + w, o.y + y + h - 14 * S),
                IM_COL32(201, 164, 92, 40), 1.0f);
    // 十字键：内容里往左就进侧栏（落在当前页那一格），在侧栏里上下移直接切页，往右或按 A 回到内容
    const Page cur = MainOf(g_page);
    ImGuiID ids[4] = {};
    int focused = -1, pressed = -1, curIdx = 0;
    for (int k = 0; k < 4; ++k) {
        ImGui::PushID(items[k].id);
        ids[k] = ImGui::GetID("##side");
        ImGui::PopID();
        if (items[k].p == cur) curIdx = k;
    }
    g_sideCurId = ids[curIdx];
    // 焦点不在侧栏时，侧栏不参与十字键导航：不然 ImGui 会按几何位置挑离得最近的一格
    //（从「重开」往左会先落到「猫饭」）。从内容区进侧栏由 Draw 接手，直接落到当前页那一格。
    const bool inSide = std::find(ids, ids + 4, GImGui->NavId) != ids + 4;
    if (!inSide) ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    float iy = y + 18 * S;
    for (int k = 0; k < 4; ++k) {
        const It& it = items[k];
        const float bw = 60 * S, bh = 62 * S;
        ImGui::SetCursorPos(ImVec2(x + (w - bw) * 0.5f, iy));
        ImGui::PushID(it.id);
        BeginCustom();
        if (ImGui::InvisibleButton("##side", ImVec2(bw, bh), ImGuiButtonFlags_EnableNav)) pressed = k;
        EndCustom();
    HintItem();
        if (ImGui::IsItemFocused()) focused = k;
        ImGui::PopID();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const bool on = cur == it.p;
        if (on) dl->AddRectFilled(a, b, IM_COL32(226, 194, 122, 40), 14 * S);
        else if (ImGui::IsItemHovered()) dl->AddRectFilled(a, b, IM_COL32(237, 230, 214, 14), 14 * S);
        if (focused == k && NavVisible()) dl->AddRect(a - ImVec2(3, 3) * S, b + ImVec2(3, 3) * S, kGold, 16 * S, 0, 2.0f * S);
        const ImU32 col = on ? kGold : kMuted;
        DrawIcon(dl, it.ic, ImVec2((a.x + b.x) * 0.5f, a.y + 22 * S), 10 * S, col);
        CenterText(dl, fReg, szSmall, ImVec2((a.x + b.x) * 0.5f, a.y + 46 * S), col, i18n::Tr(it.zh, it.en, it.ja));
        iy += bh + 6 * S;
    }
    if (!inSide) ImGui::PopItemFlag();
    g_sideFocusIdx = focused;
    const bool rightKey = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) || g_letterMove[ImGuiDir_Right];
    if (pressed >= 0) {
        Go(items[pressed].p);              // 鼠标点 / 按 A：切过去，焦点进内容的默认项
        g_sideFocusedLast = false;
    } else if (focused >= 0 && rightKey) {
        if (g_listFocusId) g_focusId = g_listFocusId;   // 回到离开时那一行，别从默认项重来
        g_focus = 2;                       // 往右：回到这一页的默认项（不让 ImGui 按几何位置乱挑）
        g_sideFocusedLast = false;
    } else if (focused >= 0) {
        if (!g_sideFocusedLast && focused != curIdx) {
            // 刚从内容移进来：落点是离原焦点最近的那格，不一定是当前页 —— 挪回当前页那格
            ImGui::SetFocusID(ids[curIdx], ImGui::GetCurrentWindow());
        } else if (focused != curIdx) {
            g_page = items[focused].p;     // 在侧栏里上下移：直接切页，焦点留在侧栏
            g_focus = 0;
        }
        g_sideFocusedLast = true;
    } else {
        if (g_sideFocusedLast) g_focus = 2;   // 从侧栏往右回到内容：落到这一页的默认项
        g_sideFocusedLast = false;
    }
    return focused >= 0;
}

} // namespace

// 猫饭页列表里会出现哪些编号：活着的按 foodOrder，另外把「删掉但还有人在用」的排在后面。
// 后者内容还在、照样吃得到，列出来才能再选回来；没人引用以后就不再出现，等于真正消失。
std::vector<int> FoodRowIds(const Config& c)
{
    std::vector<int> ids = c.FoodIds();
    for (const auto& kv : c.foods) {
        if (!kv.second.deleted) continue;
        bool used = (c.food == kv.first);
        for (const LoadoutSet& s : c.sets)
            if (s.food == kv.first) { used = true; break; }
        if (used && std::find(ids.begin(), ids.end(), kv.first) == ids.end()) ids.push_back(kv.first);
    }
    return ids;
}

// ---------------------------------------------------------------------------
void Init(float scale)
{
    S = scale < 0.7f ? 0.7f : (scale > 2.5f ? 2.5f : scale);
    szBody = 19 * S;
    szSmall = 15 * S;
    szTitle = 34 * S;
    szPage = 26 * S;
    szBtn = 22 * S;

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    // This is a single overlay, not a desktop window manager. ImGui maps its
    // windowing/menu button to FaceLeft (X), which we also use for panel actions.
    // Keep widget navigation, but never highlight/move/resize the whole panel.
    ImGuiContext& g = *ImGui::GetCurrentContext();
    g.ConfigNavWindowingWithGamepad = false;
    g.ConfigNavWindowingKeyNext = ImGuiKey_None;
    g.ConfigNavWindowingKeyPrev = ImGuiKey_None;
    for (auto& blocked : g_letterBlocked) std::fill(blocked, blocked + 2, false);
    // Init may be called again on the same context; never install duplicate hooks.
    bool hooked = false;
    for (const ImGuiContextHook& existing : g.Hooks)
        if (existing.Callback == LetterNavigation && existing.Type == ImGuiContextHookType_NewFramePost) hooked = true;
    if (!hooked) {
        ImGuiContextHook hook;
        hook.Type = ImGuiContextHookType_NewFramePost;
        hook.Callback = LetterNavigation;
        ImGui::AddContextHook(&g, &hook);
    }

    wchar_t win[MAX_PATH] = {};
    ::GetWindowsDirectoryW(win, MAX_PATH);
    char fonts[MAX_PATH * 3] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, win, -1, fonts, sizeof(fonts), nullptr, nullptr);
    const std::string dir = std::string(fonts) + "\\Fonts\\";
    ImFontConfig fc;
    fc.OversampleH = 1;
    fReg = io.Fonts->AddFontFromFileTTF((dir + "msyh.ttc").c_str(), szBody, &fc);
    fBold = io.Fonts->AddFontFromFileTTF((dir + "msyhbd.ttc").c_str(), szBody, &fc);
    if (!fReg) fReg = io.Fonts->AddFontDefault();
    if (!fBold) fBold = fReg;
    io.FontDefault = fReg;

    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 22 * S;
    st.WindowBorderSize = 1.0f;
    st.WindowPadding = ImVec2(0, 0);
    st.ChildRounding = 0;
    st.ChildBorderSize = 0;
    st.PopupRounding = 16 * S;
    st.PopupBorderSize = 1.0f;
    st.FrameRounding = 12 * S;
    st.FramePadding = ImVec2(14 * S, 10 * S);
    st.ItemSpacing = ImVec2(8 * S, 6 * S);
    st.GrabRounding = 10 * S;
    st.GrabMinSize = 18 * S;
    st.ScrollbarSize = 6 * S;
    st.ScrollbarRounding = 6 * S;
    st.SelectableTextAlign = ImVec2(0, 0.5f);

    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = V(kText);
    c[ImGuiCol_TextDisabled] = V(kMuted);
    c[ImGuiCol_WindowBg] = ImVec4(0.063f, 0.051f, 0.035f, 0.80f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.094f, 0.078f, 0.055f, 0.97f);
    c[ImGuiCol_Border] = V(IM_COL32(201, 164, 92, 90));
    c[ImGuiCol_FrameBg] = ImVec4(0, 0, 0, 0.35f);
    c[ImGuiCol_FrameBgHovered] = V(IM_COL32(226, 194, 122, 30));
    c[ImGuiCol_FrameBgActive] = V(IM_COL32(226, 194, 122, 46));
    c[ImGuiCol_Header] = V(IM_COL32(226, 194, 122, 30));
    c[ImGuiCol_HeaderHovered] = V(IM_COL32(237, 230, 214, 18));
    c[ImGuiCol_HeaderActive] = V(IM_COL32(226, 194, 122, 56));
    c[ImGuiCol_SliderGrab] = V(kGold);
    c[ImGuiCol_SliderGrabActive] = V(kGoldHot);
    c[ImGuiCol_CheckMark] = V(kGold);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = V(IM_COL32(237, 230, 214, 40));
    c[ImGuiCol_ScrollbarGrabHovered] = V(IM_COL32(237, 230, 214, 70));
    c[ImGuiCol_ScrollbarGrabActive] = V(IM_COL32(226, 194, 122, 120));
    c[ImGuiCol_NavCursor] = V(kGold);
    c[ImGuiCol_TextSelectedBg] = V(IM_COL32(226, 194, 122, 80));
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.3f);
}

// 仅由 UI 回归测试前置声明；不扩展 ui.h 的面板接口。
std::string DebugFoodSkillsSummary(const FoodPreset& f) { return FoodSkillsSummary(f); }

std::string DebugSetSummary(const Model& m, int index)
{
    if (!m.cfg || index < 0 || index >= (int)m.cfg->sets.size()) return {};
    return SetSummary(m.cfg->sets[index], m);
}

void DebugGo(int page, int editIndex)
{
    static const Page map[] = {kHome, kQuests, kSets, kSetEdit, kFood, kFoodEdit, kSettings};
    if (page < 0 || page > 6) return;
    g_editSet = editIndex;
    g_editFood = editIndex;
    g_questTarget = -1;
    g_questFrom = kHome;
    Go(map[page]);
}

float DebugListScroll()
{
    return g_listScroll;
}

int DebugListFocusId()
{
    return g_listFocusId;
}

int DebugPage()
{
    static const Page map[] = {kHome, kQuests, kSets, kSetEdit, kFood, kFoodEdit, kSettings};
    for (int i = 0; i < 7; ++i) if (map[i] == g_page) return i;
    return -1;
}

bool DebugSidebarFocused() { return g_sideFocusedLast; }
int DebugSidebarFocusIndex() { return g_sideFocusIdx; }
float DebugFrameHeight() { return g_frameH; }

void DebugStartCapture(Model& m, int row, bool pad)
{
    std::string* const specs[] = {&m.cfg->hkRestart, &m.cfg->hkPanel, &m.cfg->hkResetOnly, &m.cfg->hkRefill};
    if (row < 0 || row > 3) return;
    g_page = kSettings;
    EndCapture();
    g_cap.spec = specs[row];
    g_cap.pad = pad;
    g_cap.lastInput = ImGui::GetTime();
}

std::string DebugCaptureWhy() { return g_capWhy; }
void DebugFocusHotkey(bool pad) { g_debugFocusHotkey = pad ? 0 : 1; }

void DebugFocusSearch() { g_debugFocusSearch = true; }
std::string DebugSearch() { return g_search; }
void DebugSetSearch(const char* text) { std::snprintf(g_search, sizeof(g_search), "%s", text); }
void DebugFocusClearAll() { g_focusClearAll = true; }
void DebugFocusState() { g_focusState = true; }
void DebugRadioInfo(unsigned long long& id, int& focused, unsigned long long& nav) { id = g_dbgRadioId; focused = g_dbgRadioFocused; nav = g_dbgNavId; }
void DebugIds(unsigned long long& back, unsigned long long& r0, unsigned long long& r1) { back = g_dbgBackId; r0 = g_dbgRadio0Id; r1 = g_dbgRadioId; }
const char* DebugNavWindowName() { return GImGui->NavWindow ? GImGui->NavWindow->Name : ""; }
void DebugWindowInfo(int& navwin_h, int& clip_h) { navwin_h = GImGui->NavWindow ? (int)GImGui->NavWindow->Size.y : 0; clip_h = GImGui->NavWindow ? (int)(GImGui->NavWindow->InnerClipRect.Max.y - GImGui->NavWindow->InnerClipRect.Min.y) : 0; }
int DebugHintSource() { return static_cast<int>(g_hintSource.source); }
std::string DebugHintText() { return g_hintText; }
float DebugHintHeight() { return g_hintHeight; }
int DebugHintRows() { return g_hintRows; }
DebugHintGeometry DebugHintBounds() { return g_hintGeometry; }
std::vector<std::string> DebugHintItems() { return g_hintItems; }
std::vector<DebugHintRowGeometry> DebugHintRowBounds() { return g_hintRowBounds; }
DebugHintIconGeometry DebugHintIcons() { return g_hintIcons; }
DebugFoodPreviewGeometry DebugFoodPreviewBounds() { return g_foodPreview; }
std::string DebugFoodPreviewText() { return g_foodPreviewText; }
bool DebugHintLayout(float& top, float& bottom, float& contentBottom) {
    top = g_hintTop; bottom = g_hintBottom; contentBottom = g_hintContentBottom;
    return !g_hintText.empty() && g_hintHeight > 0;
}
int DebugClearStage() { return g_clearStage; }
ImVec2 DebugSettingsExportCenter() {
    return ImVec2((g_settingsExportMin.x + g_settingsExportMax.x) * 0.5f,
                  (g_settingsExportMin.y + g_settingsExportMax.y) * 0.5f);
}
void DebugFocusSettingsTransfer(bool importing) { g_debugTransferFocus = importing ? 2 : 1; }
void DebugFocusPickEntry(const wchar_t* name) { g_debugPickFocus = name ? name : L""; }
std::vector<std::wstring> DebugPickEntries() {
    std::vector<std::wstring> names;
    for (const auto& e : g_pickList) names.push_back(e.name);
    return names;
}
bool DebugSettingsConfirm() { return g_settingsConfirmOpen; }
void DebugOpenPick() { OpenPick(); }
void DebugClosePick() { g_pickOpen = false; }
void DebugOpenRename() { g_debugRename = true; }
void DebugCloseRename() { g_renameCloseReq = true; }

std::string DebugClampName(const char* utf8)
{
    return ClampName(utf8);
}

void DebugMove(int kind, int id)
{
    g_moveKind = kind == 1 ? kMoveSet : (kind == 2 ? kMoveFood : kMoveNone);
    g_moveId = id;
}

bool DebugMoveActive() { return g_moveKind != kMoveNone; }
int DebugMoveId() { return g_moveId; }

void DebugNewSet(Model& m)
{
    Config& c = *m.cfg;
    LoadoutSet s;
    s.id = c.NextSetId();
    s.name = i18n::Fmt(i18n::Tr("套装 %d", "Loadout %d", "セット %d"), s.id);
    c.sets.push_back(s);
    g_editSet = (int)c.sets.size() - 1;
    g_afterRename = 1;
    OpenRenameCreate(m, kRenameSet, g_editSet);
    m.reqSave = true;
}

void DebugOpenDelete() { g_openDelete = true; }

void OnOpen(Model& m)
{
    g_openDelete = false;
    g_hintText.clear();
    g_foodPreview = {};
    g_foodPreviewText.clear();
    g_hintHeight = 0;
    g_hintRows = 0;
    g_hintGeometry = {};
    g_hintRowBounds.clear();
    g_hintIcons = {};
    g_hintItems.clear();
    g_renameKind = kRenameNone;
    g_renameOpen = g_debugRename = g_renameCloseReq = false;
    g_pickOpen = false;
    g_settingsPending.clear();
    g_settingsConfirmOpen = false;
    g_debugTransferFocus = 0;
    g_editSet = -1;
    g_editFood = 0;
    g_questTarget = -1;
    g_clearStage = 0;
    Go(kHome); // 与所有换页共用退出移动模式的路径，避免关闭后重开仍禁用导航
    g_focusId = 0;
    const ImGuiKey reorderKeys[] = {ImGuiKey_GamepadL1, ImGuiKey_GamepadR1,
                                   ImGuiKey_UpArrow, ImGuiKey_DownArrow};
    for (int i = 0; i < 4; ++i) g_reorderBlocked[i] = ImGui::IsKeyDown(reorderKeys[i]);
    g_search[0] = 0;
    g_animHc = 0;
    g_popupLastFrame = false;
    EndCapture();
    m.capturing = false;
    ImGui::SetNavCursorVisible(true);
}

void Draw(Model& m, float screenW, float screenH)
{
    if (!m.cfg) return;
    if (m.reqResetView) { m.reqResetView = false; OnOpen(m); }
    // ConfigNavWindowingWithGamepad 只禁用整窗高亮，X 的松开仍会切换菜单层、清掉控件焦点。
    // 本面板没有菜单层，X 专用于改名/录键；取消 NewFrame 建立的菜单层切换请求。
    GImGui->NavWindowingToggleLayer = false;
    const float W = 560 * S;
    const float sideW = 86 * S;
    const float pad = 26 * S;
    const ImVec2 inPad(12 * S, 8 * S);   // 内容区内边距：控件的焦点描边画在控件外面，贴边会被裁
    const float hintH = 30 * S; // one 19S icon row, compact padding, stable across devices
    const float footerPad = 10 * S; // do not append the content area's 26S padding
    const float statusH = m.status.empty() ? 0 : szSmall + 14 * S;
    // 内容区高度按这一页上一帧量出来的实际内容收紧（主页就很矮），长的页面封顶后滚动；
    // 换任务是长列表，直接用满
    const float maxHc = std::fmax(120 * S, std::fmin(560 * S, screenH - 80 * S - (pad - inPad.y) - footerPad - statusH - hintH));
    const float minHc = std::fmin(300 * S, maxHc);
    const bool measured = g_page != kQuests && g_contentH[g_page] > 0;
    const float hc = measured ? std::fmin(maxHc, std::fmax(minHc, g_contentH[g_page])) : maxHc;
    // 换页时面板外框的高度平滑过渡（约 0.2 秒），以屏幕中线为轴上下一起收放。
    // 只有画出来的外框在动：ImGui 窗口一换页就放在最终的位置和大小，内容按最终排版，
    // 外框外面的部分这一帧裁掉。窗口自己每帧挪动或变高的话，ImGui 的十字键导航和滚动定位会乱。
    // 第一次进这页还没量过：先别动，下一帧量完再变。刚打开面板就碰上没量过的页时 g_animHc 还是 0，
    // 这一帧整个不画（外框高度 0，内容全裁掉），下一帧量完直接到位，免得先撑满再缩回来
    float target = hc;
    if (g_page != kQuests && !measured) target = g_animHc;
    if (g_animHc <= 0) g_animHc = target;
    else g_animHc += (target - g_animHc) * (1.0f - std::exp(-ImGui::GetIO().DeltaTime * 16.0f));
    if (std::fabs(target - g_animHc) < 0.5f) g_animHc = target;
    const float H = (pad - inPad.y) + hc + statusH + hintH + footerPad;               // 这一页的最终大小
    const float Hf = g_animHc > 0 ? (pad - inPad.y) + g_animHc + statusH + hintH + footerPad : 0.0f;   // 看得见的外框（动画中）
    // 左边有侧栏、上面是标题，正中摆放时视觉重心会偏右下。整体往左挪四分之一个侧栏宽、
    // 再往上提一点，主页的「重开」落在屏幕正中略偏上
    const float cy = std::floor(screenH * 0.5f - 26 * S);
    const ImVec2 pos(std::floor((screenW - W) * 0.5f - sideW * 0.25f), std::floor(cy - H * 0.5f));
    const ImVec2 fMin(pos.x, std::floor(cy - Hf * 0.5f)), fMax(pos.x + W, std::floor(cy - Hf * 0.5f) + Hf);
    g_frameH = Hf;
    g_panelMin = pos;
    g_panelMax = ImVec2(pos.x + W, pos.y + H);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(W, H), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);   // 底和边框自己画（按动画中的外框大小）
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    const bool shown = ImGui::Begin("IN-Q##panel", nullptr, wf);
    ImGui::PopStyleVar();
    if (!shown) { ImGui::End(); return; }
    if (ImGui::IsWindowAppearing()) ImGui::SetWindowFocus();
    ImGuiWindow* const panel = ImGui::GetCurrentWindow();
    {
        const ImGuiStyle& st = ImGui::GetStyle();
        ImVec4 bg = st.Colors[ImGuiCol_WindowBg];
        bg.w = m.cfg->panelOpacity / 100.0f;
        ImDrawList* bdl = Hf > 0 ? ImGui::GetBackgroundDrawList() : nullptr;
        if (bdl) bdl->AddRectFilled(fMin, fMax, ImGui::ColorConvertFloat4ToU32(bg), st.WindowRounding);
        if (bdl) bdl->AddRect(fMin, fMax, ImGui::GetColorU32(ImGuiCol_Border), st.WindowRounding, 0, 1.0f);
    }

    // 录快捷键时手柄和键盘都不用来导航（按什么都是在录），下一帧起生效
    const bool capturing = g_cap.spec != nullptr;
    const bool hotkeyEsc = g_page == kSettings && g_hotkeyFocusId != 0 &&
        GImGui->NavId == g_hotkeyFocusId && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    g_hotkeyFocusId = 0;
    g_captureHandledEsc = capturing && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    UpdateCapture(m);
    const ImGuiConfigFlags navFlags = ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    if (g_cap.spec || g_moveKind != kMoveNone) ImGui::GetIO().ConfigFlags &= ~navFlags;
    else ImGui::GetIO().ConfigFlags |= navFlags;

    // B / Esc 返回（主页上就是关面板）。弹窗、输入框、录快捷键时不算。
    // 弹窗里按 B 是关弹窗：ImGui 在这一帧开头就已经把弹窗关掉了，所以还要看上一帧开没开着，
    // 不然同一下 B 又会被当成「返回上一页」
    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || g_popupLastFrame;
    const bool typing = ImGui::GetIO().WantTextInput;
    const bool numberCancel = g_numberEdit && !popup && !typing && !capturing &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false));
    if (numberCancel) { g_numberEdit = 0; ImGui::ClearActiveID(); }
    // 移动模式里 B / Esc 是「放下」，由列表页自己吃掉，这里不能再当成「返回上一页」
    if (!numberCancel && !popup && !typing && !capturing && !hotkeyEsc && g_moveKind == kMoveNone &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        if (Back(g_page) != g_page) Go(Back(g_page));
        else if (g_page != kHome) Go(kHome);
        else m.reqClose = true;
    }

    const bool movingNow = g_moveKind != kMoveNone;
    if (movingNow) ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    // Sidebar draws its own 14S end inset: stop 10S before the footer rule.
    const float footerTop = (pad - inPad.y) + hc + statusH + 3 * S;
    g_hintSidebarBottom = pos.y + footerTop - 10 * S;
    const bool sideNav = Sidebar(0, 0, sideW, footerTop + 4 * S);
    if (movingNow) ImGui::PopItemFlag();

    // 焦点在侧栏时，内容区不参与十字键导航：上下只在侧栏里走，不会被右边离得近的一行抢走；
    // 回内容区由侧栏自己处理（按右 / 按 A 落到这一页的默认项）
    if (sideNav) ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::SetCursorPos(ImVec2(sideW + pad - inPad.x, pad - inPad.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, inPad);
    ImGui::BeginChild("##content", ImVec2(W - sideW - pad * 2 + inPad.x * 2, hc),
                      ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding,
                      g_page == kQuests ? ImGuiWindowFlags_NoScrollbar : 0);
    ImGui::PopStyleVar();
    g_focusUsed = false;
    const Page drawn = g_page;
    switch (g_page) {
    case kHome: PageHome(m); break;
    case kQuests: PageQuests(m); break;
    case kSets: PageSets(m); break;
    case kSetEdit: PageSetEdit(m); break;
    case kFood: PageFood(m); break;
    case kFoodEdit: PageFoodEdit(m); break;
    case kSettings: PageSettings(m); break;
    }
    // 默认项这一帧没画出来（比如被搜索过滤掉了）就作废，免得之后抢焦点。
    // 但这一帧中途换了页的话，请求是给新页的，留到下一帧
    if (!g_focusUsed && g_page == drawn) g_focus = 0;
    if (g_debugRename) {
        g_debugRename = false;
        if (g_page == kSets && !m.cfg->sets.empty()) OpenRename(m, kRenameSet, 0);
        else if (g_page == kFood && !m.cfg->foods.empty()) OpenRename(m, kRenameFood, m.cfg->foods.begin()->first);
    }
    g_contentH[drawn] = ImGui::GetCursorPosY() + inPad.y;   // 这一页内容实际多高，下一帧按它收紧面板
    ImGui::EndChild();
    if (sideNav) ImGui::PopItemFlag();

    // 三个列表页都是单列：往右没有去处，放给 ImGui 只会把焦点判空，之后上下就不再滚动。
    if (!sideNav && !popup && GImGui->NavMoveSubmitted && GImGui->NavMoveDir == ImGuiDir_Right &&
        (g_page == kHome || g_page == kSets || g_page == kFood))
        ImGui::NavMoveRequestCancel();

    // 十字键往左、内容区里左边已经没有能去的控件：直接去侧栏当前页那一格。
    // 这一帧所有控件都已经参与过打分，结果是空的就取消这次移动，换成我们指定的落点。
    ImGuiContext& g = *GImGui;
    if (!sideNav && !movingNow && !popup && g_sideCurId && g.NavMoveSubmitted && g.NavMoveScoringItems && g.NavMoveDir == ImGuiDir_Left &&
        !g.NavMoveResultLocal.ID && !g.NavMoveResultLocalVisible.ID && !g.NavMoveResultOther.ID) {
        ImGui::NavMoveRequestCancel();
        ImGui::SetFocusID(g_sideCurId, ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
    }
    m.capturing = g_cap.spec != nullptr;
    g_popupLastFrame = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);

    if (!m.status.empty()) {
        ImGui::SetCursorPos(ImVec2(sideW + pad, (pad - inPad.y) + hc + 6 * S));
        Text(fReg, szSmall, kMuted, m.status.c_str());
    }
    ImGui::End();

    // 外框在收放时，面板（连同里面的子窗口）这一帧画出来的东西裁到外框里。弹窗是独立的窗口，不受影响。
    if (Hf < H - 0.5f)
        for (ImGuiWindow* w : GImGui->Windows)
            if (w->RootWindow == panel && w->WasActive && w->DrawList)
                for (ImDrawCmd& c : w->DrawList->CmdBuffer) {
                    c.ClipRect.y = std::fmax(c.ClipRect.y, fMin.y);
                    c.ClipRect.w = std::fmin(c.ClipRect.w, fMax.y);
                }

    PickPopup(m);
    SettingsImportPopup(m);
    RenamePopup(m);
    // Popups drawn after the panel must still block B/Esc on their closing frame.
    g_popupLastFrame = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    if (!m.reqClose && Hf > 0) DrawFoodPreview(*m.cfg);
    else { g_foodPreview = {}; g_foodPreviewText.clear(); }

    // Draw in the panel's draw list, below its children and below all popup windows.
    // The reserved strip is outside content/status bounds, including at 720p.
    g_hintContentBottom = pos.y + (pad - inPad.y) + hc + statusH;
    g_hintTop = g_hintContentBottom + 3 * S;
    g_hintBottom = pos.y + H - 7 * S;
    if (!m.reqClose && Hf > 0) {
        g_hintText = OperationHints(m);
        g_hintHeight = hintH;
        ImDrawList* dl = panel->DrawList;
        const float left = pos.x + 18 * S, right = pos.x + W - 18 * S;
        dl->PushClipRect(ImVec2(left, std::fmax(g_hintTop, fMin.y)), ImVec2(right, std::fmin(g_hintBottom, fMax.y)), true);
        dl->AddLine(ImVec2(left, g_hintTop), ImVec2(right, g_hintTop), kSoft, S);
        g_hintGeometry.separatorLeft = left;
        g_hintGeometry.separatorRight = right;
        g_hintGeometry.separatorY = g_hintTop;
        g_hintGeometry.sidebarBottom = g_hintSidebarBottom;
        DrawHintGroups(dl, m, left, right, g_hintTop + 2 * S, g_hintBottom - S);
        dl->PopClipRect();
    }
}

} // namespace ui
