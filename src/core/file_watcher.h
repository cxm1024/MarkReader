#pragma once

#include <cstdint>
#include <string>

namespace mr {

// Lightweight polling / file-timestamp watcher for auto-reloading externally edited Markdown files.
class FileWatcher {
public:
    void Watch(const std::wstring& path);
    void Clear();
    bool HasChanged();

private:
    std::wstring path_;
    uint64_t last_write_time_ = 0;
    uint64_t last_size_ = 0;
    bool initialized_ = false;
};

}  // namespace mr
