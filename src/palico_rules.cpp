#include "palico_rules.h"
#include <limits>

namespace palico {
const std::array<Song, kSongCount>& Songs()
{
    static const std::array<Song, kSongCount> songs{{
        {0x3a,1,1,"attack_up_s"}, {0x3c,1,1,"defense_up_s"},
        {0x3e,1,2,"affinity_up"}, {0x3f,1,2,"recovery_up"},
        {0x40,1,3,"health_up"}, {0x41,1,3,"stamina_up"},
        {0x3b,1,4,"attack_up_l"}, {0x3d,1,4,"defense_up_l"},
        {0x42,2,1,"status_recovery"}, {0x43,2,1,"divine_protection"},
        {0x4a,2,2,"environment_negated"}, {0x45,2,2,"stun_negated"},
        {0x46,2,3,"paralysis_negated"}, {0x47,2,3,"wind_negated"},
        {0x48,2,4,"earplugs"}, {0x49,2,4,"tremor_negated"}
    }};
    return songs;
}
int SongIndex(int id)
{
    for (int i = 0; i < kSongCount; ++i) if (Songs()[i].gameId == id) return i;
    return -1;
}
bool ExperienceForLevel(const int* costs, int count, int level, int& experience)
{
    if (!costs || count < 1 || level < 1 || level > count) return false;
    std::int64_t sum = 0;
    for (int i = 0; i < level-1; ++i) {
        if (costs[i] < 0) return false;
        sum += costs[i];
        if (sum > (std::numeric_limits<int>::max)()) return false;
    }
    experience = int(sum); return true;
}
int LevelForExperience(const int* costs, int count, int experience)
{
    if (!costs || count < 1 || experience < 0) return 0;
    std::int64_t sum = 0; int crossed = 0;
    for (int i = 0; i < count; ++i) {
        if (costs[i] < 0) return 0;
        sum += costs[i];
        if (experience < sum) break;
        ++crossed;
    }
    return (crossed < count ? crossed : count-1)+1;
}
Choice Select(const Policy& p, const Request& r, bool first, std::uint32_t random)
{
    auto valid = [](int i) { return i >= 0 && i < kSongCount; };
    auto bit = [&](int i) { return valid(i) && (r.candidates & (1u << i)) != 0; };
    if (r.forced >= 0) {
        if (valid(r.forced) && p.banned[r.forced] && p.skipAllBanned) return {-1,false};
        return {r.forced,true};
    }
    if (r.slot == 1 && first && p.firstSong >= 0) return {p.firstSong,bit(p.firstSong)};
    std::uint32_t allowed = r.candidates & 0xffffu;
    for (int i = 0; i < kSongCount; ++i) if (p.banned[i]) allowed &= ~(1u << i);
    int best = -1, rank = (std::numeric_limits<int>::max)();
    for (int i = 0; i < kSongCount; ++i) {
        if (!(allowed & (1u << i)) || p.rank[i] <= 0) continue;
        if (best < 0 || p.rank[i] < rank || (p.rank[i] == rank && i == r.original)) {
            best = i; rank = p.rank[i];
        }
    }
    if (best >= 0) return {best,true};
    if (valid(r.original) && !p.banned[r.original]) return {r.original,true};
    // Weights depend on position among ALL available vanilla candidates, not
    // just the unbanned ones. Banning a song must not renumber the distribution.
    int all[kSongCount]{}, n = 0;
    const int begin = r.slot == 1 ? 0 : 9, end = r.slot == 1 ? 8 : 16;
    for (int i = begin; i < end; ++i) if (bit(i)) all[n++] = i;
    unsigned total = 0;
    for (int j = 0; j < n; ++j) if (allowed & (1u << all[j]))
        total += unsigned(r.slot == 1 ? 2*j+1 : 2*(n-j)-1);
    if (total) {
        unsigned roll = random % total;
        for (int j = 0; j < n; ++j) {
            if (!(allowed & (1u << all[j]))) continue;
            unsigned w = unsigned(r.slot == 1 ? 2*j+1 : 2*(n-j)-1);
            if (roll < w) return {all[j],true};
            roll -= w;
        }
    }
    if (p.skipAllBanned && r.candidates) return {-1,false};
    return {r.original,true};
}
Request Candidates(const Snapshot& s, int slot, int originalGameId)
{
    Request r; r.slot = slot; r.original = SongIndex(originalGameId);
    if (slot != 1 && slot != 2) return r;
    const int param = s.parameter[slot-1];
    const int count = param == 2 ? 4 : param == 3 ? 6 : (param == 4 || param == 5) ? 8 : 2;
    auto available = [&](int timer, float limit) {
        // Preserve COMISS ordered comparison: NaN is not a ready cooldown.
        return !(s.flags & (1u << timer)) || s.timers[timer] >= limit;
    };
    if (slot == 1) {
        if (s.metadata[0]) for (int i = 0; i < count; ++i)
            if (available(14+i,float(s.cooldown[0]))) r.candidates |= 1u << i;
        return r;
    }
    const float limit = s.metadata[1] ? float(s.cooldown[1]) : 600.0f;
    if ((s.healthConditionA > 0 || s.healthConditionB > 0) && available(22,limit)) {
        r.forced = 8; r.candidates = 1u << 8; return r;
    }
    constexpr int special[5][3] = {{0,2,25},{1,3,26},{2,3,27},{3,4,28},{4,4,29}};
    for (const auto& x : special) if ((s.status & (1u << x[0])) && param >= x[1] && available(x[2],limit)) {
        r.forced = x[0]+11; r.candidates = 1u << r.forced; return r;
    }
    if (s.metadata[1]) for (int i = 0; i < count-1; ++i)
        if (available(23+i,float(s.cooldown[1]))) r.candidates |= 1u << (9+i);
    return r;
}
} // namespace palico
