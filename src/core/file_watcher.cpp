#include "core/file_watcher.h"

#include <windows.h>

namespace mr {

static bool QueryFileStamp(const std::wstring& path, uint64_t& write_time, uint64_t& file_size) {
    if (path.empty()) return false;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return false;
    }
    write_time = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                 data.ftLastWriteTime.dwLowDateTime;
    file_size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return true;
}

void FileWatcher::Watch(const std::wstring& path) {
    path_ = path;
    initialized_ = QueryFileStamp(path_, last_write_time_, last_size_);
}

void FileWatcher::Clear() {
    path_.clear();
    last_write_time_ = 0;
    last_size_ = 0;
    initialized_ = false;
}

bool FileWatcher::HasChanged() {
    if (path_.empty()) return false;
    uint64_t wt = 0, sz = 0;
    if (!QueryFileStamp(path_, wt, sz)) return false;
    if (!initialized_) {
        last_write_time_ = wt;
        last_size_ = sz;
        initialized_ = true;
        return false;
    }
    if (wt != last_write_time_ || sz != last_size_) {
        last_write_time_ = wt;
        last_size_ = sz;
        return true;
    }
    return false;
}

}  // namespace mr
