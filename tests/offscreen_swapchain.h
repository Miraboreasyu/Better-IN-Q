#pragma once
// 离屏 WARP DXGI 假交换链：
//   * 在锁屏 / 无头 / CI 环境下，Windows 会拒绝 CreateSwapChain*（0x887A0022），
//     但离屏 WARP 设备与 RenderTarget 纹理 / Resource 完全可用。
//   * 实现完整的 IDXGISwapChain3 虚表并只返回抽象 COM 指针，阻止 /O2 去虚化，
//     让 overlay::InstallOnSwapChain 与 Present / ResizeBuffers 虚表钩子完整闭环。
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace offscreen_test {

inline bool IsHeadlessRequested(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--headless") == 0) return true;
    const char* env = std::getenv("BETTERINQ_HEADLESS");
    return env && env[0] == '1' && env[1] == '\0';
}

inline bool IsHeadlessRequested(int argc, wchar_t** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::wcscmp(argv[i], L"--headless") == 0) return true;
    const char* env = std::getenv("BETTERINQ_HEADLESS");
    return env && env[0] == '1' && env[1] == '\0';
}

namespace detail {

class SwapChainBase : public IDXGISwapChain3 {
public:
    SwapChainBase(HWND hwnd, UINT w, UINT h, DXGI_FORMAT fmt, UINT bufCount)
    {
        std::memset(&desc_, 0, sizeof(desc_));
        desc_.BufferDesc.Width = w;
        desc_.BufferDesc.Height = h;
        desc_.BufferDesc.Format = fmt;
        desc_.SampleDesc.Count = 1;
        desc_.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc_.BufferCount = bufCount;
        desc_.OutputWindow = hwnd;
        desc_.Windowed = TRUE;
        desc_.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    }

    virtual ~SwapChainBase() = default;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
    {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
            riid == __uuidof(IDXGIDeviceSubObject) || riid == __uuidof(IDXGISwapChain) ||
            riid == __uuidof(IDXGISwapChain1) || riid == __uuidof(IDXGISwapChain2) ||
            riid == __uuidof(IDXGISwapChain3)) {
            AddRef();
            *ppvObject = static_cast<IDXGISwapChain3*>(this);
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG n = --ref_;
        if (n == 0) delete this;
        return n;
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT* size, void*) override
    {
        if (size) *size = 0;
        return DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void** parent) override
    {
        if (parent) *parent = nullptr;
        return E_NOINTERFACE;
    }

    // IDXGISwapChain (subset shared)
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL, IDXGIOutput*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fs, IDXGIOutput** target) override
    {
        if (fs) *fs = FALSE;
        if (target) *target = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override
    {
        if (!d) return E_INVALIDARG;
        *d = desc_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** out) override
    {
        if (out) *out = nullptr;
        return DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s) override
    {
        if (s) std::memset(s, 0, sizeof(*s));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* n) override
    {
        if (n) *n = presentCount_;
        return S_OK;
    }

    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* d) override
    {
        if (!d) return E_INVALIDARG;
        std::memset(d, 0, sizeof(*d));
        d->Width = desc_.BufferDesc.Width;
        d->Height = desc_.BufferDesc.Height;
        d->Format = desc_.BufferDesc.Format;
        d->SampleDesc = desc_.SampleDesc;
        d->BufferUsage = desc_.BufferUsage;
        d->BufferCount = desc_.BufferCount;
        d->Scaling = DXGI_SCALING_STRETCH;
        d->SwapEffect = desc_.SwapEffect;
        d->AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        d->Flags = desc_.Flags;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d) override
    {
        if (!d) return E_INVALIDARG;
        std::memset(d, 0, sizeof(*d));
        d->Windowed = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* hw) override
    {
        if (!hw) return E_INVALIDARG;
        *hw = desc_.OutputWindow;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID, void** p) override
    {
        if (p) *p = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS*) override
    {
        return Present(sync, flags);
    }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return FALSE; }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** o) override
    {
        if (o) *o = nullptr;
        return DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* c) override
    {
        if (c) std::memset(c, 0, sizeof(*c));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* r) override
    {
        if (r) *r = DXGI_MODE_ROTATION_IDENTITY;
        return S_OK;
    }

    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* w, UINT* h) override
    {
        if (w) *w = desc_.BufferDesc.Width;
        if (h) *h = desc_.BufferDesc.Height;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* l) override
    {
        if (l) *l = 1;
        return S_OK;
    }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { return nullptr; }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* m) override
    {
        if (m) std::memset(m, 0, sizeof(*m));
        return S_OK;
    }

    // IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return 0; }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE cs, UINT* sup) override
    {
        if (sup) *sup = (cs == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) ? DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags,
                                             const UINT*, IUnknown* const*) override
    {
        return ResizeBuffers(count, w, h, fmt, flags);
    }

