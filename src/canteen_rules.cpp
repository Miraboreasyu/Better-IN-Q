#include "canteen_rules.h"

namespace canteen
{
namespace
{
// 食材组。前五个（勇气…炮术）肉、鱼、菜都能带；后四个只有饮料能带。
enum Group
{
    kCourage = 0,
    kResilience,
    kVigor,
    kAcumen,
    kArtillery,
    kPerception,
    kFortune,
    kPreparation,
    kTrailblazer,
    kGroupCount
};
constexpr int kDrinkFirst = kPerception;
constexpr int kSharedGroups = kDrinkFirst;                 // 前五组
constexpr int kDrinkGroups = kGroupCount - kDrinkFirst;    // 后四组

// 互斥对：speedrun_util 在 0x4b078 处硬编码的 {0x32, 0x2b}。
constexpr int kExclusiveA = 50;
constexpr int kExclusiveB = 43;

struct SkillEntry
{
    int id;
    int group;
    int tier;   // 1..3
};

// 与 .rdata:0xd7a60（组）和 0xd7b50（最少食材数）逐项一致。
const SkillEntry kSkills[] = {
    {0, kCourage, 1},   {1, kCourage, 2},   {29, kCourage, 3},
    {36, kResilience, 1}, {40, kResilience, 2}, {32, kResilience, 3},
    {20, kVigor, 1},    {6, kVigor, 2},     {2, kVigor, 3},
    {34, kAcumen, 1},   {5, kAcumen, 2},    {8, kAcumen, 3},
    {12, kArtillery, 1}, {31, kArtillery, 2}, {7, kArtillery, 3},
    {25, kPerception, 1}, {19, kPerception, 2}, {3, kPerception, 3},
    {11, kFortune, 1},  {30, kFortune, 2},  {13, kFortune, 3},
    {48, kPreparation, 1}, {49, kPreparation, 2}, {50, kPreparation, 3},
    {51, kTrailblazer, 1}, {52, kTrailblazer, 2}, {53, kTrailblazer, 3},
};

const SkillEntry* FindSkill(int id)
{
    for (const SkillEntry& s : kSkills)
        if (s.id == id) return &s;
    return nullptr;
}

// 一组要几个食材才到这个档；档位越高要得越多。
int TierMin(int tier)
{
    if (tier <= 1) return 2;
    if (tier == 2) return 4;
    return 6;
}

// 数值档位（无/小/中/大）对应的数量下界：0 → 0~1 个，1 → 2~3 个，2 → 4~5 个，3 → 6 个。
int LevelMin(int level) { return level <= 0 ? 0 : TierMin(level); }
int LevelMax(int level) { return level <= 0 ? 1 : (level == 3 ? 6 : TierMin(level) + 1); }

struct Req
{
    int group;
    int tier;
};

enum Fit
{
    kFitOk = 0,
    kFitTooMany,    // 食材根本不够
    kFitNoRoom      // 食材够，但多出来的只能塞进已经用过的组，会多出别的技能
};

// 给定每类食材数量和要凑的技能，判断能不能凑出来。
// 同一组的东西可以任意分给肉/鱼/菜（或全是饮料），所以只要各类的总数够、分组不挤爆就行。
Fit Feasible(int meat, int fish, int veg, int drink, const Req* req, int reqCount, int hpTier)
{
    const int shared = meat + fish + veg;
    int minShared = 0, minDrink = 0, usedShared = 0, usedDrink = 0;
    for (int i = 0; i < reqCount; ++i) {
        if (req[i].group < kDrinkFirst) {
            minShared += TierMin(req[i].tier);
            ++usedShared;
        } else {
            minDrink += TierMin(req[i].tier);
            ++usedDrink;
        }
    }
    if (minShared > shared || minDrink > drink) return kFitTooMany;
    // 新鲜食材带来体力，一个 +10。
    if (shared + drink < hpTier) return kFitTooMany;
    // 多出来的食材只能一个一组地放（再多一个同组就又多出一个技能了）。
    if (shared - minShared > kSharedGroups - usedShared) return kFitNoRoom;
    if (drink - minDrink > kDrinkGroups - usedDrink) return kFitNoRoom;
    return kFitOk;
}
}  // namespace

bool IsIngredientSkill(int id) { return FindSkill(id) != nullptr; }

bool Neutral(int hpTier, int staminaTier, int attack, int defense, int elemRes, const int (&skills)[3])
{
    return hpTier <= 0 && staminaTier <= 0 && attack <= 0 && defense <= 0 && elemRes <= 0 &&
           skills[0] < 0 && skills[1] < 0 && skills[2] < 0;
}

Result Check(int hpTier, int staminaTier, int attack, int defense, int elemRes, const int (&skills)[3])
{
    Result r;
    r.verdict = Verdict::Impossible;

    // 负数只是「没有这一项」，按 0 算；超出上界才是真的写错了。
    if (hpTier < 0) hpTier = 0;
    if (staminaTier < 0) staminaTier = 0;
    if (attack < 0) attack = 0;
    if (defense < 0) defense = 0;
    if (elemRes < 0) elemRes = 0;

    if (hpTier > 5 || staminaTier > 2 ||
        attack > 3 || defense > 3 || elemRes > 3) {
        r.reason = Reason::Range;
        return r;
    }
    for (int id : skills) {
        if (id < -1 || id > 56) {
            r.reason = Reason::Range;
            return r;
        }
    }

    // speedrun_util 判定函数开头就是这条：耐力不是 +50 直接不合法。
    if (staminaTier != 2) {
        r.reason = Reason::Stamina;
        return r;
    }

    Req req[kGroupCount];
    int reqCount = 0;
    int chosen[3];
    int chosenCount = 0;
    int daily = 0;
    for (int id : skills) {
        if (id < 0) continue;
        for (int j = 0; j < chosenCount; ++j) {
            if (chosen[j] == id) {
                r.reason = Reason::Duplicate;
                return r;
            }
        }
        chosen[chosenCount++] = id;

        const SkillEntry* s = FindSkill(id);
        if (!s) {
            ++daily;
            if (r.dailySkill < 0) r.dailySkill = id;
            continue;
        }
        for (int j = 0; j < reqCount; ++j) {
            if (req[j].group == s->group) {
                r.reason = Reason::SameGroup;
                return r;
            }
        }
        req[reqCount].group = s->group;
        req[reqCount].tier = s->tier;
        ++reqCount;
    }

    // 硬编码互斥对：50 与 43 不能同时出现。
    bool hasA = false, hasB = false;
    for (int id : skills) {
        if (id == kExclusiveA) hasA = true;
        if (id == kExclusiveB) hasB = true;
    }
    if (hasA && hasB) {
        r.reason = Reason::Exclusive;
        return r;
    }

    r.dailyCount = daily;
    const int aMin = LevelMin(attack), aMax = LevelMax(attack);
    const int dMin = LevelMin(defense), dMax = LevelMax(defense);
    const int rMin = LevelMin(elemRes), rMax = LevelMax(elemRes);

    int minNeeded = 0;
    for (int i = 0; i < reqCount; ++i) minNeeded += TierMin(req[i].tier);
    if (const int buffMin = aMin + dMin + rMin; buffMin > minNeeded) minNeeded = buffMin;

    bool noRoom = false;
    for (int meat = aMin; meat <= aMax; ++meat) {
        for (int fish = dMin; fish <= dMax; ++fish) {
            for (int veg = rMin; veg <= rMax; ++veg) {
                for (int drink = 0; drink + meat + fish + veg <= 6; ++drink) {
                    switch (Feasible(meat, fish, veg, drink, req, reqCount, hpTier)) {
                    case kFitOk:
                        r.verdict = daily > 0 ? Verdict::NeedsDaily : Verdict::Legal;
                        r.reason = daily > 0 ? Reason::Daily : Reason::None;
                        return r;
                    case kFitNoRoom:
                        noRoom = true;
                        break;
                    default:
                        break;
                    }
                }
            }
        }
    }

    r.verdict = Verdict::Impossible;
    if (noRoom && minNeeded <= 6) {
        r.reason = Reason::BuffCrowd;
        return r;
    }
    r.reason = Reason::TooMany;
    r.needed = minNeeded > 6 ? minNeeded : 0;
    return r;
}
}  // namespace canteen
