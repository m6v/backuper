#pragma once
#include <cstdint>
#include <cstddef>
#include <array>
#include <zlib.h>

inline constexpr std::array<char, 8> IMAGE_MAGIC = {'E', 'X', 'T', 'I', 'M', 'A', 'G', 'E'};
inline constexpr uint32_t IMAGE_VERSION = 3;

#pragma pack(push, 1)
struct ImageHeader {
    std::array<char, 8> magic;  // Магическая последовательность
    uint32_t version;           // Версия формата
    uint32_t block_size;        // Размер блока ФС (обычно 4096)
    uint64_t total_blocks;      // Общее число блоков ФС
    uint64_t used_blocks;       // Число занятых блоков (для прогресс-бара)
    uint32_t header_crc32;      // CRC32 первых 32 байт этого заголовка
};

struct ChunkHeader {
    uint64_t start_block;       // Начальный блок непрерывного диапазона
    uint32_t block_count;       // Количество блоков в чанке (0 = конец потока)
    uint32_t header_crc32;      // CRC32 полей start_block и block_count
    uint32_t data_crc32;        // CRC32 полезных данных чанка
};
#pragma pack(pop)

// Вычисление контрольной суммы CRC32 (IEEE 802.3) с использованием zlib
inline uint32_t compute_crc32(const void* data, size_t length, uint32_t initial = 0) {
    return static_cast<uint32_t>(
        ::crc32(initial, static_cast<const Bytef*>(data), static_cast<uInt>(length))
    );
}
