#include "bz2_util.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <bzlib.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace bag2mkv {

std::vector<uint8_t> bz2_decompress(const std::vector<uint8_t> &src) {
    if (src.empty()) {
        throw std::runtime_error("empty bzip2 chunk");
    }

    bz_stream stream{};
    int rc = BZ2_bzDecompressInit(&stream, 0, 0);
    if (rc != BZ_OK) {
        throw std::runtime_error("BZ2_bzDecompressInit failed: " + std::to_string(rc));
    }

    std::vector<uint8_t> out(std::max<size_t>(1u << 20, src.size() * 4));
    size_t src_pos = 0;
    size_t dst_pos = 0;
    bool done = false;

    try {
        while (true) {
            if (src_pos < src.size() && stream.avail_in == 0) {
                const size_t remaining = src.size() - src_pos;
                const size_t piece = std::min<size_t>(
                    remaining, static_cast<size_t>(std::numeric_limits<unsigned int>::max()));
                stream.next_in = const_cast<char *>(reinterpret_cast<const char *>(src.data() + src_pos));
                stream.avail_in = static_cast<unsigned int>(piece);
            }

            const size_t in_before = stream.avail_in;
            const size_t out_before = stream.avail_out;
            stream.next_out = reinterpret_cast<char *>(out.data() + dst_pos);
            const size_t out_capacity = std::min<size_t>(
                out.size() - dst_pos, static_cast<size_t>(std::numeric_limits<unsigned int>::max()));
            stream.avail_out = static_cast<unsigned int>(out_capacity);

            rc = BZ2_bzDecompress(&stream);
            src_pos += in_before - stream.avail_in;
            dst_pos += out_capacity - stream.avail_out;

            if (rc == BZ_STREAM_END) {
                done = true;
                break;
            }
            if (rc != BZ_OK) {
                throw std::runtime_error("BZ2_bzDecompress failed: " + std::to_string(rc));
            }

            if (stream.avail_out == 0) {
                out.resize(out.size() * 2);
                continue;
            }
            if (in_before == stream.avail_in && out_before == stream.avail_out) {
                throw std::runtime_error("bzip2 decompressor made no progress (truncated stream?)");
            }
            if (src_pos == src.size() && stream.avail_in == 0) {
                throw std::runtime_error("bzip2 chunk ended before end-of-stream");
            }
        }
    } catch (...) {
        BZ2_bzDecompressEnd(&stream);
        throw;
    }

    BZ2_bzDecompressEnd(&stream);
    if (!done) {
        throw std::runtime_error("bzip2 chunk ended unexpectedly");
    }
    if (src_pos != src.size() || stream.avail_in != 0) {
        throw std::runtime_error("trailing bytes after bzip2 stream");
    }
    out.resize(dst_pos);
    return out;
}

}  // namespace bag2mkv
