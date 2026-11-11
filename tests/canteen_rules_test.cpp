// 食堂猫饭合法性判定的回归：只用纯推算，不碰游戏。
// 规则对照 speedrun_util.dll：耐力必须 +50、一组只出一个技能、50/43 互斥、每日技能只算当日合法。
#include "../src/canteen_rules.h"
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static canteen::Result Check(int hp, int stam, int atk, int def, int res, int s0, int s1 = -1, int s2 = -1)
{
    const int skills[3] = {s0, s1, s2};
    return canteen::Check(hp, stam, atk, def, res, skills);
}

int main()
{
    // 空猫饭：食堂随便做，合法。
    CHECK(Check(-1, 2, 0, 0, 0, -1).verdict == canteen::Verdict::Legal);

    // 体力只能 0..50、耐力只能 0/25/50、三样数值最多 3 级。
    CHECK(Check(5, 2, 0, 0, 0, -1).verdict == canteen::Verdict::Legal);
    CHECK(Check(6, 2, 0, 0, 0, -1).reason == canteen::Reason::Range);
    CHECK(Check(0, 3, 0, 0, 0, -1).reason == canteen::Reason::Range);
    CHECK(Check(0, 2, 4, 0, 0, -1).reason == canteen::Reason::Range);
    CHECK(Check(0, 2, 0, 0, 0, 99).reason == canteen::Reason::Range);

    // speedrun_util 的硬规则：食堂固定给耐力 +50，+0 / +25 都不合法。
    CHECK(Check(0, 0, 0, 0, 0, -1).reason == canteen::Reason::Stamina);
    CHECK(Check(0, 1, 0, 0, 0, -1).reason == canteen::Reason::Stamina);

    // 攻大 = 6 个肉，6 个肉里放 6 个勇气的三级技能（29）。
    CHECK(Check(0, 2, 3, 0, 0, 29).verdict == canteen::Verdict::Legal);
    // 6 个肉里 2 个勇气出 1 级（0），另外 4 个各放一组当散件，也合法。
    CHECK(Check(0, 2, 3, 0, 0, 0).verdict == canteen::Verdict::Legal);
    // 勇气 2 级要 4~5 个，攻中要 4~5 个肉，正好同一批。
    CHECK(Check(0, 2, 2, 0, 0, 1).verdict == canteen::Verdict::Legal);
    // 勇气 1 级 + 忍耐 1 级要 2+2 个；攻大要 6 个肉，放得下。
    CHECK(Check(0, 2, 3, 0, 0, 0, 36).verdict == canteen::Verdict::Legal);
    // 6 个肉正好 6 个新鲜 → 体力 +50 也还是同一桌。
    CHECK(Check(5, 2, 3, 0, 0, 29).verdict == canteen::Verdict::Legal);

    // 勇气 1 级 + 忍耐 3 级 = 2+6 个，超一餐 6 个。
    CHECK(Check(0, 2, 0, 0, 0, 0, 32).reason == canteen::Reason::TooMany);
    CHECK(Check(0, 2, 0, 0, 0, 0, 32).needed >= 8);
    // 勇气 3 级 + 忍耐 3 级 = 12 个。
    CHECK(Check(0, 2, 0, 0, 0, 29, 32).reason == canteen::Reason::TooMany);
    CHECK(Check(0, 2, 0, 0, 0, 29, 32).needed == 12);
    // 三样数值各要 6 个，肉鱼菜分开也是 18 个。
    CHECK(Check(0, 2, 3, 3, 3, -1).reason == canteen::Reason::TooMany);
    // 两样 3 级（6 肉 + 6 鱼）已经超了。
    CHECK(Check(0, 2, 3, 3, 0, -1).reason == canteen::Reason::TooMany);

    // 同一个技能两遍、同一组两个技能都不行。
    CHECK(Check(0, 2, 0, 0, 0, 0, 0).reason == canteen::Reason::Duplicate);
    CHECK(Check(0, 2, 0, 0, 0, 0, 1).reason == canteen::Reason::SameGroup);
    CHECK(Check(0, 2, 0, 0, 0, 25, 19).reason == canteen::Reason::SameGroup);   // 都是洞察组

    // speedrun_util 硬编码的互斥对：技能 50 与 43 不能同时出现。
    CHECK(Check(0, 2, 0, 0, 0, 50, 43).reason == canteen::Reason::Exclusive);

    // 饮料组技能只能靠饮料：财运 1 级 2 瓶、调理 3 级 6 瓶。
    CHECK(Check(0, 2, 0, 0, 0, 11).verdict == canteen::Verdict::Legal);
    CHECK(Check(0, 2, 0, 0, 0, 50).verdict == canteen::Verdict::Legal);
    CHECK(Check(5, 2, 0, 0, 0, 50).verdict == canteen::Verdict::Legal);        // 6 瓶也算 6 个新鲜
    // 攻中（4 肉）+ 财运 1 级（2 瓶）正好 6 个。
    CHECK(Check(4, 2, 2, 0, 0, 11).verdict == canteen::Verdict::Legal);
    // 攻大（6 肉）已经占满，再要 2 瓶饮料就超了。
    CHECK(Check(0, 2, 3, 0, 0, 11).reason == canteen::Reason::TooMany);

    // 每日技能：不属于任何食材组，只能算「当日合法」。
    CHECK(Check(0, 2, 0, 0, 0, 4).verdict == canteen::Verdict::NeedsDaily);
    CHECK(Check(0, 2, 0, 0, 0, 4).dailySkill == 4);
    CHECK(Check(0, 2, 0, 0, 0, 4).dailyCount == 1);
    CHECK(Check(0, 2, 0, 0, 0, 56).verdict == canteen::Verdict::NeedsDaily);
    // 两个每日技能一起选：当日要同时提供两个。
    CHECK(Check(0, 2, 0, 0, 0, 4, 43).dailyCount == 2);
    // 每日技能 + 食材技能一起选：仍然只是「当日合法」。
    CHECK(Check(0, 2, 2, 0, 0, 1, 4).verdict == canteen::Verdict::NeedsDaily);

    // 食材技能表就是这 27 个。
    CHECK(canteen::IsIngredientSkill(0) && canteen::IsIngredientSkill(53));
    CHECK(!canteen::IsIngredientSkill(4) && !canteen::IsIngredientSkill(37) && !canteen::IsIngredientSkill(56));

    if (failures) { std::printf("%d 项失败\n", failures); return 1; }
    std::printf("canteen_rules_test 全部通过\n");
    return 0;
}
