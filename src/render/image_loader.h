#pragma once

#include "core/types.h"

#include <d2d1_3.h>
#include <wincodec.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mr {

constexpr UINT WM_APP_IMAGE_LOADED = WM_APP + 101;

struct SvgTextLabel {
    float x = 0.0f;
    float y = 0.0f;
    float font_size = 14.0f;
    bool bold = false;
    int anchor = 0;  // 0=start, 1=middle, 2=end
    D2D1_COLOR_F color{0.1f, 0.1f, 0.1f, 1.0f};
    std::wstring text;
};

struct LoadedImage {
    enum class Status {
        Loading = 0,
        Ready,
        Failed
    };

    Status status = Status::Loading;
    bool is_svg = false;
    float width = 0.0f;
    float height = 0.0f;
    ComPtr<ID2D1Bitmap1> bitmap;
    ComPtr<ID2D1SvgDocument> svg_doc;
    std::vector<SvgTextLabel> svg_labels;
};

// Draws a ready LoadedImage (bitmap or SVG + fallback <text> labels) into dest_rect.
void DrawLoadedImage(ID2D1DeviceContext5* dc, const LoadedImage& image, const D2D1_RECT_F& dest_rect);

// Loads and caches local raster/SVG images and asynchronously fetches remote HTTP/HTTPS images.
class ImageCache {
public:
    ImageCache() = default;
    ~ImageCache();

    void SetNotifyHwnd(HWND hwnd) { notify_hwnd_ = hwnd; }

    // Resolves and returns cached image state; triggers async download for remote URLs.
    std::shared_ptr<LoadedImage> GetOrLoad(const std::wstring& resolved_path);

    // Must be called on the UI thread when WM_APP_IMAGE_LOADED arrives to finalize D2D bitmaps/SVGs.
    void ProcessCompletedDownloads();

    void Clear();

private:
    struct PendingDownload {
        std::wstring key;
        std::vector<uint8_t> bytes;
        bool success = false;
    };

    std::shared_ptr<LoadedImage> LoadFromLocalFile(const std::wstring& path);
    std::shared_ptr<LoadedImage> DecodeFromBytes(const std::vector<uint8_t>& bytes, const std::wstring& hint);
    void StartRemoteFetch(const std::wstring& url);

    HWND notify_hwnd_ = nullptr;
    std::unordered_map<std::wstring, std::shared_ptr<LoadedImage>> cache_;
    std::mutex pending_mutex_;
    std::vector<PendingDownload> pending_results_;
    std::atomic<bool> shutting_down_{false};
};

}  // namespace mr
