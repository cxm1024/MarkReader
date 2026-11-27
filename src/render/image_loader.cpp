#include "render/image_loader.h"
#include "render/d2d_context.h"

#include <winhttp.h>
#include <shlwapi.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <thread>

namespace mr {

namespace {

bool EndsWithNoCase(const std::wstring& s, const wchar_t* suffix) {
    size_t slen = wcslen(suffix);
    if (s.size() < slen) return false;
    return _wcsicmp(s.c_str() + (s.size() - slen), suffix) == 0;
}

bool LooksLikeSvgBytes(const std::vector<uint8_t>& bytes, const std::wstring& hint) {
    if (EndsWithNoCase(hint, L".svg")) return true;
    size_t check_len = std::min<size_t>(bytes.size(), 512);
    std::string head(reinterpret_cast<const char*>(bytes.data()), check_len);
    std::string lower = ToLowerAscii(head);
    return lower.find("<svg") != std::string::npos;
}

float ParseSvgLength(const std::string& val_str) {
    std::string t = TrimAscii(val_str);
    if (t.empty() || t.back() == '%') return 0.0f;
    char* end = nullptr;
    double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || v <= 0.0) return 0.0f;
    return static_cast<float>(v);
}

D2D1_COLOR_F ParseSvgHexColor(const std::string& val_str) {
    std::string t = TrimAscii(val_str);
    if (!t.empty() && t[0] == '#') {
        unsigned long rgb = std::strtoul(t.c_str() + 1, nullptr, 16);
        if (t.size() == 4) {
            uint8_t r = static_cast<uint8_t>((rgb >> 8) & 0xF);
            uint8_t g = static_cast<uint8_t>((rgb >> 4) & 0xF);
            uint8_t b = static_cast<uint8_t>(rgb & 0xF);
            return D2D1::ColorF((r * 17) / 255.0f, (g * 17) / 255.0f, (b * 17) / 255.0f, 1.0f);
        }
        if (t.size() == 7) {
            return D2D1::ColorF(static_cast<UINT32>(rgb), 1.0f);
        }
    }
    return D2D1::ColorF(0.12f, 0.14f, 0.18f, 1.0f);
}

std::string ExtractXmlAttr(const std::string& tag, const char* name) {
    std::string lower = ToLowerAscii(tag);
    std::string key = std::string(name) + "=";
    size_t pos = 0;
    while ((pos = lower.find(key, pos)) != std::string::npos) {
        if (pos > 0 && !std::isspace(static_cast<unsigned char>(lower[pos - 1]))) {
            pos += key.size();
            continue;
        }
        size_t vs = pos + key.size();
        while (vs < tag.size() && std::isspace(static_cast<unsigned char>(tag[vs]))) ++vs;
        if (vs >= tag.size()) return {};
        char q = tag[vs];
        if (q == '"' || q == '\'') {
            size_t ve = tag.find(q, vs + 1);
            if (ve != std::string::npos) return tag.substr(vs + 1, ve - vs - 1);
        }
        return {};
    }
    return {};
}

// Extracts top-level SVG <text> nodes since Direct2D ID2D1SvgDocument omits SVG text rendering.
std::vector<SvgTextLabel> ExtractSvgTextLabels(const std::string& xml) {
    std::vector<SvgTextLabel> labels;
    std::string lower = ToLowerAscii(xml);
    size_t pos = 0;
    while ((pos = lower.find("<text", pos)) != std::string::npos) {
        if (pos + 5 < lower.size() && !std::isspace(static_cast<unsigned char>(lower[pos + 5])) && lower[pos + 5] != '>') {
            pos += 5;
            continue;
        }
        size_t gt = xml.find('>', pos);
        if (gt == std::string::npos) break;
        size_t close = lower.find("</text>", gt + 1);
        if (close == std::string::npos) break;

        std::string tag = xml.substr(pos, gt - pos + 1);
        std::string inner = TrimAscii(xml.substr(gt + 1, close - (gt + 1)));
        if (!inner.empty()) {
            SvgTextLabel lbl;
            lbl.x = ParseSvgLength(ExtractXmlAttr(tag, "x"));
            lbl.y = ParseSvgLength(ExtractXmlAttr(tag, "y"));
            float fs = ParseSvgLength(ExtractXmlAttr(tag, "font-size"));
            if (fs > 0.0f) lbl.font_size = fs;
            std::string fw = ToLowerAscii(ExtractXmlAttr(tag, "font-weight"));
            lbl.bold = (fw == "bold" || fw == "600" || fw == "700" || fw == "800");
            std::string anc = ToLowerAscii(ExtractXmlAttr(tag, "text-anchor"));
            if (anc == "middle") lbl.anchor = 1;
            else if (anc == "end") lbl.anchor = 2;
            std::string fill = ExtractXmlAttr(tag, "fill");
            if (!fill.empty()) lbl.color = ParseSvgHexColor(fill);
            lbl.text = Utf8ToWide(inner);
            labels.push_back(std::move(lbl));
        }
        pos = close + 7;
    }
    return labels;
}

void ExtractSvgDimensions(const std::string& xml, float& out_w, float& out_h) {
    out_w = 480.0f;
    out_h = 320.0f;

    size_t svg_pos = ToLowerAscii(xml).find("<svg");
    if (svg_pos == std::string::npos) return;
    size_t tag_end = xml.find('>', svg_pos);
    if (tag_end == std::string::npos) return;
    std::string tag = xml.substr(svg_pos, tag_end - svg_pos + 1);
    std::string lower = ToLowerAscii(tag);

    auto get_attr = [&](const char* name) -> std::string {
        std::string key = std::string(name) + "=";
        size_t pos = 0;
        while ((pos = lower.find(key, pos)) != std::string::npos) {
            if (pos > 0 && !std::isspace(static_cast<unsigned char>(lower[pos - 1]))) {
                pos += key.size();
                continue;
            }
            size_t vs = pos + key.size();
            while (vs < tag.size() && std::isspace(static_cast<unsigned char>(tag[vs]))) ++vs;
            if (vs >= tag.size()) return {};
            char q = tag[vs];
            if (q == '"' || q == '\'') {
                size_t ve = tag.find(q, vs + 1);
                if (ve != std::string::npos) return tag.substr(vs + 1, ve - vs - 1);
            }
            return {};
        }
        return {};
    };

    float w = ParseSvgLength(get_attr("width"));
    float h = ParseSvgLength(get_attr("height"));
    if (w > 0.0f && h > 0.0f) {
        out_w = w;
        out_h = h;
        return;
    }

    std::string vb = get_attr("viewbox");
    if (!vb.empty()) {
        for (char& ch : vb) {
            if (ch == ',') ch = ' ';
        }
        double nums[4]{};
        int count = 0;
        const char* p = vb.c_str();
        while (*p && count < 4) {
            while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
            if (!*p) break;
            char* end = nullptr;
            nums[count] = std::strtod(p, &end);
            if (end == p) break;
            p = end;
            ++count;
        }
        if (count == 4 && nums[2] > 0.0 && nums[3] > 0.0) {
            out_w = static_cast<float>(nums[2]);
            out_h = static_cast<float>(nums[3]);
        }
    }
}

bool ReadWholeFileBytes(const std::wstring& path, std::vector<uint8_t>& out_bytes) {
    HANDLE hFile = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(hFile, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 64LL * 1024 * 1024) {
        CloseHandle(hFile);
        return false;
    }

    out_bytes.resize(static_cast<size_t>(sz.QuadPart));
    size_t total = 0;
    while (total < out_bytes.size()) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(out_bytes.size() - total, 1u << 20));
        DWORD read_bytes = 0;
        if (!ReadFile(hFile, out_bytes.data() + total, chunk, &read_bytes, nullptr)) {
            CloseHandle(hFile);
            return false;
        }
        if (read_bytes == 0) break;
        total += read_bytes;
    }
    out_bytes.resize(total);
    CloseHandle(hFile);
    return !out_bytes.empty();
}

