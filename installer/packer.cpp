#include "payload_format.h"

#include <windows.h>
#include <compressapi.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void AppendU32(std::vector<uint8_t>& buf, uint32_t v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    buf.insert(buf.end(), p, p + sizeof(v));
}

void AppendU64(std::vector<uint8_t>& buf, uint64_t v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    buf.insert(buf.end(), p, p + sizeof(v));
}

std::string WideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), len, nullptr, nullptr);
    return out;
}

bool ReadFileAll(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream ifs(p, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    auto sz = ifs.tellg();
    if (sz < 0) return false;
    out.resize(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    if (sz > 0 && !ifs.read(reinterpret_cast<char*>(out.data()), sz)) return false;
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 5) {
        std::wcerr << L"Usage: packer <output_payload.bin> <MarkReader.exe> <Uninstall.exe> <res_dir>\n";
        return 1;
    }

    const fs::path out_path = argv[1];
    const fs::path app_exe = argv[2];
    const fs::path uninst_exe = argv[3];
    const fs::path res_dir = argv[4];

    struct Entry {
        std::string rel_utf8;
        fs::path src_path;
    };
    std::vector<Entry> entries;
    entries.push_back({"MarkReader.exe", app_exe});
    entries.push_back({"Uninstall.exe", uninst_exe});

    if (!fs::exists(res_dir) || !fs::is_directory(res_dir)) {
        std::wcerr << L"Missing res dir: " << res_dir.wstring() << L"\n";
        return 1;
    }

    std::vector<fs::path> res_files;
    for (const auto& it : fs::recursive_directory_iterator(res_dir)) {
        if (it.is_regular_file()) {
            res_files.push_back(it.path());
        }
    }
    std::sort(res_files.begin(), res_files.end());

    for (const auto& rf : res_files) {
        fs::path rel = fs::path("res") / fs::relative(rf, res_dir);
        std::wstring wrel = rel.wstring();
        for (wchar_t& ch : wrel) {
            if (ch == L'/') ch = L'\\';
        }
        entries.push_back({WideToUtf8(wrel), rf});
    }

    std::vector<uint8_t> archive;
    archive.insert(archive.end(), mr::pkg::kArchiveMagic, mr::pkg::kArchiveMagic + 4);
    AppendU32(archive, static_cast<uint32_t>(entries.size()));

    for (const auto& e : entries) {
        std::vector<uint8_t> data;
        if (!ReadFileAll(e.src_path, data)) {
            std::wcerr << L"Failed to read: " << e.src_path.wstring() << L"\n";
            return 1;
        }
        AppendU32(archive, static_cast<uint32_t>(e.rel_utf8.size()));
        archive.insert(archive.end(), e.rel_utf8.begin(), e.rel_utf8.end());
        AppendU64(archive, static_cast<uint64_t>(data.size()));
        archive.insert(archive.end(), data.begin(), data.end());
    }

    COMPRESSOR_HANDLE compressor = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &compressor)) {
        std::wcerr << L"CreateCompressor(LZMS) failed: " << GetLastError() << L"\n";
        return 1;
    }

    SIZE_T comp_buf_size = 0;
    Compress(compressor, archive.data(), archive.size(), nullptr, 0, &comp_buf_size);
    std::vector<uint8_t> comp_buf(comp_buf_size);
    SIZE_T comp_size = 0;
    if (!Compress(compressor, archive.data(), archive.size(), comp_buf.data(), comp_buf.size(), &comp_size)) {
        std::wcerr << L"Compress failed: " << GetLastError() << L"\n";
        CloseCompressor(compressor);
        return 1;
    }
    CloseCompressor(compressor);
    comp_buf.resize(comp_size);

    std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        std::wcerr << L"Failed to open output: " << out_path.wstring() << L"\n";
        return 1;
    }
    mr::pkg::CompressedHeader hdr{};
    memcpy(hdr.magic, mr::pkg::kCompressedMagic, 4);
    hdr.uncompressed_size = archive.size();
    ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    ofs.write(reinterpret_cast<const char*>(comp_buf.data()), static_cast<std::streamsize>(comp_buf.size()));

    std::wcout << L"Packed " << entries.size() << L" files: "
               << archive.size() << L" -> " << comp_buf.size() << L" bytes\n";
    return 0;
}
