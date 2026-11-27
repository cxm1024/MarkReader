#pragma once

#include <cstdint>

namespace mr::pkg {

inline constexpr char kArchiveMagic[4] = {'M', 'R', 'P', 'K'};
inline constexpr char kCompressedMagic[4] = {'M', 'R', 'L', 'Z'};
inline constexpr int kPayloadResourceId = 102;

#pragma pack(push, 1)
struct CompressedHeader {
    char magic[4];
    uint64_t uncompressed_size;
};

struct ArchiveHeader {
    char magic[4];
    uint32_t file_count;
};
#pragma pack(pop)

}  // namespace mr::pkg