ComPtr<IStream> CreateMemoryStream(const std::vector<uint8_t>& bytes) {
    ComPtr<IStream> stream;
    if (bytes.empty()) return stream;
    IStream* raw = SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size()));
    if (raw) {
        stream.Reset(raw);
    }
    return stream;
}

bool HttpDownloadBytes(const std::wstring& url, std::vector<uint8_t>& out_bytes) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[512]{};
    wchar_t path[2048]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) {
        return false;
    }

    HINTERNET hSession = WinHttpOpen(
        L"MarkReader/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 10000);

    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", path[0] ? path : L"/", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    bool ok = false;
    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD status = 0;
        DWORD status_sz = sizeof(status);
        WinHttpQueryHeaders(
            hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz, WINHTTP_NO_HEADER_INDEX);
        if (status >= 200 && status < 300) {
            constexpr size_t kMaxBytes = 32 * 1024 * 1024;
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
                if (out_bytes.size() + avail > kMaxBytes) break;
                size_t old_sz = out_bytes.size();
                out_bytes.resize(old_sz + avail);
                DWORD read = 0;
                if (!WinHttpReadData(hRequest, out_bytes.data() + old_sz, avail, &read)) {
                    out_bytes.clear();
                    break;
                }
                out_bytes.resize(old_sz + read);
            }
            ok = !out_bytes.empty();
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

}  // namespace

