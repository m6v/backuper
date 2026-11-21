#pragma once
#include <cstdint>
#include <cstddef>
#include <array>

#pragma pack(push, 1)
struct ImageHeader {
    char magic[8];          // "MYEXT4\0\0"
    uint32_t version;       // 2 (обновили версию из-за добавления CRC32)
    uint32_t block_size;    // Например: 4096
    uint64_t total_blocks;  // Общее число блоков ФС
    uint32_t header_crc32;  // CRC32 первых 24 байт этого заголовка
};

struct ChunkHeader {
    uint64_t start_block;   // Начальный блок чанка
    uint32_t block_count;   // Количество блоков подряд (0 = конец потока)
    uint32_t header_crc32;  // CRC32 полей start_block и block_count (12 байт)
    uint32_t data_crc32;    // CRC32 полезных данных чанка
};
#pragma pack(pop)

// Быстрый табличный CRC32 (полином 0xEDB88320, стандартный IEEE 802.3)
inline uint32_t compute_crc32(const void* data, size_t length, uint32_t initial = 0) {
    static constexpr auto table = []() {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();

    uint32_t crc = ~initial;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; ++i) {
        crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}
