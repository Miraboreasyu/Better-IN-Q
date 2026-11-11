#include "palico_memory.h"
#include <map>
#include <cstring>
#include <cstdio>
struct Memory {
    std::map<std::uintptr_t,unsigned char> bytes;
    template<class T> void Put(std::uintptr_t a,T v) { const auto* p=(const unsigned char*)&v; for(std::size_t i=0;i<sizeof(v);++i) bytes[a+i]=p[i]; }
    static bool Read(std::uintptr_t a,void* p,std::size_t n,void* c) {
        auto& m=*(Memory*)c; auto* b=(unsigned char*)p;
        for(std::size_t i=0;i<n;++i) { auto it=m.bytes.find(a+i); if(it==m.bytes.end()) return false; b[i]=it->second; } return true;
    }
};
int main() {
    int fails=0; auto check=[&](bool b,const char* s){if(!b){++fails;std::printf("FAIL %s\n",s);}};
    Memory m; palico::Reader r{Memory::Read,&m};
    constexpr std::uintptr_t exe=0x140000000, obj=0x300000, mgr=0x400000, area=0x500000;
    palico::Snapshot s; s.flags=123;
    check(!palico::ReadOwnedCoral(r,exe,obj,s)&&s.flags==123,"failed snapshot does not publish partial state");
    m.Put(obj+0x7630,1);m.Put(obj+0xa244,2);m.Put(exe+0x500ec40,mgr);m.Put(exe+0x51c42b8,area);
    m.Put(area+0xd2ea,(unsigned char)1);m.Put(mgr+0x248,2);m.Put(obj+0x7f7c,3);m.Put(obj+0x804c,6);
    m.Put(exe+0x34a2fd0+6*12,3);m.Put(exe+0x34a2fd4+6*12,2);m.Put(obj+0xa4f0,0u);
    for(int i=0;i<30;++i)m.Put(obj+0xa508+i*4,0.f);
    m.Put(obj+0xa5e8,std::uintptr_t(0)); m.Put(obj+0x8090,std::uintptr_t(0)); m.Put(obj+0x7638,std::uintptr_t(0));
    check(palico::ReadOwnedCoral(r,exe,obj,s)&&s.parameter[0]==3&&!s.metadata[0],"owned coral snapshot with absent resource");
    for (auto offset : {0xa5e8,0x8090,0x7638}) {
        m.bytes.erase(obj+offset); s.flags=123;
        check(!palico::ReadOwnedCoral(r,exe,obj,s)&&s.flags==123,"unreadable pointer does not publish snapshot");
        m.Put(obj+offset,std::uintptr_t(0));
    }
    m.Put(obj+0xa244,3); check(!palico::ReadOwnedCoral(r,exe,obj,s),"foreign palico rejected");m.Put(obj+0xa244,2);
    m.Put(obj+0x7f7c,2);check(!palico::ReadOwnedCoral(r,exe,obj,s),"other gadget rejected");m.Put(obj+0x7f7c,3);
    m.Put(obj+0x804c,15);check(!palico::ReadOwnedCoral(r,exe,obj,s),"invalid gadget level rejected");
    std::uintptr_t slot=0;int index=-1;
    m.Put(exe+0x5013950,mgr);m.Put(mgr+0xa8,area);m.Put(mgr+0xa0,std::uint64_t(2));
    check(palico::ReadSaveSlot(r,exe,slot,index)&&index==2&&slot==area+2*0x26cc00,"third save slot bounded");
    m.Put(mgr+0xa0,std::uint64_t(3));check(!palico::ReadSaveSlot(r,exe,slot,index),"out-of-range slot refused");
    std::printf("palico memory failures: %d\n",fails);return fails?1:0;
}
