#include "palico_memory.h"
namespace palico {
namespace {
bool Entry(const Reader& r, std::uintptr_t container, unsigned index, std::uintptr_t& out)
{
    unsigned n=0; std::uintptr_t array=0;
    return container && r.Get(container+0xb8,n) && index<n && n<=4096 &&
        r.Get(container+0xc8,array) && array && r.Get(array+index*8,out) && out;
}
}
bool ReadSaveSlot(const Reader& r, std::uintptr_t exe, std::uintptr_t& slot, int& index)
{
    std::uintptr_t root=0,base=0; std::uint64_t i=0;
    if (!r.Get(exe+0x5013950,root) || !root || !r.Get(root+0xa8,base) || !base ||
        !r.Get(root+0xa0,i) || i>2) return false;
    slot=base+std::uintptr_t(i)*0x26cc00; index=int(i); return true;
}
bool ReadOwnedCoral(const Reader& r, std::uintptr_t exe, std::uintptr_t obj, Snapshot& out)
{
    Snapshot s;
    int owner=0,player=0,local=0,gadget=0,level=0;
    std::uintptr_t manager=0,area=0; unsigned char mode=0;
    if (!obj || !r.Get(obj+0x7630,owner) || owner!=1 || !r.Get(obj+0xa244,player) ||
        !r.Get(exe+0x500ec40,manager) || !manager || !r.Get(exe+0x51c42b8,area) || !area ||
        !r.Get(area+0xd2ea,mode) || !r.Get(manager+(mode==1?0x248:0x24c),local) || player!=local ||
        !r.Get(obj+0x7f7c,gadget) || gadget!=3 || !r.Get(obj+0x8040+gadget*4,level) || level<0 || level>14 ||
        !r.Get(exe+0x34a2fd0+level*12,s.parameter[0]) ||
        !r.Get(exe+0x34a2fd4+level*12,s.parameter[1]) ||
        !r.Get(obj+0xa4f0,s.flags) || !r.Bytes(obj+0xa508,s.timers.data(),sizeof(float)*30)) return false;
    std::uintptr_t table=0,entry=0;
    if (!r.Get(obj+0xa5e8,table)) return false;
    if (table) {
        for(int k=0;k<2;++k) {
            int p=s.parameter[k];
            unsigned size=0; std::uintptr_t entries=0;
            if(!r.Get(table+0xb8,size) || size>4096) return false;
            if(p>=1 && unsigned(p-1)<size) {
                if(!r.Get(table+0xc8,entries)) return false;
                if(entries) {
                    if(!r.Get(entries+unsigned(p-1)*8,entry)) return false;
                    if(entry) {
                        if(!r.Get(entry+(k==0?0xe:0x14),s.cooldown[k])) return false;
                        s.metadata[k]=true;
                    }
                }
            }
        }
    }
    std::uintptr_t status=0,actor=0,stats=0; unsigned char actorFlags=0;
    if(!r.Get(obj+0x8090,status) || (status && !r.Get(status+0x90,s.status))) return false;
    if(!r.Get(obj+0x7638,actor)) return false;
    if(actor && !r.Get(actor+0xc,actorFlags)) return false;
    if(actor && (actorFlags&0xe)) {
        if(!r.Get(actor+0x7d20,stats) || !stats) return false;
        if(!r.Get(stats+0x5dc,s.healthConditionA) || !r.Get(stats+0x5e0,s.healthConditionB)) return false;
    }
    out=s; return true;
}
bool ReadExperienceTable(const Reader& r, std::uintptr_t exe, int gadget, int* values, int& count)
{
    if(!values || gadget < -1 || gadget>5) return false;
    std::uintptr_t resource=0,entry=0;
    if(!r.Get(exe+(gadget<0?0x50121d0:0x5012228),resource) || !resource) return false;
    if(gadget<0) {
        unsigned n=0;
        if(!r.Get(resource+0xb8,n) || n==0 || n>200 || int(n)>count) return false;
        for(unsigned i=0;i<n;++i) if(!Entry(r,resource,i,entry) || !r.Get(entry+0xc,values[i])) return false;
        count=int(n);
    } else {
        if(count<15 || !Entry(r,resource,unsigned(gadget),entry)) return false;
        unsigned short xp[15]{};
        if(!r.Bytes(entry+8,xp,sizeof(xp))) return false;
        for(int i=0;i<15;++i) values[i]=xp[i];
        count=15;
    }
    // These entries are per-level COSTS, not cumulative thresholds.
    for(int i=0;i<count;++i) if(values[i]<0) return false;
    return true;
}
} // namespace palico