ImageCache::~ImageCache() {
    shutting_down_ = true;
    Clear();
}

void ImageCache::Clear() {
    cache_.clear();
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_results_.clear();
}

std::shared_ptr<LoadedImage> ImageCache::GetOrLoad(const std::wstring& resolved_path) {
    if (resolved_path.empty()) return nullptr;
    if (auto it = cache_.find(resolved_path); it != cache_.end()) {
        return it->second;
    }

    if (IsRemoteUrl(resolved_path)) {
        auto img = std::make_shared<LoadedImage>();
        img->status = LoadedImage::Status::Loading;
        cache_[resolved_path] = img;
        StartRemoteFetch(resolved_path);
        return img;
    }

    auto img = LoadFromLocalFile(resolved_path);
    cache_[resolved_path] = img;
    return img;
}

std::shared_ptr<LoadedImage> ImageCache::LoadFromLocalFile(const std::wstring& path) {
    std::vector<uint8_t> bytes;
    if (!ReadWholeFileBytes(path, bytes)) {
        auto failed = std::make_shared<LoadedImage>();
        failed->status = LoadedImage::Status::Failed;
        return failed;
    }
    return DecodeFromBytes(bytes, path);
}

std::shared_ptr<LoadedImage> ImageCache::DecodeFromBytes(const std::vector<uint8_t>& bytes, const std::wstring& hint) {
    auto res = std::make_shared<LoadedImage>();
    auto* ctx = D2DContext::Instance();
    if (!ctx || !ctx->D2D() || !ctx->WIC() || bytes.empty()) {
        res->status = LoadedImage::Status::Failed;
        return res;
    }

    if (LooksLikeSvgBytes(bytes, hint)) {
        std::string xml(reinterpret_cast<const char*>(bytes.data()), std::min<size_t>(bytes.size(), 256 * 1024));
        float w = 480.0f, h = 320.0f;
        ExtractSvgDimensions(xml, w, h);
        ComPtr<IStream> stream = CreateMemoryStream(bytes);
        if (stream) {
            ComPtr<ID2D1SvgDocument> svg;
            HRESULT hr = ctx->D2D()->CreateSvgDocument(stream.Get(), D2D1::SizeF(w, h), svg.Put());
            if (SUCCEEDED(hr) && svg) {
                res->status = LoadedImage::Status::Ready;
                res->is_svg = true;
                res->width = w;
                res->height = h;
                res->svg_doc = svg;
                res->svg_labels = ExtractSvgTextLabels(xml);
                return res;
            }
        }
    }

    ComPtr<IStream> stream = CreateMemoryStream(bytes);
    if (!stream) {
        res->status = LoadedImage::Status::Failed;
        return res;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = ctx->WIC()->CreateDecoderFromStream(
        stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.Put());
    if (FAILED(hr) || !decoder) {
        res->status = LoadedImage::Status::Failed;
        return res;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.Put());
    if (FAILED(hr) || !frame) {
        res->status = LoadedImage::Status::Failed;
        return res;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = ctx->WIC()->CreateFormatConverter(converter.Put());
    if (FAILED(hr) || !converter) {
        res->status = LoadedImage::Status::Failed;
        return res;
    }

    hr = converter->Initialize(
        frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        res->status = LoadedImage::Status::Failed;
        return res;
    }

    UINT pw = 0, ph = 0;
    frame->GetSize(&pw, &ph);
    ComPtr<ID2D1Bitmap1> bmp;
    hr = ctx->D2D()->CreateBitmapFromWicBitmap(converter.Get(), nullptr, bmp.Put());
    if (FAILED(hr) || !bmp || pw == 0 || ph == 0) {
        res->status = LoadedImage::Status::Failed;
        return res;
    }

    res->status = LoadedImage::Status::Ready;
    res->is_svg = false;
    res->width = static_cast<float>(pw);
    res->height = static_cast<float>(ph);
    res->bitmap = bmp;
    return res;
}

void ImageCache::StartRemoteFetch(const std::wstring& url) {
    HWND hwnd = notify_hwnd_;
    std::thread([this, url, hwnd]() {
        PendingDownload item;
        item.key = url;
        item.success = HttpDownloadBytes(url, item.bytes);
        if (shutting_down_.load()) return;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_results_.push_back(std::move(item));
        }
        if (hwnd && IsWindow(hwnd)) {
            PostMessageW(hwnd, WM_APP_IMAGE_LOADED, 0, 0);
        }
    }).detach();
}

void ImageCache::ProcessCompletedDownloads() {
    std::vector<PendingDownload> batch;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        batch.swap(pending_results_);
    }

    for (auto& item : batch) {
        auto it = cache_.find(item.key);
        if (it == cache_.end()) continue;
        if (!item.success) {
            it->second->status = LoadedImage::Status::Failed;
            continue;
        }
        auto decoded = DecodeFromBytes(item.bytes, item.key);
        *it->second = *decoded;
    }
}

