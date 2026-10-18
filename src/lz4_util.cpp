#include "lz4_util.hpp"

#include <lz4frame.h>

#include <algorithm>
#include <stdexcept>

namespace bag2mkv {

std::vector<uint8_t> lz4_decompress(const std::vector<uint8_t> &src) {
    if (src.empty()) {
        throw std::runtime_error("empty LZ4 chunk");
    }
    LZ4F_dctx *ctx = nullptr;
    size_t rc = LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION);
    if (LZ4F_isError(rc)) {
        throw std::runtime_error(std::string("LZ4F_createDecompressionContext failed: ") + LZ4F_getErrorName(rc));
    }

    std::vector<uint8_t> out;
    out.resize(std::max<size_t>(1 << 20, src.size() * 4));
    size_t dst_pos = 0;
    size_t src_pos = 0;
    bool done = false;

    try {
        while (src_pos < src.size()) {
            size_t dst_size = out.size() - dst_pos;
            size_t src_size = src.size() - src_pos;
            rc = LZ4F_decompress(ctx, out.data() + dst_pos, &dst_size, src.data() + src_pos, &src_size, nullptr);
            if (LZ4F_isError(rc)) {
                throw std::runtime_error(std::string("LZ4F_decompress failed: ") + LZ4F_getErrorName(rc));
            }
            dst_pos += dst_size;
            src_pos += src_size;
            if (rc == 0) {
                done = true;
                break;
            }
            if (dst_pos == out.size()) {
                out.resize(out.size() * 2);
            }
            if (dst_size == 0 && src_size == 0) {
                throw std::runtime_error("LZ4 decompressor made no progress");
            }
        }
    } catch (...) {
        LZ4F_freeDecompressionContext(ctx);
        throw;
    }

    LZ4F_freeDecompressionContext(ctx);
    if (!done) {
        throw std::runtime_error("LZ4 frame ended unexpectedly");
    }
    if (src_pos != src.size()) {
        throw std::runtime_error("trailing bytes after LZ4 frame");
    }
    out.resize(dst_pos);
    return out;
}

}  // namespace bag2mkv
