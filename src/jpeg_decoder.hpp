#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bag2mkv {

class JpegDecoder {
public:
    JpegDecoder();
    ~JpegDecoder();

    JpegDecoder(const JpegDecoder &) = delete;
    JpegDecoder &operator=(const JpegDecoder &) = delete;

    // Decodes a baseline/progressive JPEG into tightly packed BGRA (B,G,R,A).
    void decode(const uint8_t *jpeg, size_t size, std::vector<uint8_t> &bgra, uint32_t &width, uint32_t &height);

private:
    void *factory_ = nullptr;
    bool com_initialized_ = false;
};

}  // namespace bag2mkv
