#include "../src/palico_apply.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
struct Fake {
    static constexpr std::uintptr_t slot=0x100000, exe=0x140000000;
    std::vector<unsigned char> data=std::vector<unsigned char>(palico::kPalicoSlotSize);
    std::vector<unsigned char> saved;
    bool idle=true, durable=true, locked=false, busy=false, mutate=false, switchSlot=false, changeContext=false;
    int writes=0, backups=0, locks=0, unlocks=0, failWrite=0;
    bool failRollback=false, failRead=false, missingTable=false, partialWrite=false, corruptWrite=false, leaveIdleOnWrite=false;
    palico::ApplyContext current{slot,0x20000,0x30000,0x40000,1};
    void put(std::size_t off,int v) { std::memcpy(data.data()+off,&v,4); }
    int get(std::size_t off) { int v; std::memcpy(&v,data.data()+off,4); return v; }
    static bool Read(std::uintptr_t at,void* p,std::size_t n,void* arg) {
        auto& f=*static_cast<Fake*>(arg);
        if(f.failRead) return false;
        if(f.missingTable && at>=exe) return false;
        if(at>=slot && at-slot<=f.data.size() && n<=f.data.size()-(at-slot)) {
            std::memcpy(p,f.data.data()+(at-slot),n); return true;
        }
        // Fake cost resources: main table with 50 entries, gadget table six entries.
        auto copy=[&](auto v) { if(n!=sizeof(v)) return false; std::memcpy(p,&v,n); return true; };
        if(at==exe+0x50121d0) return copy(std::uintptr_t(0x10000000));
        if(at==exe+0x5012228) return copy(std::uintptr_t(0x10001000));
        if(at==0x100000b8) return copy(unsigned(50));
        if(at==0x100010b8) return copy(unsigned(6));
        if(at==0x100000c8) return copy(std::uintptr_t(0x10002000));
        if(at==0x100010c8) return copy(std::uintptr_t(0x10003000));
        if(at>=0x10002000 && at<0x10002000+50*8) return copy(std::uintptr_t(0x10010000+(at-0x10002000)*8));
        if(at>=0x10003000 && at<0x10003000+6*8) return copy(std::uintptr_t(0x10020000+(at-0x10003000)*8));
        if(at>=0x10010000 && at<0x10020000 && (at-0x10010000)%64==12) return copy(int(10));
        if(at>=0x10020000 && at<0x10021000 && n==30) {
            unsigned short costs[15]; for(auto& v:costs) v=5;
            std::memcpy(p,costs,30); return true;
        }
        return false;
    }
    static bool Context(void* arg,palico::ApplyContext& c) {
        auto& f=*static_cast<Fake*>(arg); c=f.current; return f.idle;
    }
    static bool Write(void* arg,std::uintptr_t at,const void* p,std::size_t n) {
        auto& f=*static_cast<Fake*>(arg); assert(f.locked); assert(n==4); ++f.writes;
        assert(at==slot+0x1026f8 || at==slot+0x10265c || (at>=slot+0x102660 && at<slot+0x102678));
        if(f.failWrite && f.writes>=f.failWrite && (f.writes==f.failWrite || f.failRollback)) {
            if(f.partialWrite) std::memcpy(f.data.data()+at-slot,p,1);
            return false;
        }
        std::memcpy(f.data.data()+at-slot,p,n);
        if(f.corruptWrite && f.writes==1) f.data[at-slot]++;
        if(f.leaveIdleOnWrite && f.writes==1) f.idle=false;
        return true;
    }
    static bool Lock(void* arg,std::uintptr_t at) {
        auto& f=*static_cast<Fake*>(arg); assert(at==slot+8); ++f.locks;
        if(f.busy) return false;
        f.locked=true;
        if(f.mutate) f.data[0x123]++;
        if(f.switchSlot) f.current.slotIndex=2;
        if(f.changeContext) f.current.ui++;
        // Lock state changes are intentionally excluded from comparisons.
        f.data[8]++;
        return true;
    }
    static void Unlock(void* arg,std::uintptr_t) { auto& f=*static_cast<Fake*>(arg); assert(f.locked); f.locked=false; ++f.unlocks; }
    static bool Backup(void* arg,const palico::ApplyContext& c,const void* p,std::size_t n,
                       const std::filesystem::path&,std::filesystem::path& out) {
        auto& f=*static_cast<Fake*>(arg); assert(!f.locked); assert(c.slot==slot); assert(n==palico::kPalicoSlotSize);
        ++f.backups; f.saved.assign(static_cast<const unsigned char*>(p),static_cast<const unsigned char*>(p)+n);
        out="test.biqbak"; return f.durable;
    }
    palico::ApplyBackend backend() {
        palico::ApplyBackend b; b.reader={Read,this}; b.exe=exe; b.user=this;
        b.context=Context; b.write=Write; b.tryLock=Lock; b.unlock=Unlock; b.backup=Backup; return b;
    }
    palico::Result apply(palico::Options o) { return palico::ApplyWithBackend(o,"backups",backend()); }
};
}
int main() {
    { Fake f; auto r=f.apply({}); assert(r.ok && !r.changed && !f.backups); }
    { Fake f; auto r=f.apply({2,0,-1,0}); assert(!r.ok && !f.backups && !f.writes); }
    { Fake f; f.idle=false; auto r=f.apply({0,0,-1,0}); assert(!r.ok && !f.backups); }
    { Fake f; f.durable=false; auto r=f.apply({0,0,-1,0}); assert(!r.ok && !f.writes && !f.locks); }
    { Fake f; f.busy=true; auto r=f.apply({0,0,-1,0}); assert(!r.ok && f.backups==1 && !f.writes && !f.unlocks); }
    { Fake f; f.mutate=true; auto r=f.apply({0,0,-1,0}); assert(!r.ok && !f.writes && f.unlocks==1); }
    { Fake f; f.switchSlot=true; auto r=f.apply({0,0,-1,0}); assert(!r.ok && !f.writes); }
    { Fake f; f.changeContext=true; auto r=f.apply({0,0,-1,0}); assert(!r.ok && !f.writes); }
    { Fake f; f.put(0x1026f8,0x12345680); auto r=f.apply({0,0,-1,0}); assert(r.ok && r.changed);
      assert(f.get(0x1026f8)==0x12345690 && f.saved.size()==palico::kPalicoSlotSize && f.writes==1); }
    { Fake f; f.put(0x1026f8,0x12345690); auto r=f.apply({1,0,-1,0}); assert(r.ok && f.get(0x1026f8)==0x12345680); }
    { Fake f; auto r=f.apply({1,0,-1,0}); assert(r.ok && !r.changed && !f.backups); }
    { Fake f; auto r=f.apply({0,50,-1,15}); assert(r.ok && f.get(0x10265c)==490 && f.get(0x102660)==70 && f.writes==3); }
    { Fake f; f.put(0x2d4,5); auto r=f.apply({-1,0,5,15}); assert(r.ok && f.get(0x102674)==70 && !f.get(0x102660)); }
    { Fake f; auto r=f.apply({0,50,4,15}); assert(!r.ok && !f.backups && !f.writes && !f.get(0x2d4)); }
    { Fake f; f.put(0x2d4,6); auto r=f.apply({-1,0,-1,15}); assert(!r.ok && !f.backups); }
    { Fake f; f.failWrite=2; auto r=f.apply({0,50,-1,0}); assert(!r.ok && !r.changed && !f.get(0x1026f8) && !f.get(0x10265c));
      assert(f.unlocks==1 && !r.backupPath.empty()); }
    { Fake f; f.failWrite=2; f.failRollback=true; auto r=f.apply({0,50,-1,0});
      assert(!r.ok && r.error.find("rollback failed")!=std::string::npos && !r.backupPath.empty()); }
    { Fake f; f.failRead=true; auto r=f.apply({0,0,-1,0}); assert(!r.ok && !f.backups && !f.writes); }
    { Fake f; f.missingTable=true; auto r=f.apply({0,50,-1,0}); assert(!r.ok && !f.backups && !f.writes); }
    { Fake f; f.failWrite=2; f.partialWrite=true; auto r=f.apply({0,50,-1,0});
      assert(!r.ok && !f.get(0x10265c) && !f.get(0x1026f8)); }
    { Fake f; f.corruptWrite=true; auto r=f.apply({0,0,-1,0}); assert(!r.ok && !f.get(0x1026f8) && f.unlocks==1); }
    { Fake f; f.leaveIdleOnWrite=true; auto r=f.apply({0,50,-1,0}); assert(!r.ok && !f.get(0x1026f8) && !f.get(0x10265c)); }
    { Fake f; f.put(0x10265c,490); f.put(0x102660,70); auto r=f.apply({-1,50,-1,15}); assert(r.ok && !r.changed && !f.backups); }
    { Fake f; f.put(0x1026f8,0x12345680); f.put(0x10265c,100); f.put(0x102660,1); f.put(0x102664,2);
      f.put(0x102668,3); f.put(0x10266c,4); f.put(0x102670,5); f.put(0x102674,6);
      palico::SaveOriginal orig; palico::ApplyContext c; assert(f.Context(&f,c));
      assert(palico::CaptureSaveFields(f.backend(),c,orig) && orig.valid);
      assert(orig.deploy==0x12345680u && orig.xp==100 && orig.gadgetXp[0]==1 && orig.gadgetXp[5]==6);
      f.put(0x1026f8,0x12345690); f.put(0x10265c,490); f.put(0x102660,7); f.put(0x102664,8);
      f.put(0x102668,9); f.put(0x10266c,10); f.put(0x102670,11); f.put(0x102674,12);
      auto r=palico::RestoreWithBackend(orig,"backups",f.backend());
      assert(r.ok && r.changed && f.backups==1 && f.get(0x1026f8)==0x12345680u && f.get(0x10265c)==100);
      assert(f.get(0x102660)==1 && f.get(0x102674)==6); }
    { Fake f; palico::SaveOriginal o; auto r=palico::RestoreWithBackend(o,"backups",f.backend()); assert(!r.ok && !f.backups); }
    { Fake f; f.put(0x1026f8,0x12345680); f.put(0x10265c,100);
      palico::SaveOriginal orig; palico::ApplyContext c; assert(f.Context(&f,c));
      assert(palico::CaptureSaveFields(f.backend(),c,orig));
      auto r=palico::RestoreWithBackend(orig,"backups",f.backend());
      assert(r.ok && !r.changed && f.backups==0); }
    { Fake f; auto b=f.backend(); b.write=nullptr; auto r=palico::ApplyWithBackend({0,0,-1,0},"backups",b); assert(!r.ok && !f.backups); }
    { Fake f; f.put(0x13b680,3); auto r=f.apply({-1,0,-1,15,1}); assert(r.ok && f.get(0x10266c)==70 && !f.get(0x102660) && f.writes==1); }
    std::cout << "palico_apply_test: 29 cases passed\n";
}
