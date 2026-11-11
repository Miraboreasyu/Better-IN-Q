#include "palico_rules.h"
#include <cstdio>
#include <limits>
int main()
{
    int failures = 0;
    auto check = [&](bool v, const char* msg) { if (!v) { ++failures; std::printf("FAIL %s\n",msg); } };
    using namespace palico;
    const int costs[]={100,200,50,400}; int xp=-1;
    check(ExperienceForLevel(costs,4,1,xp)&&xp==0,"level one uses zero XP");
    check(ExperienceForLevel(costs,4,4,xp)&&xp==350,"XP sums nonmonotonic level costs");
    check(LevelForExperience(costs,4,99)==1&&LevelForExperience(costs,4,100)==2&&
          LevelForExperience(costs,4,350)==4,"XP threshold boundaries");
    check(!ExperienceForLevel(costs,4,5,xp),"out-of-range target refused");
    const int overflow[]={(std::numeric_limits<int>::max)(),1,1};
    check(!ExperienceForLevel(overflow,3,3,xp),"XP sum overflow refused");
    Policy p; Snapshot s;
    s.parameter[0]=s.parameter[1]=1; s.metadata[0]=s.metadata[1]=true;
    auto r=Candidates(s,1,0x3a);
    check(r.candidates==3 && r.original==0,"initial first-slot pool");
    check(Candidates(s,2,0x43).candidates==(1u<<9),"initial second-slot pool");
    for (int param=1;param<=5;++param) {
        s.parameter[0]=s.parameter[1]=param;
        int n=param==1?2:param==2?4:param==3?6:8;
        check(Candidates(s,1,-1).candidates==((1u<<n)-1),"first-slot level pool");
        check(Candidates(s,2,-1).candidates==(((1u<<(n-1))-1)<<9),"second-slot level pool");
    }
    s.flags=1u<<14; s.cooldown[0]=600; s.timers[14]=599;
    check(!(Candidates(s,1,-1).candidates&1),"cooldown blocks played song");
    s.timers[14]=600; check((Candidates(s,1,-1).candidates&1)!=0,"cooldown equality allows");
    s.timers[14]=(std::numeric_limits<float>::quiet_NaN)();
    check(!(Candidates(s,1,-1).candidates&1),"NaN cooldown fails closed");
    s.healthConditionA=1;
    check(Candidates(s,2,-1).forced==8,"health forced status song");
    s.flags|=1u<<22; s.cooldown[1]=600; s.timers[22]=0; s.status=1;
    check(Candidates(s,2,-1).forced==11,"status priority when health cooldown active");
    s.status=3; check(Candidates(s,2,-1).forced==11,"first special status has precedence");
    r={1,0xff,-1,0};
    check(Select(p,r,false,0).index==0,"neutral keeps original");
    p.firstSong=7; auto c=Select(p,r,true,0);
    check(c.index==7&&c.legal,"first song override");
    r.candidates=3; c=Select(p,r,true,0);
    check(c.index==7&&!c.legal,"unavailable first song reported illegal");
    p=Policy(); p.rank[0]=p.rank[1]=2; r.original=1;
    check(Select(p,r,false,0).index==1,"equal ranks prefer original");
    p.rank[0]=1; check(Select(p,r,false,0).index==0,"smaller positive rank wins");
    p.banned[0]=true; check(Select(p,r,false,0).index==1,"ban excludes ranked song");
    p=Policy(); p.banned[0]=true; r={1,15,-1,0};
    // Vanilla positions have weights 1,3,5,7. The banned position's weight is removed only.
    int counts[4]{};
    for(unsigned i=0;i<15;++i) ++counts[Select(p,r,false,i).index];
    check(counts[1]==3&&counts[2]==5&&counts[3]==7,"original positional weights preserved");
    p.banned.fill(true); p.skipAllBanned=true;
    check(Select(p,r,false,0).index==-1,"all banned skip");
    r.forced=8; check(Select(p,r,false,0).index==-1,"forced banned skip");
    p.skipAllBanned=false; check(Select(p,r,false,0).index==8,"forced priority ignores ordinary ban");
    check(SongIndex(0x3b)==6&&SongIndex(0x49)==15&&SongIndex(-1)==-1,"game ID mapping");
    std::printf("palico rules failures: %d\n",failures); return failures?1:0;
}