void DrawLoadedImage(ID2D1DeviceContext5* dc, const LoadedImage& image, const D2D1_RECT_F& dest_rect) {
    if (!dc || image.status != LoadedImage::Status::Ready) return;

    if (image.is_svg && image.svg_doc) {
        dc->PushAxisAlignedClip(dest_rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        D2D1_MATRIX_3X2_F saved{};
        dc->GetTransform(&saved);
        float sx = (image.width > 0.0f) ? ((dest_rect.right - dest_rect.left) / image.width) : 1.0f;
        float sy = (image.height > 0.0f) ? ((dest_rect.bottom - dest_rect.top) / image.height) : 1.0f;
        D2D1::Matrix3x2F xform = D2D1::Matrix3x2F::Scale(sx, sy) *
                                 D2D1::Matrix3x2F::Translation(dest_rect.left, dest_rect.top);
        dc->SetTransform(xform * saved);
        dc->DrawSvgDocument(image.svg_doc.Get());

        auto* ctx = D2DContext::Instance();
        if (ctx && ctx->DWrite() && !image.svg_labels.empty()) {
            ComPtr<IDWriteTextFormat> fmt;
            ComPtr<ID2D1SolidColorBrush> brush;
            dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), brush.Put());
            for (const auto& lbl : image.svg_labels) {
                if (lbl.text.empty() || !brush) continue;
                fmt.Reset();
                if (SUCCEEDED(ctx->DWrite()->CreateTextFormat(
                        L"Segoe UI", nullptr,
                        lbl.bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                        std::max(6.0f, lbl.font_size), L"", fmt.Put()))) {
                    ComPtr<IDWriteTextLayout> tl;
                    if (SUCCEEDED(ctx->DWrite()->CreateTextLayout(
                            lbl.text.c_str(), static_cast<UINT32>(lbl.text.size()),
                            fmt.Get(), 2000.0f, 500.0f, tl.Put()))) {
                        DWRITE_TEXT_METRICS tm{};
                        DWRITE_LINE_METRICS lm{};
                        UINT32 lc = 0;
                        tl->GetMetrics(&tm);
                        tl->GetLineMetrics(&lm, 1, &lc);
                        float draw_x = lbl.x;
                        if (lbl.anchor == 1) draw_x -= tm.widthIncludingTrailingWhitespace * 0.5f;
                        else if (lbl.anchor == 2) draw_x -= tm.widthIncludingTrailingWhitespace;
                        float draw_y = lbl.y - (lc > 0 ? lm.baseline : lbl.font_size * 0.8f);
                        brush->SetColor(lbl.color);
                        dc->DrawTextLayout(D2D1::Point2F(draw_x, draw_y), tl.Get(), brush.Get());
                    }
                }
            }
        }
        dc->SetTransform(saved);
        dc->PopAxisAlignedClip();
        return;
    }

    if (image.bitmap) {
        dc->DrawBitmap(image.bitmap.Get(), &dest_rect, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    }
}

}  // namespace mr
