// 食堂猫饭「合法性」判定：按 speedrun_util 逆向出来的判定规则复刻，不读游戏内存、不改任何东西。
//
// 规则来源：`speedrun_util.dll` 的判定函数（`.text` 0x4aed0–0x4c388）与两张表
// （`.rdata` 0xd7a60 = 技能→组，0xd7b50 = 技能→最少食材数）。要点：
//   1. 耐力不是 +50 直接判不合法（"The canteen always gives stamina +50."）。
//   2. 一餐最多 6 个食材；每组 2~3 个 = 1 级技能，4~5 个 = 2 级，6 个 = 3 级；一组只出一个技能。
//   3. 三个技能号里 >56 的忽略；组号为 -1 的算「每日技能」，不占食材。
//   4. 硬编码互斥对：技能 50 与 43 不能同时出现。
//   5. 数值档位（无/小/中/大）对应食材数 0~1 / 2~3 / 4~5 / 6；
//      buff 要的食材和技能要的食材从同一批 6 个里出，两者区间必须能重叠。
//   6. 含每日技能时只能算「当日合法」，需要当天正好提供这些每日技能。
#pragma once

namespace canteen
{
enum class Verdict
{
    Legal,       // 食堂能复现
    NeedsDaily,  // 含只能用当天每日技能凑出来的条目
    Impossible   // 食堂无论如何做不出
};

enum class Reason
{
    None = 0,
    Range,       // 数值超出食堂能给的档位
    Duplicate,   // 同一个技能选了两遍
    SameGroup,   // 同一组技能只能有一个
    TooMany,     // 需要的食材超过 6 个
    Daily,       // 只能来自每日技能
    Stamina,     // 食堂固定给耐力 +50
    Exclusive,   // 硬编码互斥的技能对同时出现
    BuffCrowd    // buff 与技能的食材要求凑不到同一批
};

struct Result
{
    Verdict verdict = Verdict::Impossible;
    Reason reason = Reason::None;
    int needed = 0;        // TooMany 时给出的下界；其它情况为 0
    int dailySkill = -1;   // NeedsDaily 时第一个只能靠每日技能拿到的编号
    int dailyCount = 0;    // NeedsDaily 时的每日技能个数
};

// hpTier: 0..5（每档 +10）；staminaTier: 0..2（每档 +25）；
// attack/defense/elemRes: 0..3（0 无，1 小，2 中，3 大）；skills 里 -1 表示没选。
Result Check(int hpTier, int staminaTier, int attack, int defense, int elemRes, const int (&skills)[3]);

// 「空饭」：所有数值为 0、三个技能都没选。这种预设不是一顿饭，而是把内存里的猫饭清空，
// 所以耐力照 0 写、也不参与合法性判定。
bool Neutral(int hpTier, int staminaTier, int attack, int defense, int elemRes, const int (&skills)[3]);

// 编号是否属于「食材能出的 27 个技能」之一。
bool IsIngredientSkill(int id);
}  // namespace canteen
