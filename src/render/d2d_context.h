#pragma once

#include "core/types.h"

#include <d2d1_3.h>
#include <d3d11.h>
#include <dwrite_3.h>
#include <dxgi1_2.h>
#include <wincodec.h>

namespace mr {

// Manages the Direct3D 11 device, DXGI Flip-Sequential swap chain,
// Direct2D 1.3 device context (supporting native SVG), DirectWrite 3 factory, and WIC factory.
class D2DContext {
public:
    D2DContext() = default;
    ~D2DContext();

    D2DContext(const D2DContext&) = delete;
    D2DContext& operator=(const D2DContext&) = delete;

    bool Initialize(HWND hwnd, UINT width, UINT height, float dpi);
    bool Resize(UINT width, UINT height, float dpi);
    void Shutdown();

    bool BeginDraw();
    bool EndDraw(const D2D1_COLOR_F& background);

    ID2D1DeviceContext5* D2D() const { return d2d_dc_.Get(); }
    ID2D1Factory4* D2DFactory() const { return d2d_factory_.Get(); }
    IDWriteFactory5* DWrite() const { return dwrite_factory_.Get(); }
    IWICImagingFactory* WIC() const { return wic_factory_.Get(); }
    float Dpi() const { return dpi_; }
    float DpiScale() const { return dpi_ / 96.0f; }

    // Global DirectWrite & WIC factories accessible to the MicroTeX backend and caches.
    static D2DContext* Instance();

private:
    bool CreateDeviceResources();
    bool CreateSwapChainAndTarget(UINT width, UINT height);
    void ReleaseSwapChainTarget();

    HWND hwnd_ = nullptr;
    UINT width_ = 1;
    UINT height_ = 1;
    float dpi_ = 96.0f;

    ComPtr<ID3D11Device> d3d_device_;
    ComPtr<ID3D11DeviceContext> d3d_context_;
    ComPtr<IDXGIDevice> dxgi_device_;
    ComPtr<IDXGISwapChain1> swap_chain_;

    ComPtr<ID2D1Factory4> d2d_factory_;
    ComPtr<ID2D1Device3> d2d_device_;
    ComPtr<ID2D1DeviceContext5> d2d_dc_;
    ComPtr<ID2D1Bitmap1> target_bitmap_;

    ComPtr<IDWriteFactory5> dwrite_factory_;
    ComPtr<IWICImagingFactory> wic_factory_;
};

}  // namespace mr
