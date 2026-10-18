#pragma once

#include <cstdint>
#include <vector>

namespace bag2mkv {

// Decompresses one bzip2 stream as stored in a ROSBAG V2.0 chunk.
std::vector<uint8_t> bz2_decompress(const std::vector<uint8_t> &src);

}  // namespace bag2mkv
