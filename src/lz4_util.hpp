#pragma once

#include <cstdint>
#include <vector>

namespace bag2mkv {

std::vector<uint8_t> lz4_decompress(const std::vector<uint8_t> &src);
std::vector<uint8_t> lz4_compress(const std::vector<uint8_t> &src);

}  // namespace bag2mkv