protected:
    std::atomic<ULONG> ref_{1};
    DXGI_SWAP_CHAIN_DESC desc_{};
    UINT presentCount_ = 0;
};

class SwapChain11 : public SwapChainBase {
public:
    SwapChain11(ID3D11Device* dev, HWND hwnd, UINT w, UINT h, DXGI_FORMAT fmt, UINT bufCount)
        : SwapChainBase(hwnd, w, h, fmt, bufCount), dev_(dev)
    {
        if (dev_) dev_->AddRef();
        CreateBackBuffer();
    }

    ~SwapChain11() override
    {
        if (backBuffer_) backBuffer_->Release();
        if (dev_) dev_->Release();
    }

    bool Valid() const { return dev_ != nullptr && backBuffer_ != nullptr; }

    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) override
    {
        if (!ppDevice) return E_POINTER;
        *ppDevice = nullptr;
        if (!dev_) return E_NOINTERFACE;
        return dev_->QueryInterface(riid, ppDevice);
    }

    HRESULT STDMETHODCALLTYPE Present(UINT, UINT) override
    {
        ++presentCount_;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetBuffer(UINT idx, REFIID riid, void** ppSurface) override
    {
        if (!ppSurface) return E_POINTER;
        *ppSurface = nullptr;
        if (idx != 0 || !backBuffer_) return DXGI_ERROR_INVALID_CALL;
        return backBuffer_->QueryInterface(riid, ppSurface);
    }

    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) override
    {
        if (backBuffer_) {
            backBuffer_->AddRef();
            const ULONG rc = backBuffer_->Release();
            if (rc > 1) return DXGI_ERROR_INVALID_CALL; // outstanding backbuffer reference leaked!
            backBuffer_->Release();
            backBuffer_ = nullptr;
        }
        if (count) desc_.BufferCount = count;
        if (w == 0 || h == 0) {
            RECT rc{};
            if (desc_.OutputWindow && ::GetClientRect(desc_.OutputWindow, &rc)) {
                if (w == 0) w = (UINT)(rc.right - rc.left);
                if (h == 0) h = (UINT)(rc.bottom - rc.top);
            }
        }
        if (w) desc_.BufferDesc.Width = w;
        if (h) desc_.BufferDesc.Height = h;
        if (fmt != DXGI_FORMAT_UNKNOWN) desc_.BufferDesc.Format = fmt;
        desc_.Flags = flags;
        return CreateBackBuffer();
    }

private:
    HRESULT CreateBackBuffer()
    {
        if (!dev_) return E_FAIL;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = desc_.BufferDesc.Width ? desc_.BufferDesc.Width : 64;
        td.Height = desc_.BufferDesc.Height ? desc_.BufferDesc.Height : 64;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = desc_.BufferDesc.Format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        return dev_->CreateTexture2D(&td, nullptr, &backBuffer_);
    }

    ID3D11Device* dev_ = nullptr;
    ID3D11Texture2D* backBuffer_ = nullptr;
};

#if defined(__ID3D12Device_INTERFACE_DEFINED__)
class SwapChain12 : public SwapChainBase {
public:
    SwapChain12(ID3D12Device* dev, HWND hwnd, UINT w, UINT h, DXGI_FORMAT fmt, UINT bufCount)
        : SwapChainBase(hwnd, w, h, fmt, bufCount), dev_(dev)
    {
        if (dev_) dev_->AddRef();
        CreateBuffers();
    }

    ~SwapChain12() override
    {
        ReleaseBuffers();
        if (dev_) dev_->Release();
    }

