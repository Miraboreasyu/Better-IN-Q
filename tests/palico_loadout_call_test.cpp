// White-box regression for the original DLL's pointer-to-object call contract.
#include "../src/palico_apply.cpp"
#include <cassert>
#include <map>
#include <iostream>

namespace {
struct Fixture {
    std::map<std::uintptr_t, std::uintptr_t> words;
    std::uintptr_t exe=0x140000000, object=0x900000, slot=0xa00000;
    unsigned char indexObject[0x280]{}, pageObject[0x250]{};
    int calls=0, expectedIndex=0, expectedPage=0;
    Fixture() {
        words[exe+0x51c4640]=0x100000;
        words[0x100000+0x278]=0x200000;
        words[0x200000+0x62*8]=object;
        words[object]=exe+0x346cfe0;
        words[object+0x2958]=reinterpret_cast<std::uintptr_t>(indexObject);
        words[object+0x2960]=reinterpret_cast<std::uintptr_t>(pageObject);
        words[exe+0x5013950]=0x300000;
        words[0x300000+0xa8]=slot;
        words[0x300000+0xa0]=0;
        for(int i=0;i<24;++i) words[slot+0x13b674+i*0x130]=123;
        Set(indexObject+0x274,7); Set(pageObject+0x240,2);
    }
    static void Set(void* p,unsigned n) { std::memcpy(p,&n,4); }
    static unsigned Get(const void* p) { unsigned n;std::memcpy(&n,p,4);return n; }
    static bool Read(std::uintptr_t address,void* out,std::size_t n,void* ctx) {
        auto& f=*static_cast<Fixture*>(ctx);
        if (address==reinterpret_cast<std::uintptr_t>(f.indexObject+0x274) ||
            address==reinterpret_cast<std::uintptr_t>(f.pageObject+0x240)) {
            if(n!=4) return false;
            std::memcpy(out,reinterpret_cast<void*>(address),n);return true;
        }
        auto p=f.words.find(address);
        if(p==f.words.end() || n>sizeof p->second) return false;
        std::memcpy(out,&p->second,n);return true;
    }
};
Fixture* current=nullptr;
std::uintptr_t GameApply(void* objectAddress,std::uintptr_t a,std::uintptr_t b) {
    assert(current && objectAddress && !a && !b);
    assert(*static_cast<std::uintptr_t*>(objectAddress)==current->object);
    assert(Fixture::Get(current->indexObject+0x274)==unsigned(current->expectedIndex));
    assert(Fixture::Get(current->pageObject+0x240)==unsigned(current->expectedPage));
    ++current->calls; return 0;
}
}
int main() {
    Fixture f; current=&f;
    palico::g_loadoutApply=reinterpret_cast<std::uintptr_t>(&GameApply);
    palico::Reader reader{Fixture::Read,&f};std::string error;
    for(int n : {1,8,9,24}) {
        f.expectedIndex=(n-1)%8;f.expectedPage=(n-1)/8;
        assert(palico::ApplyLoadoutGame(reader,f.exe,n,error));
        assert(error.empty());
        assert(Fixture::Get(f.indexObject+0x274)==7 && Fixture::Get(f.pageObject+0x240)==2);
    }
    assert(f.calls==4);
    f.words[f.object]=0;
    assert(!palico::ApplyLoadoutGame(reader,f.exe,1,error) && f.calls==4);
    f.words[f.object]=f.exe+0x346cfe0;
    f.words[f.slot+0x13b674]=0xffffffffu;
    assert(!palico::ApplyLoadoutGame(reader,f.exe,1,error) && f.calls==4);
    f.words[f.slot+0x13b674]=123;
    f.words[f.object+0x2958]=0;
    assert(!palico::ApplyLoadoutGame(reader,f.exe,1,error) && f.calls==4);
    assert(!palico::ApplyLoadoutGame(reader,f.exe,0,error) && f.calls==4);
    assert(!palico::ApplyLoadoutGame(reader,f.exe,25,error) && f.calls==4);
    std::cout << "palico_loadout_call_test: original ABI, pages, restoration and reject guards passed\n";
}
