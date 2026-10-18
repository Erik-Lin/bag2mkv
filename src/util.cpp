#include "util.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace bag2mkv {

std::vector<uint8_t> read_file(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open file: " + path);
    }
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    if (size < 0) {
        throw std::runtime_error("cannot determine file size: " + path);
    }
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!data.empty()) {
        in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!in) {
            throw std::runtime_error("cannot read file: " + path);
        }
    }
    return data;
}

std::string base64_encode(const uint8_t *data, size_t size) {
    static const char *kTable = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= size) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | uint32_t(data[i + 2]);
        out.push_back(kTable[(v >> 18) & 63]);
        out.push_back(kTable[(v >> 12) & 63]);
        out.push_back(kTable[(v >> 6) & 63]);
        out.push_back(kTable[v & 63]);
        i += 3;
    }
    if (i < size) {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < size) {
            v |= uint32_t(data[i + 1]) << 8;
        }
        out.push_back(kTable[(v >> 18) & 63]);
        out.push_back(kTable[(v >> 12) & 63]);
        out.push_back(i + 1 < size ? kTable[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::vector<uint8_t> base64_decode(const std::string &text) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(text.size() * 3 / 4);
    uint32_t accumulator = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=') break;
        int v = value(c);
        if (v < 0) {
            if (std::isspace(static_cast<unsigned char>(c))) continue;
            throw std::runtime_error("invalid base64 input");
        }
        accumulator = (accumulator << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xFF));
        }
    }
    return out;
}

std::string to_hex(const uint8_t *data, size_t size) {
    static const char *kHex = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 15]);
    }
    return out;
}

std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    return out;
}

std::string sha256_hex(const uint8_t *data, size_t size) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0) {
        throw std::runtime_error("BCryptOpenAlgorithmProvider(SHA256) failed");
    }
    DWORD object_size = 0;
    DWORD result_size = 0;
    status = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                               &result_size, 0);
    if (status < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCryptGetProperty(SHA256 object length) failed");
    }
    std::vector<uint8_t> object(object_size);
    status = BCryptCreateHash(alg, &hash, object.data(), static_cast<ULONG>(object.size()), nullptr, 0, 0);
    if (status < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCryptCreateHash(SHA256) failed");
    }
    status = BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0);
    if (status < 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCryptHashData(SHA256) failed");
    }
    std::array<uint8_t, 32> digest{};
    status = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (status < 0) {
        throw std::runtime_error("BCryptFinishHash(SHA256) failed");
    }
    return to_hex(digest.data(), digest.size());
}

void swap16_copy(const uint8_t *src, size_t size, std::vector<uint8_t> &dst) {
    if (size % 2 != 0) {
        throw std::runtime_error("16-bit byte swap requires an even buffer size");
    }
    dst.resize(size);
    for (size_t i = 0; i < size; i += 2) {
        dst[i] = src[i + 1];
        dst[i + 1] = src[i];
    }
}

std::string format_float(float v) {
    std::ostringstream oss;
    oss << std::setprecision(9) << v;
    return oss.str();
}

std::string format_double(double v) {
    std::ostringstream oss;
    oss << std::setprecision(17) << v;
    return oss.str();
}

}  // namespace bag2mkv
