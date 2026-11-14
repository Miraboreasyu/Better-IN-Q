#pragma once
// Window-thread-only acquisition; rendering only copies a bounded snapshot.
#include <windows.h>
#include <imm.h>
#include <msctf.h>
#include <oleauto.h>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#pragma comment(lib, "oleaut32.lib")
namespace ime_candidates {
struct View { std::wstring composition; std::vector<std::wstring> candidates; int selected=-1; };
inline std::mutex mutex;
inline View view;
inline bool active=false;
inline DWORD candidateId=TF_INVALID_COOKIE, cookie=TF_INVALID_COOKIE;
inline ITfUIElementMgr* manager=nullptr;
inline ITfSource* source=nullptr;
inline void Clear() { std::lock_guard<std::mutex> lock(mutex); view={}; }
inline View Copy() { std::lock_guard<std::mutex> lock(mutex); return view; }
inline bool CandidateFailure(DWORD id) {
    if(candidateId==id)candidateId=TF_INVALID_COOKIE;
    std::lock_guard<std::mutex> lock(mutex);view.candidates.clear();view.selected=-1;return false;
}
inline bool ReadList(ITfCandidateListUIElement* list, DWORD id) {
    if(!active || !list) return CandidateFailure(id);
    UINT count=0,selection=0,page=0,pages=0;
    bool ok=SUCCEEDED(list->GetCount(&count)) && count<=65536 && SUCCEEDED(list->GetSelection(&selection)) && SUCCEEDED(list->GetCurrentPage(&page));
    UINT first=0,last=count;
    if(ok && SUCCEEDED(list->GetPageIndex(nullptr,0,&pages)) && pages && pages<=4096) {
        std::vector<UINT> indices(pages); UINT got=0;
        ok=SUCCEEDED(list->GetPageIndex(indices.data(),pages,&got)) && got<=pages && page<got;
        if(ok) { first=indices[page]; last=page+1<got?indices[page+1]:count; ok=first<=last && last<=count; }
    } else { ok=false; } // No guessed page size or guessed current page.
    View next;
    if(ok) {
        if(last-first>256) return CandidateFailure(id); // reject, never silently truncate
        for(UINT i=first;i<last;++i) {
            BSTR word=nullptr;
            if(FAILED(list->GetString(i,&word))) { ok=false; break; }
            if(!word || SysStringLen(word)>4096) {if(word)SysFreeString(word);ok=false;break;}
            next.candidates.emplace_back(word,SysStringLen(word));
            if(word) SysFreeString(word);
        }
        next.selected=selection>=first && selection<last?int(selection-first):-1;
    }
    if(!ok || next.candidates.empty()) return CandidateFailure(id);
    { std::lock_guard<std::mutex> lock(mutex); view.candidates=std::move(next.candidates); view.selected=next.selected; }
    candidateId=id; return true;
}
inline bool ReadElement(DWORD id) {
    if(!active || !manager) return false;
    ITfUIElement* element=nullptr;
    if(FAILED(manager->GetUIElement(id,&element)) || !element) return false;
    ITfCandidateListUIElement* list=nullptr;
    HRESULT hr=element->QueryInterface(__uuidof(ITfCandidateListUIElement),reinterpret_cast<void**>(&list));
    element->Release();
    if(FAILED(hr)||!list) return false;
    const bool ok=ReadList(list,id);list->Release();return ok;
}
struct Sink final:ITfUIElementSink {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER; *out=nullptr;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(ITfUIElementSink)) {*out=static_cast<ITfUIElementSink*>(this);AddRef();return S_OK;}return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return 2;}
    ULONG STDMETHODCALLTYPE Release() override {return 1;}
    HRESULT STDMETHODCALLTYPE BeginUIElement(DWORD id,BOOL* show) override {
        // Never hide unrelated UI or a candidate list we could not read.
        if(ReadElement(id) && show) *show=FALSE; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UpdateUIElement(DWORD id) override {
        if(id!=candidateId) return S_OK; // unrelated TSF elements must not clear our list
        if(!ReadElement(id) && manager) {
            CandidateFailure(id);
            ITfUIElement* element=nullptr;
            if(SUCCEEDED(manager->GetUIElement(id,&element)) && element){element->Show(TRUE);element->Release();}
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndUIElement(DWORD id) override {
        if(id==candidateId) {candidateId=TF_INVALID_COOKIE;std::lock_guard<std::mutex> lock(mutex);view.candidates.clear();view.selected=-1;}return S_OK;
    }
};
inline Sink sink;
inline void Stop() {
    active=false;
    if(manager && candidateId!=TF_INVALID_COOKIE) {
        ITfUIElement* element=nullptr;
        if(SUCCEEDED(manager->GetUIElement(candidateId,&element)) && element){element->Show(TRUE);element->Release();}
    }
    if(source) {if(cookie!=TF_INVALID_COOKIE)source->UnadviseSink(cookie);source->Release();source=nullptr;}
    if(manager){manager->Release();manager=nullptr;}
    cookie=candidateId=TF_INVALID_COOKIE;Clear();
}
inline void Start() {
    active=true;if(source)return;
    using GetManager=HRESULT(WINAPI*)(ITfThreadMgr**);
    static HMODULE module=LoadLibraryW(L"msctf.dll");
    auto get=module?reinterpret_cast<GetManager>(GetProcAddress(module,"TF_GetThreadMgr")):nullptr;
    ITfThreadMgr* thread=nullptr;
    if(!get || FAILED(get(&thread))||!thread)return;
    bool ok=SUCCEEDED(thread->QueryInterface(__uuidof(ITfUIElementMgr),reinterpret_cast<void**>(&manager))) && manager &&
        SUCCEEDED(manager->QueryInterface(__uuidof(ITfSource),reinterpret_cast<void**>(&source))) && source &&
        SUCCEEDED(source->AdviseSink(__uuidof(ITfUIElementSink),&sink,&cookie));
    thread->Release();if(!ok){Stop();active=true;}
}
inline void Message(HWND window,UINT msg,WPARAM w,LPARAM l) {
    if(!active)return;
    if(msg==WM_IME_ENDCOMPOSITION){candidateId=TF_INVALID_COOKIE;Clear();return;}
    if(msg!=WM_IME_COMPOSITION && msg!=WM_IME_NOTIFY)return;
    if(msg==WM_IME_NOTIFY && w==IMN_CLOSECANDIDATE && candidateId==TF_INVALID_COOKIE) {
        std::lock_guard<std::mutex> lock(mutex);view.candidates.clear();view.selected=-1;return;
    }
    HIMC context=ImmGetContext(window);if(!context)return;
    if(msg==WM_IME_COMPOSITION) {
        std::wstring text;
        LONG bytes=ImmGetCompositionStringW(context,GCS_COMPSTR,nullptr,0);
        if(bytes>0 && bytes<=2048 && bytes%2==0) {
            text.resize(bytes/2);LONG got=ImmGetCompositionStringW(context,GCS_COMPSTR,text.data(),bytes);
            if(got<0 || got>bytes || got%2)text.clear();else text.resize(got/2);
        }
        std::lock_guard<std::mutex> lock(mutex);view.composition=std::move(text);
    }
    if(msg==WM_IME_NOTIFY && (w==IMN_OPENCANDIDATE||w==IMN_CHANGECANDIDATE) && candidateId==TF_INVALID_COOKIE) {
        // Notification lParam is a bitmask of changed candidate-list indices.
        DWORD index=0;while(index<32 && !(DWORD(l)&(1u<<index)))++index;
        if(index==32)index=0;
        DWORD size=ImmGetCandidateListW(context,index,nullptr,0);
        View next;
        if(size>=sizeof(CANDIDATELIST) && size<=65536) {
            std::vector<DWORD> buffer((size+3)/4);
            auto c=reinterpret_cast<CANDIDATELIST*>(buffer.data());
            DWORD got=ImmGetCandidateListW(context,index,c,size);
            if(got && got<=size && c->dwCount<=(got-std::min<DWORD>(got,offsetof(CANDIDATELIST,dwOffset)))/4) {
                DWORD first=c->dwPageStart,last=first;
                if(first<=c->dwCount && c->dwPageSize>0 && c->dwPageSize<=256) last=first+(std::min)(c->dwPageSize,c->dwCount-first);
                last=(std::min)(last,c->dwCount);
                if(first<=last)for(DWORD i=first;i<last;++i) {
                    DWORD off=c->dwOffset[i];if(off>=got || off%2){next.candidates.clear();break;}
                    const wchar_t* p=reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(c)+off);
                    size_t cap=(got-off)/2,n=0;while(n<cap && p[n])++n;
                    if(n==cap || n>4096){next.candidates.clear();break;}
                    next.candidates.emplace_back(p,n);
                }
                next.selected=c->dwSelection>=first && c->dwSelection<last?int(c->dwSelection-first):-1;
            }
        }
        std::lock_guard<std::mutex> lock(mutex);view.candidates=std::move(next.candidates);view.selected=next.selected;
    }
    ImmReleaseContext(window,context);
}
} // namespace ime_candidates
