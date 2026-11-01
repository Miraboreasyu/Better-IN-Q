#include "../src/settings_transfer.h"
#include "../src/profile.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

static int failures=0;
#define CHECK(c,m) do { if(!(c)){std::printf("FAIL: %s\n",m);++failures;} else std::printf("ok: %s\n",m); }while(0)
using namespace settings_transfer;
static bool Write(const std::wstring& p,const std::string& s) {
    HANDLE h=CreateFileW(p.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);
    if(h==INVALID_HANDLE_VALUE)return false;DWORD n=0;bool ok=!!WriteFile(h,s.data(),(DWORD)s.size(),&n,nullptr)&&n==s.size();CloseHandle(h);return ok;
}
static std::string Read(const std::wstring& p) {
    HANDLE h=CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(h==INVALID_HANDLE_VALUE)return "<missing>";std::string s;char buf[4096];DWORD n=0;
    while(ReadFile(h,buf,sizeof(buf),&n,nullptr)&&n)s.append(buf,n);CloseHandle(h);return s;
}
static unsigned long Hash(const std::string& s){unsigned long h=2166136261UL;for(unsigned char c:s)h=(h^c)*16777619UL;return h;}
static std::string Pack(const std::vector<std::string>& parts) {
    std::string b="BIQ-SETTINGS\n1\n";for(size_t i=0;i<parts.size();++i)b+=(i?"slot"+std::to_string(i):"global")+" "+std::to_string(parts[i].size())+" "+std::to_string(Hash(parts[i]))+"\n"+parts[i];return b;
}
static std::vector<std::string> Parts(const Snapshot& s) {
    std::vector<std::string> p(4);std::string unused;SerializeConfigParts(s.global,p[0],unused);
    for(int i=0;i<3;++i)SerializeConfigParts(s.slots[i],unused,p[i+1]);return p;
}
int main() {
    Snapshot s;s.global.language="en";s.global.panelOpacity=91;s.global.enabled=false;s.global.questId=66845;
    LoadoutSet set;set.id=7;set.name="portable";set.equip="224";set.item="#80";set.food=3;set.quest=66845;
    s.slots[0].sets.push_back(set);s.slots[0].foods[3].name="meal";s.slots[0].foods[3].hp=5;
    s.slots[0].foods[3].skill[0]=56;s.slots[0].foods[4].deleted=true;s.slots[0].foodOrder={3};
    s.slots[0].nextSetId=8;s.slots[0].nextFoodId=5;s.slots[0].activeSet=7;s.slots[0].food=3;
    s.slots[2].foods[1].name="third";s.slots[2].nextFoodId=2;
    std::string bytes,err;Snapshot decoded;
    Snapshot defaults;CHECK(Encode(defaults,bytes,err),"default empty configuration is exportable");
    Snapshot allMeals;
    for(int id=-1;id<=56;++id) { FoodPreset f;f.hp=5;f.stamina=2;f.attack=f.defense=f.elemRes=3;f.skill[0]=id;allMeals.slots[0].foods[id+2]=f; }
    allMeals.slots[0].nextFoodId=59;
    CHECK(Encode(allMeals,bytes,err),"every built-in meal skill and maximum UI meal values export");
    CHECK(Encode(s,bytes,err),"encode complete snapshot");
    CHECK(Decode(bytes,decoded,err)&&decoded.global.panelOpacity==91&&!decoded.global.enabled&&decoded.slots[0].sets[0].id==7&&decoded.slots[0].foods[4].deleted,"roundtrip settings IDs and tombstones");
    CHECK(decoded.slots[1].sets.empty()&&decoded.slots[1].foods.empty(),"empty slot remains empty");
    CHECK(bytes.find("Steam")==std::string::npos&&bytes.find(":\\")==std::string::npos,"portable bytes have no identity or absolute path metadata");
    std::string damaged=bytes;damaged.back()^=1;decoded.global.panelOpacity=55;
    CHECK(!Decode(damaged,decoded,err)&&decoded.global.panelOpacity==55,"corruption rejected without publishing output");
    CHECK(!Decode(bytes.substr(0,bytes.size()-1),decoded,err),"truncation rejected");
    CHECK(!Decode(bytes+"extra",decoded,err),"trailing data rejected");
    CHECK(!Decode("[General]\nEnabled=0\n",decoded,err),"ordinary INI cannot be imported");
    damaged=bytes;damaged.replace(13,1,"9");CHECK(!Decode(damaged,decoded,err),"unknown version rejected");
    damaged=bytes;auto pos=damaged.find("slot2 ");damaged.replace(pos,5,"slot1");CHECK(!Decode(damaged,decoded,err),"duplicate/missing slot rejected");
    CHECK(!Decode(std::string(MaxBackupBytes+1,'x'),decoded,err),"oversized container rejected");
    auto p=Parts(s);p[0]+="[Unknown]\nFoo=1\n";CHECK(!Decode(Pack(p),decoded,err),"unknown section rejected with valid integrity hash");
    p=Parts(s);p[0]+="[General]\nEnabled=1\n";CHECK(!Decode(Pack(p),decoded,err),"duplicate section rejected");
    p=Parts(s);p[1]+="[Set.7]\nName=duplicate\n";CHECK(!Decode(Pack(p),decoded,err),"duplicate loadout ID rejected");
    p=Parts(s);p[1]+="[Food.3]\nName=duplicate\n";CHECK(!Decode(Pack(p),decoded,err),"duplicate meal ID rejected");
    p=Parts(s);p[1]="[General]\nActiveSet=-1\n";CHECK(!Decode(Pack(p),decoded,err),"missing required slot keys rejected");
    p=Parts(s);p[0]+="[Hotkeys]\nAccount=bad\n";CHECK(!Decode(Pack(p),decoded,err),"unknown account key rejected");
    Snapshot invalid=s;invalid.slots[0].sets[0].food=999;CHECK(!Decode(Pack(Parts(invalid)),decoded,err),"import rejects dangling meal reference");
    invalid=s;invalid.slots[0].sets[0].name="unsafe\n[General]";CHECK(!Encode(invalid,damaged,err),"INI line injection rejected");
    invalid=s;invalid.slots[0].foods[3].hp=6;CHECK(!Decode(Pack(Parts(invalid)),decoded,err),"import rejects invalid meal range");

    wchar_t tmp[MAX_PATH];GetTempPathW(MAX_PATH,tmp);
    const std::wstring dir=std::wstring(tmp)+L"biq-transfer-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    CreateDirectoryW(dir.c_str(),nullptr);
    Storage storage;storage.globalPath=dir+L"\\global.ini";for(int i=0;i<3;++i)storage.slotPaths[i]=dir+L"\\slot"+std::to_wstring(i+1)+L".ini";
    Config live=s.slots[0];live.language="ja";live.panelOpacity=77;live.questId=123;
    Write(storage.globalPath,"EXACT ORIGINAL GLOBAL\r\n");
    std::string base,prof;SerializeConfigParts(s.slots[2],base,prof);Write(storage.slotPaths[2],prof);
    Write(storage.slotPaths[0],"EXACT ORIGINAL CURRENT SLOT\r\n");
    Snapshot captured;CHECK(Capture(storage,live,captured,err)&&captured.global.panelOpacity==77&&captured.slots[0].sets[0].name=="portable"&&captured.slots[1].foods.empty()&&captured.slots[2].foods[1].name=="third","capture takes live current plus other on-disk slots and missing empty slot");
    CHECK(GetFileAttributesW(storage.slotPaths[1].c_str())==INVALID_FILE_ATTRIBUTES,"capture does not create absent slot");
    const std::string legacyEmpty = "; legacy empty slot\r\n[General]\r\nActiveSet=0 ; none\r\nFood=0 ; leave unchanged\r\nFoodOrder=\r\n";
    Write(storage.slotPaths[1],legacyEmpty);
    CHECK(Capture(storage,live,captured,err)&&captured.slots[1].activeSet==-1&&captured.slots[1].food==-1&&captured.slots[1].sets.empty(),"legacy zero sentinel in inactive slot is accepted and normalized");
    CHECK(Read(storage.slotPaths[1])==legacyEmpty,"capturing legacy inactive slot never rewrites it");
    std::string legacyBytes; Snapshot legacyDecoded;
    CHECK(Encode(captured,legacyBytes,err)&&Decode(legacyBytes,legacyDecoded,err)&&legacyDecoded.slots[1].activeSet==-1,"legacy zero sentinel exports as canonical portable backup");
    DeleteFileW(storage.slotPaths[1].c_str());
    Snapshot stale=s; stale.slots[0].nextSetId=1; stale.slots[0].nextFoodId=1;
    std::string canonical; Snapshot counters;
    CHECK(Encode(stale,canonical,err)&&Decode(canonical,counters,err)&&counters.slots[0].nextSetId==8&&counters.slots[0].nextFoodId==5,"export normalizes stale live counters including meal tombstones");
    CHECK(stale.slots[0].nextSetId==1&&stale.slots[0].nextFoodId==1,"export counter normalization never changes caller state");
    std::string staleGlobal,staleSlot; SerializeConfigParts(stale.slots[0],staleGlobal,staleSlot);
    Write(storage.slotPaths[1],staleSlot);
    CHECK(Capture(storage,live,captured,err)&&captured.slots[1].nextSetId==8&&captured.slots[1].nextFoodId==5,"capture accepts old inactive-slot counters and safely normalizes");
    CHECK(Read(storage.slotPaths[1])==staleSlot,"legacy counter capture leaves original file byte-for-byte unchanged");
    CHECK(!Decode(Pack(Parts(stale)),counters,err),"portable import still rejects unsafe allocation counters");
    DeleteFileW(storage.slotPaths[1].c_str());
    const std::wstring source=dir+L"\\import.biq",backupDir=dir+L"\\Backups",out=dir+L"\\export.biq";
    Write(source,bytes);
    CHECK(ExportFile(storage,live,out,err),"export file succeeds");
    const std::string originalExport=Read(out);CHECK(!ExportFile(storage,live,out,err)&&Read(out)==originalExport,"export refuses overwrite");
    const std::string originalGlobal=Read(storage.globalPath),originalCurrent=Read(storage.slotPaths[0]),originalThird=Read(storage.slotPaths[2]);
    std::wstring backup;
    for(int n=1;n<=4;++n) {
        testing::FailCommitAt(n);
        CHECK(!ImportFile(storage,source,backupDir,live,backup,err)&&!backup.empty(),"injected commit failure retains recovery backup");
        CHECK(Read(storage.globalPath)==originalGlobal&&Read(storage.slotPaths[0])==originalCurrent&&Read(storage.slotPaths[2])==originalThird&&Read(storage.slotPaths[1])=="<missing>"&&live.panelOpacity==77&&live.questId==123,"rollback restores all exact bytes/existence and leaves live unchanged");
        Snapshot recovery;CHECK(Decode(Read(backup),recovery,err)&&recovery.global.panelOpacity==77&&recovery.slots[0].activeSet==7,"recovery backup captures unsaved live state and original selection");
    }
    testing::FailCommitAt(0);
    const std::wstring blocked=dir+L"\\blocked";Write(blocked,"file not directory");
    CHECK(!ImportFile(storage,source,blocked,live,backup,err)&&Read(storage.globalPath)==originalGlobal&&live.panelOpacity==77,"backup failure aborts before target writes");
    CHECK(ImportFile(storage,source,backupDir,live,backup,err)&&live.panelOpacity==91&&live.language=="en"&&!live.enabled&&live.activeSet==-1&&live.food==-1&&live.questId==0,"import publishes merged config only after successful transaction");
    Config onDisk;CHECK(ParseConfigText(Read(storage.slotPaths[0]),onDisk,err)&&onDisk.activeSet==-1&&onDisk.food==-1,"disk and memory selection clearing match");
    Snapshot after;CHECK(Capture(storage,live,after,err)&&after.slots[2].foods[1].name=="third"&&after.slots[1].foods.empty(),"all three slots imported without creating example loadouts");
    const std::wstring profilesDir=dir+L"\\fresh-profiles";
    Storage fresh=storage;for(int i=0;i<3;++i)fresh.slotPaths[i]=profilesDir+L"\\local_"+std::to_wstring(i+1)+L".ini";
    CHECK(ImportFile(fresh,source,backupDir,live,backup,err)&&Read(fresh.slotPaths[1])!="<missing>","first import creates only trusted missing local profiles parent");
    for(const auto& path:fresh.slotPaths)DeleteFileW(path.c_str());RemoveDirectoryW(profilesDir.c_str());
    testing::FailCommitAt(3);testing::FailRollbackAt(1);
    CHECK(!ImportFile(storage,source,backupDir,live,backup,err)&&err.find("ROLLBACK FAILED")!=std::string::npos&&!backup.empty(),"rollback failure explicit with recovery retained");
    testing::FailCommitAt(0);testing::FailRollbackAt(0);
    // Fixture directory intentionally contains only our unique files. Remove via Win32 enumeration.
    WIN32_FIND_DATAW fd{};HANDLE h=FindFirstFileW((backupDir+L"\\*").c_str(),&fd);
    if(h!=INVALID_HANDLE_VALUE){do{if(!(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))DeleteFileW((backupDir+L"\\"+fd.cFileName).c_str());}while(FindNextFileW(h,&fd));FindClose(h);}RemoveDirectoryW(backupDir.c_str());
    h=FindFirstFileW((dir+L"\\*").c_str(),&fd);
    if(h!=INVALID_HANDLE_VALUE){do{if(!(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))DeleteFileW((dir+L"\\"+fd.cFileName).c_str());}while(FindNextFileW(h,&fd));FindClose(h);}RemoveDirectoryW(dir.c_str());
    std::printf("settings_transfer: %d failures\n",failures);return failures?1:0;
}
