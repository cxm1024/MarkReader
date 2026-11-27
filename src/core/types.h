#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mr {

// Minimal RAII COM pointer (works cleanly across MinGW64 without ATL/WRL).
template <typename T>
class ComPtr {
public:
    ComPtr() noexcept = default;
    ComPtr(std::nullptr_t) noexcept {}
    explicit ComPtr(T* ptr) noexcept : ptr_(ptr) {}

    ComPtr(const ComPtr& other) noexcept : ptr_(other.ptr_) {
        if (ptr_) ptr_->AddRef();
    }

    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    ~ComPtr() {
        Reset();
    }

    ComPtr& operator=(std::nullptr_t) noexcept {
        Reset();
        return *this;
    }

    ComPtr& operator=(const ComPtr& other) noexcept {
        if (this != &other) {
            ComPtr tmp(other);
            Swap(tmp);
        }
        return *this;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    T* Get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    T** Put() noexcept {
        Reset();
        return &ptr_;
    }

    void** PutVoid() noexcept {
        return reinterpret_cast<void**>(Put());
    }

    void Reset(T* ptr = nullptr) noexcept {
        T* old = ptr_;
        ptr_ = ptr;
        if (old) old->Release();
    }

    void Swap(ComPtr& other) noexcept {
        T* tmp = ptr_;
        ptr_ = other.ptr_;
        other.ptr_ = tmp;
    }

    T* Detach() noexcept {
        T* p = ptr_;
        ptr_ = nullptr;
        return p;
    }

private:
    T* ptr_ = nullptr;
};

struct Color4 {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    static constexpr Color4 FromHex(uint32_t rgb, float alpha = 1.0f) noexcept {
        return {
            static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
            static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
            static_cast<float>(rgb & 0xFF) / 255.0f,
            alpha
        };
    }

    constexpr uint32_t ToTexArgb() const noexcept {
        auto clamp_byte = [](float v) -> uint32_t {
            if (v <= 0.0f) return 0;
            if (v >= 1.0f) return 255;
            return static_cast<uint32_t>(v * 255.0f + 0.5f);
        };
        return (clamp_byte(a) << 24) | (clamp_byte(r) << 16) | (clamp_byte(g) << 8) | clamp_byte(b);
    }

    constexpr uint32_t ToColorRef() const noexcept {
        auto clamp_byte = [](float v) -> uint32_t {
            if (v <= 0.0f) return 0;
            if (v >= 1.0f) return 255;
            return static_cast<uint32_t>(v * 255.0f + 0.5f);
        };
        return clamp_byte(r) | (clamp_byte(g) << 8) | (clamp_byte(b) << 16);
    }
};

enum class ThemeMode {
    Light = 0,
    Dark = 1,
    Sepia = 2
};

struct AppConfig {
    ThemeMode theme = ThemeMode::Light;
    int zoom_percent = 100;
    bool outline_visible = true;
    float outline_width = 256.0f;
    int window_width = 1240;
    int window_height = 840;
    bool window_maximized = false;
    std::vector<std::wstring> recent_files;
};

std::wstring Utf8ToWide(const std::string& utf8);
std::wstring Utf8ToWide(const char* data, size_t len);
std::string WideToUtf8(const std::wstring& wide);
std::string WideToUtf8(const wchar_t* data, size_t len);

std::string TrimAscii(const std::string& s);
std::string ToLowerAscii(const std::string& s);
std::wstring UrlDecodeWide(const std::wstring& s);
std::string DecodeHtmlEntityUtf8(const char* entity, size_t len);
std::string SlugifyHeading(const std::string& text);

bool IsRemoteUrl(const std::wstring& url);
bool IsWindowsAbsolutePath(const std::wstring& path);
std::wstring GetExecutableDir();

bool LoadTextFileUtf8(const std::wstring& path, std::string& out_utf8, std::wstring& out_error);
AppConfig LoadAppConfig();
void SaveAppConfig(const AppConfig& cfg);
void AddRecentFile(AppConfig& cfg, const std::wstring& path);

}  // namespace mr