    bool Valid() const { return dev_ != nullptr && !buffers_.empty(); }

    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) override
    {
        if (!ppDevice) return E_POINTER;
        *ppDevice = nullptr;
        if (!dev_) return E_NOINTERFACE;
        return dev_->QueryInterface(riid, ppDevice);
    }

    HRESULT STDMETHODCALLTYPE Present(UINT, UINT) override
    {
        ++presentCount_;
        if (!buffers_.empty()) currentIndex_ = (currentIndex_ + 1) % (UINT)buffers_.size();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetBuffer(UINT idx, REFIID riid, void** ppSurface) override
    {
        if (!ppSurface) return E_POINTER;
        *ppSurface = nullptr;
        if (idx >= buffers_.size() || !buffers_[idx]) return DXGI_ERROR_INVALID_CALL;
        return buffers_[idx]->QueryInterface(riid, ppSurface);
    }

    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) override
    {
        for (ID3D12Resource* res : buffers_) {
            if (!res) continue;
            res->AddRef();
            const ULONG rc = res->Release();
            if (rc > 1) return DXGI_ERROR_INVALID_CALL; // outstanding buffer ref
        }
        ReleaseBuffers();
        if (count) desc_.BufferCount = count;
        if (w == 0 || h == 0) {
            RECT rc{};
            if (desc_.OutputWindow && ::GetClientRect(desc_.OutputWindow, &rc)) {
                if (w == 0) w = (UINT)(rc.right - rc.left);
                if (h == 0) h = (UINT)(rc.bottom - rc.top);
            }
        }
        if (w) desc_.BufferDesc.Width = w;
        if (h) desc_.BufferDesc.Height = h;
        if (fmt != DXGI_FORMAT_UNKNOWN) desc_.BufferDesc.Format = fmt;
        desc_.Flags = flags;
        currentIndex_ = 0;
        return CreateBuffers();
    }

    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return currentIndex_; }

private:
    void ReleaseBuffers()
    {
        for (ID3D12Resource* res : buffers_)
            if (res) res->Release();
        buffers_.clear();
    }

    HRESULT CreateBuffers()
    {
        if (!dev_ || desc_.BufferCount == 0) return E_FAIL;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = desc_.BufferDesc.Width ? desc_.BufferDesc.Width : 64;
        rd.Height = desc_.BufferDesc.Height ? desc_.BufferDesc.Height : 64;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = desc_.BufferDesc.Format;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        buffers_.resize(desc_.BufferCount, nullptr);
        for (UINT i = 0; i < desc_.BufferCount; ++i) {
            const HRESULT hr = dev_->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PRESENT, nullptr,
                __uuidof(ID3D12Resource), reinterpret_cast<void**>(&buffers_[i]));
            if (FAILED(hr)) {
                ReleaseBuffers();
                return hr;
            }
        }
        return S_OK;
    }

    ID3D12Device* dev_ = nullptr;
    std::vector<ID3D12Resource*> buffers_;
    UINT currentIndex_ = 0;
};
#endif

} // namespace detail

inline HRESULT CreateOffscreenD3D11(HWND hwnd, UINT w, UINT h, DXGI_FORMAT fmt, UINT bufCount,
                                    IDXGISwapChain** scOut, ID3D11Device** devOut, ID3D11DeviceContext** ctxOut)
{
    if (!scOut || !devOut || !ctxOut) return E_POINTER;
    *scOut = nullptr;
    *devOut = nullptr;
    *ctxOut = nullptr;
    const HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                           D3D11_SDK_VERSION, devOut, nullptr, ctxOut);
    if (FAILED(hr) || !*devOut) return FAILED(hr) ? hr : E_FAIL;
    auto* sc = new detail::SwapChain11(*devOut, hwnd, w, h, fmt, bufCount);
    if (!sc->Valid()) {
        sc->Release();
        (*ctxOut)->Release();
        (*devOut)->Release();
        *ctxOut = nullptr;
        *devOut = nullptr;
        return E_FAIL;
    }
    *scOut = static_cast<IDXGISwapChain*>(sc);
    return S_OK;
}

#if defined(__ID3D12Device_INTERFACE_DEFINED__)
inline HRESULT CreateOffscreenD3D12(ID3D12Device* dev, HWND hwnd, UINT w, UINT h, DXGI_FORMAT fmt, UINT bufCount,
                                    IDXGISwapChain1** scOut)
{
    if (!scOut || !dev) return E_POINTER;
    *scOut = nullptr;
    auto* sc = new detail::SwapChain12(dev, hwnd, w, h, fmt, bufCount);
    if (!sc->Valid()) {
        sc->Release();
        return E_FAIL;
    }
    *scOut = static_cast<IDXGISwapChain1*>(sc);
    return S_OK;
}
#endif

} // namespace offscreen_test
