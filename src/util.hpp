#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bag2mkv {

std::vector<uint8_t> read_file(const std::string &path);
std::string base64_encode(const uint8_t *data, size_t size);
std::vector<uint8_t> base64_decode(const std::string &text);
std::string to_hex(const uint8_t *data, size_t size);
std::string json_escape(const std::string &s);
std::string sha256_hex(const uint8_t *data, size_t size);
void swap16_copy(const uint8_t *src, size_t size, std::vector<uint8_t> &dst);
std::string format_float(float v);
std::string format_double(double v);

}  // namespace bag2mkv
