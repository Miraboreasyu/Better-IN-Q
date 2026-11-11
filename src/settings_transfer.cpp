#include "settings_transfer.h"
#include "profile.h"
#include <windows.h>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace settings_transfer {
namespace {
thread_local int failCommit = 0, failRollback = 0;
bool Fail(std::string& err, const char* s) { err = s; return false; }
std::string Trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
}
std::string Value(std::string s) {
    for (size_t i = 0; i < s.size(); ++i)
        if ((s[i] == ';' && (!i || s[i-1] == ' ' || s[i-1] == '\t')) ||
            (s[i] == '#' && i && (s[i-1] == ' ' || s[i-1] == '\t'))) { s.resize(i); break; }
    return Trim(s);
}
bool Number(const std::string& s, int lo, int hi, int* output = nullptr) {
    if (s.empty()) return false;
    size_t i = s[0] == '-' ? 1 : 0;
    if (i == s.size()) return false;
    long long n = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        n = n * 10 + s[i] - '0';
        if (n > INT_MAX) return false;
    }
    if (s[0] == '-') n = -n;
    if (n < lo || n > hi) return false;
    if (output) *output = (int)n;
    return true;
}
bool In(const std::string& s, const char* names) {
    return (std::string("|") + names + "|").find("|" + s + "|") != std::string::npos;
}
bool List(const std::string& s, int lo, int hi, int count = -1) {
    if (s.empty()) return count < 0;
    size_t p = 0; int n = 0;
    std::set<int> seen;
    do {
        size_t e = s.find(',', p); if (e == std::string::npos) e = s.size();
        int v = 0; if (!Number(Trim(s.substr(p,e-p)), lo, hi, &v)) return false;
        if (count < 0 && !seen.insert(v).second) return false;
        ++n; p = e + 1;
    } while (p <= s.size());
    return count < 0 || n == count;
}
// mode: 0 global only, 1 slot only, 2 local legacy combined INI.
bool Validate(const std::string& text, int mode, bool complete, Config& output, std::string& err) {
    if (text.empty() || text.size() > MaxPartBytes || text.find('\0') != std::string::npos)
        return Fail(err, "Empty, oversized or binary INI section");
    if (!::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0))
        return Fail(err, "Invalid UTF-8 INI section");
    std::string input = text;
    if (input.compare(0,3,"\xEF\xBB\xBF") == 0) input.erase(0,3);
    std::istringstream lines(input); std::string line, section;
    std::map<std::string,std::set<std::string>> keys;
    while (std::getline(lines,line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[' && line.back() == ']') {
            section = line.substr(1,line.size()-2);
            int id = 0;
            const bool set = section.compare(0,4,"Set.") == 0;
            const bool food = section.compare(0,5,"Food.") == 0;
            if ((set || food) && (!Number(section.substr(set?4:5),1,1000000,&id) ||
                section.substr(set?4:5) != std::to_string(id))) return Fail(err,"Invalid section number");
            if (!set && !food && !In(section,"General|Debug|Hotkeys")) return Fail(err,"Unknown INI section");
            if ((mode == 0 && (set || food)) || (mode == 1 && section != "General" && !set && !food))
                return Fail(err,"INI section in wrong backup segment");
            if (!keys.emplace(section,std::set<std::string>{}).second) return Fail(err,"Duplicate INI section/number");
            continue;
        }
        const size_t eq = line.find('=');
        if (section.empty() || eq == std::string::npos) return Fail(err,"Malformed INI line");
        std::string k = Trim(line.substr(0,eq)), v = Value(line.substr(eq+1));
        if (!keys[section].insert(k).second) return Fail(err,"Duplicate INI key");
        bool ok = false;
        if (section == "General") {
            const bool slot = In(k,"ActiveSet|Food|FoodOrder|NextSetId|NextFoodId");
            if ((mode == 0 && slot) || (mode == 1 && !slot)) return Fail(err,"INI key in wrong backup segment");
            if (In(k,"Enabled|Reset|AcceptOnly|WyvernStart|QuickBlackout|PalicoEnabled|RefillHotkey")) ok = In(v,"0|1|true|false|on|off|yes|no");
            else if (In(k,"ActiveSet|Food")) ok = Number(v,-1,1000000) && (mode == 2 || v != "0");
            else if (In(k,"NextSetId|NextFoodId")) ok = Number(v,1,1000001);
            else if (k == "FoodOrder") ok = List(v,1,1000000);
            else if (k == "QuestID") ok = Number(v,0,INT_MAX);
            else if (k == "PanelOpacity") ok = Number(v,30,100);
            else if (k == "SdrNits") ok = Number(v,80,10000);
            else if (k == "Debug") ok = Number(v,0,INT_MAX);
            else if (k == "Language") ok = In(v,"auto|zh|en|ja");
            else if (k == "Hdr") ok = In(v,"auto|off|scrgb|hdr10");
        } else if (section == "Debug") {
            if (k == "Disable") ok = v.size() <= 4096;
            else if (In(k,"ForceQuit|MealProbe|NoWndHook|PalicoProbe")) ok = In(v,"0|1|true|false|on|off|yes|no");
        } else if (section == "Hotkeys") {
            ok = In(k,"Restart|Panel|ResetOnly|Refill") && v.size() <= 4096 && (k != "Panel" || !v.empty());
        } else if (section.compare(0,4,"Set.") == 0) {
            if (k == "Name") ok = v.size() <= 4096;
            else if (k == "Equip") ok = v.empty() || Number(v,1,224);
            else if (k == "Item") ok = v.empty() || (v[0] == '#' && Number(v.substr(1),1,80));
            else if (k == "LinkRadial") ok = In(v,"0|1|true|false|on|off|yes|no");
            else if (k == "Food") ok = Number(v,-1,1000000) && (mode == 2 || v != "0");
            else if (k == "Quest") ok = Number(v,0,INT_MAX);
            else if (k == "PalicoDeploy") ok = Number(v,-1,1);
            else if (k == "PalicoLevel") ok = Number(v,0,50);
            else if (k == "PalicoGadget") ok = Number(v,-1,5);
            else if (k == "PalicoLoadout") ok = Number(v,0,24);
            else if (k == "PalicoGadgetLevel") ok = Number(v,0,15);
        } else {
            if (k == "Name") ok = v.size() <= 4096;
            else if (k == "HP") ok = Number(v,0,5);
            else if (k == "Stamina") ok = Number(v,0,2);
            else if (In(k,"Attack|Defense|ElemRes")) ok = Number(v,0,3);
            else if (k == "Skills") ok = List(v,-1,56,3);
            else if (k == "Deleted") ok = In(v,"0|1|true|false|on|off|yes|no");
        }
        if (!ok) { err = "Unknown INI key or invalid value: [" + section + "] " + k; return false; }
    }
    if (!keys.count("General")) return Fail(err,"Missing General section");
    if (complete) {
        auto required = [&](const std::string& sec, const char* names) {
            std::istringstream parts(names); std::string name;
            while (std::getline(parts,name,'|')) if (!keys[sec].count(name)) return false;
            return true;
        };
        if (mode == 0 && (!required("General","Enabled|QuestID|Reset|AcceptOnly|WyvernStart|QuickBlackout|PanelOpacity|Hdr|SdrNits|Language") ||
            !required("Debug","Disable|ForceQuit|MealProbe|NoWndHook") || !required("Hotkeys","Restart|Panel|ResetOnly")))
            return Fail(err,"Incomplete global segment");
        if (mode == 1 && !required("General","ActiveSet|Food|FoodOrder|NextSetId|NextFoodId")) return Fail(err,"Incomplete slot segment");
        for (const auto& pair : keys) {
            if (pair.first.compare(0,4,"Set.") == 0 && !required(pair.first,"Name|Equip|Item|LinkRadial|Food|Quest")) return Fail(err,"Incomplete loadout");
            if (pair.first.compare(0,5,"Food.") == 0 && !required(pair.first,"Name|HP|Stamina|Attack|Defense|ElemRes|Skills")) return Fail(err,"Incomplete meal");
        }
    }
    Config c; if (!ParseConfigText(text,c,err)) return false;
    // Verify references before the tolerant legacy parser can silently normalize them.
    std::istringstream refs(input); section.clear();
    while (std::getline(refs,line)) {
        line = Trim(line); if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') { section = line.substr(1,line.size()-2); continue; }
        size_t eq=line.find('='); const std::string k=Trim(line.substr(0,eq)), v=Value(line.substr(eq+1));
        int n=0;
        if (mode != 2 && section == "General" && k == "NextSetId" && Number(v,1,1000001,&n) && n < c.NextSetId())
            return Fail(err,"Loadout allocation counter would reuse an existing ID");
        if (mode != 2 && section == "General" && k == "NextFoodId" && Number(v,1,1000001,&n) && !c.foods.empty() && n <= c.foods.rbegin()->first)
            return Fail(err,"Meal allocation counter would reuse an existing ID");
        if ((k == "ActiveSet" || k == "Food") && Number(v,1,1000000,&n)) {
            if (k == "ActiveSet" ? !c.FindSetById(n) : !c.foods.count(n)) return Fail(err,"Dangling loadout/meal reference");
        }
        if (section == "General" && k == "FoodOrder" && !v.empty()) {
            std::istringstream ids(v); std::string id;
            while (std::getline(ids,id,',')) if (!c.foods.count(std::stoi(Trim(id)))) return Fail(err,"Dangling meal order reference");
        }
    }
    output = std::move(c); return true;
}
std::string Compact(const std::string& text) {
    std::istringstream lines(text); std::string line,out;
    while (std::getline(lines,line)) {
        if (line.compare(0,3,"\xEF\xBB\xBF") == 0) line.erase(0,3);
        line=Trim(line); if (line.empty() || line[0]==';' || line[0]=='#') continue;
        const size_t eq=line.find('=');
        out += eq == std::string::npos ? line : Trim(line.substr(0,eq))+"="+Value(line.substr(eq+1));
        out+='\n';
    }
    return out;
}
unsigned long Hash(const std::string& s) { unsigned long h=2166136261UL; for(unsigned char c:s) h=(h^c)*16777619UL; return h; }
bool Read(const std::wstring& path,std::string& out,bool& exists,std::string& err,size_t max=MaxBackupBytes) {
    HANDLE h=::CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    exists=h!=INVALID_HANDLE_VALUE;
    if (!exists) { const DWORD e=::GetLastError(); if(e==ERROR_FILE_NOT_FOUND || e==ERROR_PATH_NOT_FOUND) {out.clear();return true;} return Fail(err,"Cannot open configuration/backup"); }
    LARGE_INTEGER size{}; bool ok=::GetFileSizeEx(h,&size) && size.QuadPart>=0 && (unsigned long long)size.QuadPart<=max;
    DWORD got=0; if(ok) {out.resize((size_t)size.QuadPart);ok=out.empty() || (::ReadFile(h,&out[0],(DWORD)out.size(),&got,nullptr) && got==out.size());}
    ::CloseHandle(h); return ok || Fail(err,"Cannot read file or size limit exceeded");
}
bool WriteNew(const std::wstring& path,const std::string& text) {
    HANDLE h=::CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE) return false;
    DWORD n=0; const bool ok=::WriteFile(h,text.data(),(DWORD)text.size(),&n,nullptr) && n==text.size() && ::FlushFileBuffers(h);
    ::CloseHandle(h); if(!ok) ::DeleteFileW(path.c_str()); return ok;
}
bool ValidStorage(const Storage& s,std::string& err) {
    if(s.currentSlot<1 || s.currentSlot>3 || s.globalPath.empty()) return Fail(err,"Invalid storage binding");
    std::set<std::wstring> paths;
    auto add=[&](std::wstring p) {std::transform(p.begin(),p.end(),p.begin(),[](wchar_t c){return (wchar_t)::towlower(c);});return !p.empty() && paths.insert(p).second;};
    if(!add(s.globalPath)) return false;
    for(const auto& p:s.slotPaths) if(!add(p)) return Fail(err,"Duplicate/empty storage path");
    return true;
}
bool EnsureParent(const std::wstring& path) {
    const size_t slash=path.find_last_of(L"\\/");
    if(slash==std::wstring::npos)return true;
    const std::wstring parent=path.substr(0,slash);
    const DWORD attr=::GetFileAttributesW(parent.c_str());
    if(attr!=INVALID_FILE_ATTRIBUTES)return !!(attr&FILE_ATTRIBUTE_DIRECTORY);
    // Caller supplies trusted local targets. Create just their immediate parent,
    // never derive directories from portable backup content.
    return !!::CreateDirectoryW(parent.c_str(),nullptr) || (::GetLastError()==ERROR_ALREADY_EXISTS &&
        (::GetFileAttributesW(parent.c_str())&FILE_ATTRIBUTE_DIRECTORY));
}
std::wstring Unique(const std::wstring& stem) {
    static volatile LONG serial=0;
    return stem+L"."+std::to_wstring(::GetCurrentProcessId())+L"."+std::to_wstring(::InterlockedIncrement(&serial));
}
}
namespace testing { void FailCommitAt(int n){failCommit=n;} void FailRollbackAt(int n){failRollback=n;} }
Storage CurrentStorage() {
    Storage s; s.globalPath=profile::BasePath();s.currentSlot=profile::Slot();
    for(int i=0;i<3;++i)s.slotPaths[i]=profile::PathForSlot(i+1);return s;
}
bool Encode(const Snapshot& s,std::string& bytes,std::string& err) {
    auto safe=[](const std::string& v) {
        return v.find_first_of("\r\n") == std::string::npos && v.find('\0') == std::string::npos &&
               Trim(v) == v && Value(v) == v;
    };
    for(const Config* c : {&s.global,&s.slots[0],&s.slots[1],&s.slots[2]}) {
        if(!safe(c->hkRestart) || !safe(c->hkPanel) || !safe(c->hkResetOnly) || !safe(c->hkRefill) || !safe(c->debugDisable))
            return Fail(err,"Configuration text cannot be represented safely in INI");
        for(const auto& set:c->sets) if(!safe(set.name) || !safe(set.equip) || !safe(set.item))
            return Fail(err,"Loadout text cannot be represented safely in INI");
        for(const auto& meal:c->foods) if(!safe(meal.second.name))
            return Fail(err,"Meal text cannot be represented safely in INI");
    }
    std::array<std::string,4> parts;std::string unused;
    SerializeConfigParts(s.global,parts[0],unused);
    for(int i=0;i<3;++i) {
        Config slot = s.slots[i]; // export canonical counters without mutating live/on-disk state
        for (const auto& set : slot.sets) {
            if (set.id < 1 || set.id > 1000000) return Fail(err,"Invalid loadout ID");
            slot.nextSetId = (std::max)(slot.nextSetId, set.id + 1);
        }
        for (const auto& food : slot.foods) {
            if (food.first < 1 || food.first > 1000000) return Fail(err,"Invalid meal ID");
            slot.nextFoodId = (std::max)(slot.nextFoodId, food.first + 1);
        }
        SerializeConfigParts(slot,unused,parts[i+1]);
    }
    std::string result="BIQ-SETTINGS\n1\n";
    for(int i=0;i<4;++i) {
        parts[i]=Compact(parts[i]);
        if (parts[i].empty() || parts[i].size() > MaxPartBytes)
            return Fail(err,"Export segment exceeds size limit");
        result+=(i?"slot"+std::to_string(i):"global")+" "+std::to_string(parts[i].size())+" "+std::to_string(Hash(parts[i]))+"\n"+parts[i];
    }
    bytes=std::move(result);err.clear();return true;
}
bool Decode(const std::string& bytes,Snapshot& s,std::string& err) {
    const std::string magic="BIQ-SETTINGS\n1\n";
    if(bytes.size()>MaxBackupBytes || bytes.compare(0,magic.size(),magic)!=0)return Fail(err,"Not a supported Better-IN-Q backup/version");
    size_t pos=magic.size();Snapshot parsed;
    for(int i=0;i<4;++i) {
        size_t end=bytes.find('\n',pos);if(end==std::string::npos)return Fail(err,"Missing backup segment");
        const std::string header=bytes.substr(pos,end-pos);std::istringstream in(header);
        std::string name,extra;unsigned long long len=0,hash=0;
        if(!(in>>name>>len>>hash) || (in>>extra) || name!=(i?"slot"+std::to_string(i):"global") || !len || len>MaxPartBytes || hash>0xffffffffULL)
            return Fail(err,"Invalid, missing, duplicate or oversized backup segment");
        pos=end+1;if(len>bytes.size()-pos)return Fail(err,"Truncated backup segment");
        const std::string text=bytes.substr(pos,(size_t)len);pos+=(size_t)len;
        if(Hash(text)!=(unsigned long)hash)return Fail(err,"Backup integrity check failed");
        if(!Validate(text,i?1:0,true,i?parsed.slots[i-1]:parsed.global,err))return false;
    }
    if(pos!=bytes.size())return Fail(err,"Unexpected trailing backup content");
    s=std::move(parsed);err.clear();return true;
}
bool Capture(const Storage& storage,const Config& live,Snapshot& s,std::string& err) {
    if(!ValidStorage(storage,err))return false;
    Snapshot captured;captured.global=live;
    for(int i=0;i<3;++i) {
        if(i+1==storage.currentSlot) {captured.slots[i]=live;continue;}
        std::string text;bool exists=false;
        if(!Read(storage.slotPaths[i],text,exists,err,MaxPartBytes))return false;
        // Local slots use exactly the normal configuration parser (legacy values,
        // counters and references are normalized there). Strict Validate belongs
        // to the portable backup boundary, not local INI ingestion.
        if(exists && !ParseConfigText(text,captured.slots[i],err))return false;
    }
    s=std::move(captured);return true;
}
bool ExportFile(const Storage& storage,const Config& live,const std::wstring& destination,std::string& err) {
    Snapshot s;std::string bytes;
    if(!Capture(storage,live,s,err) || !Encode(s,bytes,err))return false;
    if(!WriteNew(destination,bytes))return Fail(err,"Cannot create export (destination must not already exist)");
    return true;
}
bool ImportFile(const Storage& storage,const std::wstring& source,const std::wstring& backupDirectory,
                Config& live,std::wstring& backupPath,std::string& err) {
    backupPath.clear();if(!ValidStorage(storage,err))return false;
    std::string bytes;bool exists=false;Snapshot incoming,current;
    if(!Read(source,bytes,exists,err) || !exists)return Fail(err,"Cannot read import file");
    if(!Decode(bytes,incoming,err) || !Capture(storage,live,current,err) || !Encode(current,bytes,err))return false;
    // Recovery backup is independent of any target, never overwritten.
    if(backupDirectory.empty())return Fail(err,"Missing recovery backup directory");
    if(!::CreateDirectoryW(backupDirectory.c_str(),nullptr) && ::GetLastError()!=ERROR_ALREADY_EXISTS)return Fail(err,"Cannot create recovery directory");
    SYSTEMTIME t{};::GetLocalTime(&t);wchar_t stamp[96];
    std::swprintf(stamp,sizeof(stamp)/sizeof(stamp[0]),L"\\before-import-%04u%02u%02u-%02u%02u%02u-%03u",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
    std::wstring recovery=Unique(backupDirectory+stamp)+L".biq";
    if(!WriteNew(recovery,bytes))return Fail(err,"Recovery backup failed; import aborted");
    backupPath=recovery;
    incoming.global.questId=0;
    for(auto& c:incoming.slots)c.ResetSessionSelection();
    Config published=incoming.global;
    const Config& slot=incoming.slots[storage.currentSlot-1];
    published.sets=slot.sets;published.foods=slot.foods;published.foodOrder=slot.foodOrder;
    published.activeSet=slot.activeSet;published.food=slot.food;
    published.nextSetId=slot.nextSetId;published.nextFoodId=slot.nextFoodId;
    std::array<std::wstring,4> targets,stages,oldFiles;
    std::array<std::string,4> texts,original;
    std::array<bool,4> had{};
    targets[0]=storage.globalPath;std::string unused;
    SerializeConfigParts(incoming.global,texts[0],unused);
    for(int i=1;i<4;++i){targets[i]=storage.slotPaths[i-1];SerializeConfigParts(incoming.slots[i-1],unused,texts[i]);}
    auto cleanup=[&]() {for(auto& p:stages)if(!p.empty())::DeleteFileW(p.c_str());for(auto& p:oldFiles)if(!p.empty())::DeleteFileW(p.c_str());};
    for(int i=0;i<4;++i) {
        if(!Read(targets[i],original[i],had[i],err,MaxPartBytes)){cleanup();return false;}
        if(!EnsureParent(targets[i])) {cleanup();return Fail(err,"Cannot create local configuration directory");}
        const std::wstring stage=Unique(targets[i]+L".import-stage");
        if(!WriteNew(stage,texts[i])) {cleanup();return Fail(err,"Cannot stage import; original files unchanged");}
        stages[i]=stage;
        if(had[i]) {
            const std::wstring rollback=Unique(targets[i]+L".import-rollback");
            if(!WriteNew(rollback,original[i])) {cleanup();return Fail(err,"Cannot stage rollback; original files unchanged");}
            oldFiles[i]=rollback;
        }
    }
    for(int i=0;i<4;++i) {
        if(failCommit==i+1 || !::MoveFileExW(stages[i].c_str(),targets[i].c_str(),MOVEFILE_WRITE_THROUGH|(had[i]?MOVEFILE_REPLACE_EXISTING:0))) {
            bool restored=true;
            for(int j=i-1;j>=0;--j) {
                const bool ok=failRollback!=j+1 && (had[j] ? !!::MoveFileExW(oldFiles[j].c_str(),targets[j].c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH) : !!::DeleteFileW(targets[j].c_str()));
                if(ok)oldFiles[j].clear();else {restored=false;oldFiles[j].clear();} // retain failed rollback copy
            }
            cleanup();
            return Fail(err,restored?"Import replace failed; all original files restored (recovery backup retained)":"ROLLBACK FAILED: files may be incomplete; recovery backup and rollback copies retained");
        }
        stages[i].clear();
    }
    cleanup();live=std::move(published);err.clear();return true;
}
} // namespace settings_transfer
