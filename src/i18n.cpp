#include "i18n.h"

#include <windows.h>

#include <atomic>
#include <cctype>

namespace i18n {
namespace {

std::atomic<int> g_lang{-1};

Lang GuessSystem()
{
    // 只看界面语言的主语言部分：zh-CN / zh-TW 都当中文（表里只有简体），ja 当日文，其余当英文。
    switch (PRIMARYLANGID(::GetUserDefaultUILanguage())) {
    case LANG_CHINESE: return kZh;
    case LANG_JAPANESE: return kJa;
    default: return kEn;
    }
}

std::string Subst(const char* f, const std::string* a, int n)
{
    std::string out;
    int used = 0;
    for (const char* p = f; *p; ++p) {
        if (*p == '%' && p[1] == '%') { out += '%'; ++p; }
        else if (*p == '%' && (p[1] == 's' || p[1] == 'd') && used < n) { out += a[used++]; ++p; }
        else out += *p;
    }
    return out;
}

} // namespace

Lang SystemLang() { return GuessSystem(); }

Lang Current()
{
    int v = g_lang.load(std::memory_order_acquire);
    if (v < 0) {
        int guessed = static_cast<int>(GuessSystem());
        if (g_lang.compare_exchange_strong(v, guessed, std::memory_order_acq_rel, std::memory_order_acquire)) {
            v = guessed;
        }
    }
    return (v >= 0 && v < kCount) ? static_cast<Lang>(v) : kZh;
}

void Set(Lang l)
{
    const int val = (l >= 0 && l < kCount) ? static_cast<int>(l) : static_cast<int>(kZh);
    g_lang.store(val, std::memory_order_release);
}

Lang FromString(const std::string& s)
{
    std::string v;
    for (char ch : s) if (!std::isspace((unsigned char)ch)) v += (char)std::tolower((unsigned char)ch);
    if (v.empty() || v == "auto" || v == "system") return GuessSystem();
    if (v == "zh" || v == "chs" || v == "cn" || v == "zh-cn" || v == "chinese") return kZh;
    if (v == "en" || v == "eng" || v == "english") return kEn;
    if (v == "ja" || v == "jp" || v == "jpn" || v == "japanese") return kJa;
    return GuessSystem();
}

const char* ToString(Lang l)
{
    switch (l) {
    case kEn: return "en";
    case kJa: return "ja";
    default: return "zh";
    }
}

const char* Tr(const char* zh, const char* en, const char* ja)
{
    switch (Current()) {
    case kEn: return en && *en ? en : zh;
    case kJa: return ja && *ja ? ja : zh;
    default: return zh;
    }
}

std::string Fmt(const char* f) { return f; }
std::string Fmt(const char* f, const char* a) { const std::string v[] = {a ? a : ""}; return Subst(f, v, 1); }
std::string Fmt(const char* f, const char* a, const char* b) { const std::string v[] = {a ? a : "", b ? b : ""}; return Subst(f, v, 2); }
std::string Fmt(const char* f, int a) { const std::string v[] = {std::to_string(a)}; return Subst(f, v, 1); }
std::string Fmt(const char* f, int a, const char* b) { const std::string v[] = {std::to_string(a), b ? b : ""}; return Subst(f, v, 2); }
std::string Fmt(const char* f, int a, int b) { const std::string v[] = {std::to_string(a), std::to_string(b)}; return Subst(f, v, 2); }
std::string Fmt(const char* f, int a, int b, int c) { const std::string v[] = {std::to_string(a), std::to_string(b), std::to_string(c)}; return Subst(f, v, 3); }

} // namespace i18n
