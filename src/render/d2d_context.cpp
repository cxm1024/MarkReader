#include "render/d2d_context.h"

#include <algorithm>

namespace mr {

namespace {
D2DContext* g_instance = nullptr;
}

D2DContext* D2DContext::Instance() {
    return g_instance;
}

D2DContext::~D2DContext() {
    Shutdown();
}

bool D2DContext::Initialize(HWND hwnd, UINT width, UINT height, float dpi) {
    hwnd_ = hwnd;
    width_ = std::max<UINT>(1, width);
    height_ = std::max<UINT>(1, height);
    dpi_ = (dpi > 0.0f) ? dpi : 96.0f;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return false;
    }

    D2D1_FACTORY_OPTIONS opts{};
    hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory4),
        &opts,
        d2d_factory_.PutVoid());
    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory5),
        reinterpret_cast<IUnknown**>(dwrite_factory_.Put()));
    if (FAILED(hr)) return false;

    hr = CoCreateInstance(
        CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(wic_factory_.Put()));
    if (FAILED(hr)) {
        hr = CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(wic_factory_.Put()));
        if (FAILED(hr)) return false;
    }

    if (!CreateDeviceResources()) return false;
    if (!CreateSwapChainAndTarget(width_, height_)) return false;

    g_instance = this;
    return true;
}

bool D2DContext::CreateDeviceResources() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_1
    };

    D3D_FEATURE_LEVEL obtained{};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
        d3d_device_.Put(), &obtained, d3d_context_.Put());
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            d3d_device_.Put(), &obtained, d3d_context_.Put());
        if (FAILED(hr)) return false;
    }

    hr = d3d_device_->QueryInterface(__uuidof(IDXGIDevice), dxgi_device_.PutVoid());
    if (FAILED(hr)) return false;

    hr = d2d_factory_->CreateDevice(dxgi_device_.Get(), d2d_device_.Put());
    if (FAILED(hr)) return false;

    ComPtr<ID2D1DeviceContext3> dc3;
    hr = d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc3.Put());
    if (FAILED(hr) || !dc3) return false;

    hr = dc3->QueryInterface(__uuidof(ID2D1DeviceContext5), d2d_dc_.PutVoid());
    if (FAILED(hr) || !d2d_dc_) return false;

    d2d_dc_->SetDpi(dpi_, dpi_);
    d2d_dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return true;
}

bool D2DContext::CreateSwapChainAndTarget(UINT width, UINT height) {
    ComPtr<IDXGIAdapter> adapter;
    HRESULT hr = dxgi_device_->GetAdapter(adapter.Put());
    if (FAILED(hr)) return false;

    ComPtr<IDXGIFactory2> dxgi_factory;
    hr = adapter->GetParent(__uuidof(IDXGIFactory2), dxgi_factory.PutVoid());
    if (FAILED(hr)) return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    hr = dxgi_factory->CreateSwapChainForHwnd(
        d3d_device_.Get(), hwnd_, &desc, nullptr, nullptr, swap_chain_.Put());
    if (FAILED(hr)) {
        // Fallback for older drivers
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = dxgi_factory->CreateSwapChainForHwnd(
            d3d_device_.Get(), hwnd_, &desc, nullptr, nullptr, swap_chain_.Put());
        if (FAILED(hr)) return false;
    }

    ComPtr<IDXGISurface> surface;
    hr = swap_chain_->GetBuffer(0, __uuidof(IDXGISurface), surface.PutVoid());
    if (FAILED(hr)) return false;

    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        dpi_, dpi_);
    hr = d2d_dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, target_bitmap_.Put());
    if (FAILED(hr)) return false;

    d2d_dc_->SetTarget(target_bitmap_.Get());
    return true;
}

void D2DContext::ReleaseSwapChainTarget() {
    if (d2d_dc_) d2d_dc_->SetTarget(nullptr);
    target_bitmap_.Reset();
}

bool D2DContext::Resize(UINT width, UINT height, float dpi) {
    width_ = std::max<UINT>(1, width);
    height_ = std::max<UINT>(1, height);
    if (dpi > 0.0f) dpi_ = dpi;
    if (!swap_chain_ || !d2d_dc_) return false;

    ReleaseSwapChainTarget();
    HRESULT hr = swap_chain_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        swap_chain_.Reset();
        d2d_dc_.Reset();
        d2d_device_.Reset();
        dxgi_device_.Reset();
        d3d_context_.Reset();
        d3d_device_.Reset();
        return CreateDeviceResources() && CreateSwapChainAndTarget(width_, height_);
    }
    if (FAILED(hr)) return false;

    ComPtr<IDXGISurface> surface;
    hr = swap_chain_->GetBuffer(0, __uuidof(IDXGISurface), surface.PutVoid());
    if (FAILED(hr)) return false;

    d2d_dc_->SetDpi(dpi_, dpi_);
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        dpi_, dpi_);
    hr = d2d_dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, target_bitmap_.Put());
    if (FAILED(hr)) return false;

    d2d_dc_->SetTarget(target_bitmap_.Get());
    return true;
}

bool D2DContext::BeginDraw() {
    if (!d2d_dc_) return false;
    d2d_dc_->BeginDraw();
    d2d_dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}

bool D2DContext::EndDraw() {
    if (!d2d_dc_ || !swap_chain_) return false;
    HRESULT hr = d2d_dc_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        swap_chain_.Reset();
        d2d_dc_.Reset();
        d2d_device_.Reset();
        dxgi_device_.Reset();
        d3d_context_.Reset();
        d3d_device_.Reset();
        if (CreateDeviceResources()) {
            CreateSwapChainAndTarget(width_, height_);
        }
        return false;
    }
    if (FAILED(hr)) return false;

    hr = swap_chain_->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        Resize(width_, height_, dpi_);
        return false;
    }
    return SUCCEEDED(hr);
}

void D2DContext::Shutdown() {
    if (g_instance == this) g_instance = nullptr;
    ReleaseSwapChainTarget();
    swap_chain_.Reset();
    d2d_dc_.Reset();
    d2d_device_.Reset();
    dxgi_device_.Reset();
    d3d_context_.Reset();
    d3d_device_.Reset();
    d2d_factory_.Reset();
    dwrite_factory_.Reset();
    wic_factory_.Reset();
}

}  // namespace mr
